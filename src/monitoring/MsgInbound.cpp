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

#include "MsgInbound.h"

#include <fstream>
#include <iostream>
#include <boost/filesystem.hpp>

#include "common/Logger.h"

extern bool stopThreads;

namespace fs = boost::filesystem;


MsgInbound::MsgInbound(const std::string &fromDir, zmq::context_t &zmqContext):
    pullFromDirq(new DirQ(fromDir+"/monitoring")),
    publishSocket(zmqContext, ZMQ_PUB)
{
    publishSocket.bind(SUBSCRIBE_SOCKET_ID);
}


MsgInbound::~MsgInbound()
{
}


int MsgInbound::consume()
{
    const char *error = NULL;
    dirq_clear_error(*pullFromDirq);

    unsigned i = 0;
    for (auto iter = dirq_first(*pullFromDirq); iter != NULL && i < 1000; iter = dirq_next(*pullFromDirq), ++i) {
        if (dirq_lock(*pullFromDirq, iter, 0) == 0) {
            const char *path = dirq_get_path(*pullFromDirq, iter);

            try {
                std::string body;
                std::ifstream fstream(path);
                body.assign((std::istreambuf_iterator<char>(fstream)), std::istreambuf_iterator<char>());

                zmq::message_t msg(body.size());
                memcpy(msg.data(), body.data(), body.size());
                publishSocket.send(msg, 0);
            }
            catch (const std::exception &ex) {
                FTS3_COMMON_LOGGER_NEWLOG(ERR)
                    << "Could not load message from " << path << " (" << ex.what() << ")"
                    << fts3::common::commit;
            }


            if (dirq_remove(*pullFromDirq, iter) < 0) {
                error = dirq_get_errstr(*pullFromDirq);
                FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Failed to remove message from queue (" << path << "): "
                                               << error
                                               << fts3::common::commit;
                dirq_clear_error(*pullFromDirq);
            }
        }
    }

    error = dirq_get_errstr(*pullFromDirq);
    if (error) {
        FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Failed to consume messages: " << error << fts3::common::commit;
        return -1;
    }

    return 0;
}


void MsgInbound::run()
{
    while (stopThreads == false) {
        try {
            int returnValue = consume();
            if (returnValue != 0) {
                std::ostringstream errorMessage;
                errorMessage << "runConsumerMonitoring returned " << returnValue;
                FTS3_COMMON_LOGGER_LOG(ERR, errorMessage.str());
            }
        }
        catch (const fs::filesystem_error &ex) {
            FTS3_COMMON_LOGGER_LOG(ERR, ex.what());
        }
        catch (...) {
            FTS3_COMMON_LOGGER_LOG(CRIT, "Unexpected exception");
        }
        dirq_purge(*pullFromDirq.get());
        sleep(1);
    }
    FTS3_COMMON_LOGGER_LOG(INFO, "MsgInbound exiting");
}
