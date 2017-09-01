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

#ifndef FTS3_SUPERVISORSERVICE_H
#define FTS3_SUPERVISORSERVICE_H

#include <msg-bus/Channel.h>
#include "../BaseService.h"

namespace fts3 {
namespace server {

/**
 * The supervisor service consumes messages from fts_url_copy transfers.
 * Ping messages: After a given amount of time passes without signal of life, CancelerService will reap
 * those stalled processes
 * Log messages: Update on the DB the new location of the log file
 * Status messages: Transfer status transition
 */
class SupervisorService: public BaseService {
protected:
    events::ChannelFactory msgFactory;

public:
    /// Constructor
    SupervisorService();

    /// Service code
    void runService();
};

}
}

#endif //FTS3_SUPERVISORSERVICE_H
