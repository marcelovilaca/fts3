/*
 * Copyright (c) CERN 2013-2015
 *
 * Copyright (c) Members of the EMI Collaboration. 2010-2013
 *  See  http://www.eu-emi.eu/partners for details on the copyright
 *  holders.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdlib>
#include <activemq/library/ActiveMQCPP.h>
#include <common/PidTools.h>
#include <common/panic.h>
#include <signal.h>

#include "common/DaemonTools.h"
#include "common/Exceptions.h"
#include "common/Logger.h"
#include "config/ServerConfig.h"

#include "BrokerConfig.h"
#include "MsgInbound.h"
#include "MsgOutboundStomp.h"

using namespace fts3::common;
using namespace fts3::config;

bool stopThreads = false;


static void shutdown_callback(int signum, void*)
{
    FTS3_COMMON_LOGGER_NEWLOG(WARNING) << "Caught signal " << signum
                                       << " (" << strsignal(signum) << ")" << commit;
    FTS3_COMMON_LOGGER_NEWLOG(INFO) << "Future signals will be ignored!" << commit;
    FTS3_COMMON_LOGGER_NEWLOG(INFO) << "Messaging server stopping" << commit;
    stopThreads = true;

    // Some require traceback
    switch (signum)
    {
        case SIGABRT: case SIGSEGV: case SIGILL:
        case SIGFPE: case SIGBUS: case SIGTRAP:
        case SIGSYS:
            FTS3_COMMON_LOGGER_NEWLOG(CRIT)<< "Stack trace: \n"
               << panic::stack_dump(panic::stack_backtrace, panic::stack_backtrace_size)
               << commit;
            break;
        default:
            break;
    }
}



static void DoServer(bool isDaemon) throw()
{
    try {
        BrokerConfig config(ServerConfig::instance().get<std::string>("MonitoringConfigFile"));
        const std::string monitoringMsgDir = ServerConfig::instance().get<std::string>("MessagingDirectory");
        const std::string logFile = config.GetLogFilePath();

        if (isDaemon) {
            if (theLogger().redirect(logFile, logFile) != 0) {
                FTS3_COMMON_LOGGER_LOG(CRIT, "Could not open the log file");
                return;
            }
        }

        panic::setup_signal_handlers(shutdown_callback, NULL);

        theLogger().setLogLevel(Logger::getLogLevel(ServerConfig::instance().get<std::string>("LogLevel")));

        activemq::library::ActiveMQCPP::initializeLibrary();

        // Shared ZeroMQ context
        zmq::context_t zmqContext(0);

        MsgInbound pipeMsg(monitoringMsgDir, zmqContext);
        MsgOutboundStomp stompProducer(zmqContext, config);

        boost::thread_group threads;
        threads.add_thread(
            new boost::thread(boost::bind(&MsgInbound::run, &pipeMsg))
        );
        threads.add_thread(
            new boost::thread(boost::bind(&MsgOutboundStomp::run, &stompProducer))
        );

        FTS3_COMMON_LOGGER_LOG(INFO, "Threads started");

        threads.join_all();
        FTS3_COMMON_LOGGER_LOG(INFO, "Threads exited");
    }
    catch (cms::CMSException& e) {
        std::string errorMessage = "PROCESS_ERROR " + e.getStackTraceString();
        FTS3_COMMON_LOGGER_LOG(ERR, errorMessage);
    }
    catch (const std::exception& e) {
        std::string  errorMessage = "PROCESS_ERROR " + std::string(e.what());
        FTS3_COMMON_LOGGER_LOG(ERR, errorMessage);
    }
    catch (...) {
        FTS3_COMMON_LOGGER_LOG(ERR, "PROCESS_ERROR Unknown exception");
    }

    activemq::library::ActiveMQCPP::shutdownLibrary();
}


static void spawnServer(int argc, char **argv)
{
    ServerConfig::instance().read(argc, argv);
    std::string user = ServerConfig::instance().get<std::string>("User");
    std::string group = ServerConfig::instance().get<std::string>("Group");
    std::string pidDir = ServerConfig::instance().get<std::string>("PidDirectory");

    if (dropPrivileges(user, group)) {
        FTS3_COMMON_LOGGER_NEWLOG(DEBUG) << "Changed running user and group to " << user << ":" << group << commit;
    }

    bool isDaemon = !ServerConfig::instance().get<bool> ("no-daemon");
    if (isDaemon) {
        int d = daemon(0, 0);
        if (d < 0) {
            throw SystemError("Can't set daemon");
        }
    }

    createPidFile(pidDir, "fts-msg-bulk.pid");

    DoServer(argc <= 1);
}


int main(int argc, char **argv)
{
    // Do not even try if already running
    int n_running = countProcessesWithName("fts_msg_bulk");
    if (n_running < 0) {
        std::cerr << "Could not check if FTS3 is already running" << std::endl;
        return EXIT_FAILURE;
    }
    else if (n_running > 1) {
        std::cerr << "Only 1 instance of FTS3 messaging daemon can run at a time" << std::endl;
        return EXIT_FAILURE;
    }

    try {
        spawnServer(argc, argv);
    }
    catch (const std::exception& ex) {
        std::cerr << "Failed to spawn the messaging server! " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
    catch (...) {
        std::cerr << "Failed to spawn the messaging server! Unknown exception" << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
