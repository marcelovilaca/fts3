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

#ifndef MSGOUTBOUNDTCP_H
#define MSGOUTBOUNDTCP_H

#include <string>
#include <zmq.hpp>

class MsgOutboundTCP {
private:
    zmq::context_t &zmqContext;
    std::string bindAddress;

public:
    MsgOutboundTCP(zmq::context_t &zmqContext, const std::string &bindAddress);
    ~MsgOutboundTCP();

    void run();
};


#endif // MSGOUTBOUNDTCP_H
