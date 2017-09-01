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

#include "MessageProcessingService.h"

#include <glib.h>
#include <boost/filesystem.hpp>

#include "common/Exceptions.h"
#include "config/ServerConfig.h"
#include "common/Logger.h"
#include "db/generic/SingleDbInstance.h"
#include "SingleTrStateInstance.h"
#include "ThreadSafeList.h"


using namespace fts3::common;
using fts3::config::ServerConfig;


namespace fts3 {
namespace server {

extern time_t updateRecords;


MessageProcessingService::MessageProcessingService(): BaseService("MessageProcessingService"),
    consumer(ServerConfig::instance().get<std::string>("MessagingDirectory")),
    msgFactory(ServerConfig::instance().get<std::string>("MessagingDirectory"))
{
    messages.reserve(600);
}


MessageProcessingService::~MessageProcessingService()
{
}


void MessageProcessingService::runService()
{
    namespace fs = boost::filesystem;

    auto msgCheckInterval = config::ServerConfig::instance().get<boost::posix_time::time_duration>("MessagingConsumeInterval");
    auto logProducer = msgFactory.createProducer(events::UrlCopyLogChannel);
    auto statusProducer = msgFactory.createProducer(events::UrlCopyStatusChannel);

    while (!boost::this_thread::interruption_requested())
    {
        updateRecords = time(0);

        try
        {
            if (boost::this_thread::interruption_requested() && messages.empty() && messagesLog.empty())
            {
                break;
            }

            // update statuses
            if (consumer.runConsumerStatus(messages) != 0)
            {
                char buffer[128] = {0};
                FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Could not get the status messages:" << strerror_r(errno, buffer, sizeof(buffer)) << commit;
                continue;
            }

            for (auto i = messages.begin(); i != messages.end(); ++i) {
                statusProducer->send(*i);
            }
            messages.clear();

            // update log file path
            if (consumer.runConsumerLog(messagesLog) != 0)
            {
                char buffer[128] = { 0 };
                FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Could not get the log messages:" << strerror_r(errno, buffer, sizeof(buffer)) << commit;
                continue;
            }

            for (auto i = messagesLog.begin(); i != messagesLog.end(); ++i) {
                logProducer->send(*i);
            }
        }
        catch (const std::exception& e) {
            FTS3_COMMON_LOGGER_NEWLOG(ERR) << e.what() << commit;
        }
        catch (...) {
            FTS3_COMMON_LOGGER_NEWLOG(ERR) << "Message queue thrown unhandled exception" << commit;
        }
        boost::this_thread::sleep(msgCheckInterval);
    }
}

} // end namespace server
} // end namespace fts3
