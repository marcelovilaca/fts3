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
#include "SingleTrStateInstance.h"
#include <msg-bus/events.h>

using namespace fts3::common;


namespace fts3 {
namespace server {


SupervisorService::SupervisorService(): BaseService("SupervisorService"),
    msgFactory(config::ServerConfig::instance().get<std::string>("MessagingDirectory"))
{
}

/// Handle ping messages
static void pingCallback(events::Consumer *consumer)
{
    events::MessageUrlCopyPing event;
    while (consumer->receive(&event, false)) {
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

        db::DBSingleton::instance().getDBObjectInstance()->updateFileTransferProgress(event);
    }
}

/// Handle updates for the protocol data
static void handleProtocolUpdate(const events::MessageUrlCopy &event)
{
    FTS3_COMMON_LOGGER_NEWLOG(DEBUG)
        << "Protocol update for " << event.job_id() << "/" << event.file_id() << commit;

    db::DBSingleton::instance().getDBObjectInstance()->updateProtocol(event);
}

/// Handle new states of a transfer
static void handleStatusUpdate(const events::MessageUrlCopy &event)
{
    auto db = db::DBSingleton::instance().getDBObjectInstance();

    FTS3_COMMON_LOGGER_NEWLOG(INFO)
        << "Job id:" << event.job_id()
        << "\nFile id: " << event.file_id()
        << "\nPid: " << event.process_id()
        << "\nState: " << event.transfer_status()
        << "\nSource: " << event.source_se()
        << "\nDest: " << event.dest_se() << commit;

    if (event.transfer_status() == "FINISHED" ||
        event.transfer_status() == "FAILED" ||
        event.transfer_status() == "CANCELED") {
        FTS3_COMMON_LOGGER_NEWLOG(INFO)
            << "Removing job from monitoring list " << event.job_id() << " " << event.file_id()
            << commit;
        ThreadSafeList::get_instance().removeFinishedTr(event.job_id(), event.file_id());
    }

    // Move the transfer to SUBMITTED again if can be retried
    if(event.transfer_status() == "FAILED")
    {
        //multiple replica files belonging to a job will not be retried
        int retry = db->getRetry(event.job_id());

        if(event.retry() && retry > 0 && event.file_id() > 0)
        {
            int retryTimes = db->getRetryTimes(event.job_id(), event.file_id());
            if (retryTimes <= retry - 1)
            {
                db->setRetryTransfer(
                    event.job_id(), event.file_id(), retryTimes+1, event.transfer_message(), event.errcode());
                return;
            }
        }
    }

    // Update file/job state
    boost::tuple<bool, std::string> updated = db->updateTransferStatus(
        event.job_id(), event.file_id(), event.throughput(), event.transfer_status(),
        event.transfer_message(), event.process_id(), event.filesize(), event.time_in_secs(), event.retry()
    );
    if (!event.log_path().empty()) {
        db->transferLogFile(event.file_id(), event.log_path(), event.has_debug_file());
    }

    db->updateJobStatus(event.job_id(), event.transfer_status());

    if (!updated.get<0>() && event.transfer_status() != "CANCELED") {
        FTS3_COMMON_LOGGER_NEWLOG(ERR)
            << "Entry in the database not updated for "
            << event.job_id() << " " << event.file_id()
            << ". Probably already in a different terminal state. Tried to set "
            << event.transfer_status() << " over " << updated.get<1>() << commit;
    }
    else if (!event.job_id().empty() && event.file_id() > 0) {
        SingleTrStateInstance::instance().sendStateMessage(event.job_id(), event.file_id());
    }
}

/// Handle status messages
static void statusCallback(events::Consumer *consumer)
{
    events::MessageUrlCopy event;
    while (consumer->receive(&event, false)) {
        if (event.transfer_status() == "UPDATE") {
            handleProtocolUpdate(event);
        }
        else {
            handleStatusUpdate(event);
        }
    }
}

/// Handle log messages
static void logCallback(events::Consumer *consumer)
{
    events::MessageLog event;
    while(consumer->receive(&event, false)) {
        FTS3_COMMON_LOGGER_NEWLOG(DEBUG)
            << "Log for " << event.job_id() << "/" << event.file_id()
            << ": " << event.log_path()
            << commit;
        db::DBSingleton::instance().getDBObjectInstance()->transferLogFile(
            event.file_id(), event.log_path(), event.has_debug_file()
        );
    }
}

void SupervisorService::runService()
{
    auto pingConsumer = msgFactory.createConsumer(events::UrlCopyPingChannel);
    auto statusConsumer = msgFactory.createConsumer(events::UrlCopyStatusChannel);
    auto logConsumer = msgFactory.createConsumer(events::UrlCopyLogChannel);

    events::Poller poller;
    poller.add(pingConsumer.get(), events::Poller::CallbackFunction(pingCallback));
    poller.add(statusConsumer.get(), events::Poller::CallbackFunction(statusCallback));
    poller.add(logConsumer.get(), events::Poller::CallbackFunction(logCallback));

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
