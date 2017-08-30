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

#include <fstream>
#include <common/DirQ.h>
#include <common/Logger.h>
#include "consumer.h"


Consumer::Consumer(const std::string &baseDir, unsigned limit):
    baseDir(baseDir), limit(limit),
    monitoringQueue(new DirQ(baseDir + "/monitoring")), statusQueue(new DirQ(baseDir + "/status")),
    logQueue(new DirQ(baseDir + "/logs"))
{
}


Consumer::~Consumer()
{
}


template <typename MSG>
static int genericConsumer(std::unique_ptr<DirQ> &dirq, unsigned limit, std::vector<MSG> &messages)
{
    MSG event;

    const char *error = NULL;
    dirq->clearError();

    unsigned i = 0;
    for (auto iter = dirq->begin(); iter != dirq->end() && i < limit; ++iter, ++i) {
        if (iter.lock()) {
            const char *path = iter.get();

            try {
                std::ifstream fstream(path);
                event.ParseFromIstream(&fstream);
            }
            catch (const std::exception &ex) {
                FTS3_COMMON_LOGGER_NEWLOG(ERR)
                    << "Could not load message from " << path << " (" << ex.what() << ")"
                    << fts3::common::commit;
            }

            messages.emplace_back(event);
            if (!iter.remove()) {
                error = dirq->errstr();
                FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Failed to remove message from queue (" << path << "): "
                    << error
                    << fts3::common::commit;
                dirq->clearError();
            }
        }
    }

    error = dirq->errstr();
    if (error) {
        FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Failed to consume messages: " << error << fts3::common::commit;
        return -1;
    }

    return 0;
}


int Consumer::runConsumerStatus(std::vector<fts3::events::MessageUrlCopy> &messages)
{
    return genericConsumer<fts3::events::MessageUrlCopy>(statusQueue, limit, messages);
}


int Consumer::runConsumerLog(std::vector<fts3::events::MessageLog> &messages)
{
    fts3::events::MessageLog buffer;

    const char *error = NULL;
    logQueue->clearError();

    unsigned i = 0;
    for (auto iter = logQueue->begin(); iter != logQueue->end() && i < limit; ++iter, ++i) {
        if (iter.lock()) {
            const char *path = iter.get();

            try {
                std::ifstream fstream(path);
                buffer.ParseFromIstream(&fstream);
            }
            catch (const std::exception &ex) {
                FTS3_COMMON_LOGGER_NEWLOG(ERR)
                << "Could not load message from " << path << " (" << ex.what() << ")"
                << fts3::common::commit;
            }

            messages.emplace_back(buffer);
            if (!iter.remove()) {
                error = logQueue->errstr();
                FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Failed to remove message from queue (" << path << "): "
                    << error
                    << fts3::common::commit;
                logQueue->clearError();
            }
        }
    }

    error = logQueue->errstr();
    if (error) {
        FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Failed to consume messages: " << error << fts3::common::commit;
        return -1;
    }

    return 0;
}


static void _purge(DirQ *dq)
{
    if (!dq->purge()) {
        FTS3_COMMON_LOGGER_NEWLOG(ERR)
            << "Could not purge " << dq->getPath() << " (" << dq->errstr() << ")"
            << fts3::common::commit;
    }
}


void Consumer::purgeAll()
{
    _purge(monitoringQueue.get());
    _purge(statusQueue.get());
    _purge(logQueue.get());
}
