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

#include <common/Logger.h>
#include "MsgOutboundTCP.h"
#include "MsgInbound.h"


MsgOutboundTCP::MsgOutboundTCP(zmq::context_t &zmqContext, const std::string &bindAddress):
    zmqContext(zmqContext), bindAddress(bindAddress)
{
}


MsgOutboundTCP::~MsgOutboundTCP()
{
}


void MsgOutboundTCP::run()
{
    zmq::socket_t subscribeSocket(zmqContext, ZMQ_SUB), publisherSocket(zmqContext, ZMQ_PUB);
    subscribeSocket.connect(SUBSCRIBE_SOCKET_ID);
    subscribeSocket.setsockopt(ZMQ_SUBSCRIBE, "", 0);
    publisherSocket.bind(("tcp://" + bindAddress).c_str());

    FTS3_COMMON_LOGGER_LOG(INFO, "Starting ZeroMQ forwarder");
    zmq_device(ZMQ_FORWARDER, static_cast<void*>(subscribeSocket), static_cast<void*>(publisherSocket));
    FTS3_COMMON_LOGGER_LOG(INFO, "ZeroMQ forwarder stopped");
}
