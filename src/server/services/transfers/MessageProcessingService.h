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
 *
 * TO BE GONE
 * Kept alive to avoid draining on 3.8 update
 */

#pragma once
#ifndef PROCESSQUEUE_H_
#define PROCESSQUEUE_H_

#include <vector>
#include <msg-bus/Channel.h>

#include "msg-bus/consumer.h"
#include "../BaseService.h"

namespace fts3 {
namespace server {


class MessageProcessingService: public BaseService
{
private:
    std::vector<fts3::events::MessageUrlCopy> messages;
    std::vector<fts3::events::MessageLog> messagesLog;

    Consumer consumer;
    events::ChannelFactory msgFactory;

public:
    /// Constructor
    MessageProcessingService();

    /// Destructor
    virtual ~MessageProcessingService();

    virtual void runService();
};

} // end namespace server
} // end namespace fts3


#endif // PROCESSQUEUE_H_
