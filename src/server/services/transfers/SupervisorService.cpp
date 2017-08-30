/*
 * Copyright (c) CERN 2017
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

#include "SupervisorService.h"
#include "config/ServerConfig.h"
#include "db/generic/SingleDbInstance.h"
#include "ThreadSafeList.h"
#include <msg-bus/events.h>

using namespace fts3::common;


namespace fts3 {
namespace server {


SupervisorService::SupervisorService(): BaseService("SupervisorService"),
    msgFactory(config::ServerConfig::instance().get<std::string>("MessagingDirectory"))
{
}


static void pingCallback(events::Consumer *consumer)
{
    fts3::events::MessageUpdater event;
    if (!consumer->receive(&event)) {
        return;
    }

    FTS3_COMMON_LOGGER_NEWLOG(INFO)
        << "Process Updater Monitor "
        << "\nJob id: " << event.job_id()
        << "\nFile id: " << event.file_id()
        << "\nPid: " << event.process_id()
        << "\nTimestamp: " << event.timestamp()
        << "\nThroughput: " << event.throughput()
        << "\nTransferred: " << event.transferred()
        << commit;
    ThreadSafeList::get_instance().updateMsg(event);

    std::vector<fts3::events::MessageUpdater> events{event};
    db::DBSingleton::instance().getDBObjectInstance()->updateFileTransferProgressVector(events);
}


void SupervisorService::runService()
{
    std::unique_ptr<events::Consumer> consumer = msgFactory.createConsumer(events::UrlCopyPingChannel);

    events::Poller poller;
    poller.add(consumer.get(), events::Poller::CallbackFunction(pingCallback));

    while (!boost::this_thread::interruption_requested()) {
        try {
            poller.poll(boost::posix_time::seconds(1));
        }
        catch (const boost::thread_interrupted&) {
            FTS3_COMMON_LOGGER_NEWLOG(INFO) << "Thread interruption requested" << commit;
            break;
        }
        catch (const std::exception &error) {
            FTS3_COMMON_LOGGER_NEWLOG(ERR) << error.what() << commit;
        }
    }
}

}
}