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

#include "MsgPipe.h"

#include <fstream>
#include <signal.h>
#include <iostream>
#include <boost/filesystem.hpp>

#include "common/Logger.h"
#include "common/ConcurrentQueue.h"

extern bool stopThreads;
static bool signalReceived = false;

using fts3::common::ConcurrentQueue;
namespace fs = boost::filesystem;

void handler(int)
{
    if (!signalReceived) {
        signalReceived = true;
        stopThreads = true;
        sleep(5);
        exit(0);
    }
}


MsgPipe::MsgPipe(const std::string &baseDir):
    monitoringQueue(new DirQ(baseDir+"/monitoring"))
{
    //register sig handler to cleanup resources upon exiting
    signal(SIGFPE, handler);
    signal(SIGILL, handler);
    signal(SIGSEGV, handler);
    signal(SIGBUS, handler);
    signal(SIGABRT, handler);
    signal(SIGTERM, handler);
    signal(SIGINT, handler);
    signal(SIGQUIT, handler);
}


MsgPipe::~MsgPipe()
{
}


int MsgPipe::consume(std::vector<std::string> &messages)
{
    std::string content;

    const char *error = NULL;
    dirq_clear_error(*monitoringQueue);

    unsigned i = 0;
    for (auto iter = dirq_first(*monitoringQueue); iter != NULL && i < 1000; iter = dirq_next(*monitoringQueue), ++i) {
        if (dirq_lock(*monitoringQueue, iter, 0) == 0) {
            const char *path = dirq_get_path(*monitoringQueue, iter);

            try {
                std::ifstream fstream(path);
                content.assign((std::istreambuf_iterator<char>(fstream)), std::istreambuf_iterator<char>());
                messages.emplace_back(content);
            }
            catch (const std::exception &ex) {
                FTS3_COMMON_LOGGER_NEWLOG(ERR)
                    << "Could not load message from " << path << " (" << ex.what() << ")"
                    << fts3::common::commit;
            }


            if (dirq_remove(*monitoringQueue, iter) < 0) {
                error = dirq_get_errstr(*monitoringQueue);
                FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Failed to remove message from queue (" << path << "): "
                                               << error
                                               << fts3::common::commit;
                dirq_clear_error(*monitoringQueue);
            }
        }
    }

    error = dirq_get_errstr(*monitoringQueue);
    if (error) {
        FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Failed to consume messages: " << error << fts3::common::commit;
        return -1;
    }

    return 0;
}


void MsgPipe::run()
{
    std::vector<std::string> messages;

    while (stopThreads == false) {
        try {
            int returnValue = consume(messages);
            if (returnValue != 0) {
                std::ostringstream errorMessage;
                errorMessage << "runConsumerMonitoring returned " << returnValue;
                FTS3_COMMON_LOGGER_LOG(ERR, errorMessage.str());
            }

            for (auto iter = messages.begin(); iter != messages.end(); ++iter) {
                ConcurrentQueue::getInstance()->push(*iter);
            }
            messages.clear();
        }
        catch (const fs::filesystem_error &ex) {
            FTS3_COMMON_LOGGER_LOG(ERR, ex.what());
        }
        catch (...) {
            FTS3_COMMON_LOGGER_LOG(CRIT, "Unexpected exception");
        }
        sleep(1);
    }
}
