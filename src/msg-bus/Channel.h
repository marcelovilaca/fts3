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

#ifndef MSG_FACTORY_H
#define MSG_FACTORY_H

#include <zmq.hpp>
#include <memory>
#include <google/protobuf/message.h>
#include <boost/filesystem/path.hpp>


namespace fts3 {
namespace events {

/// URL Copy Ping Messages go to this channel
const std::string UrlCopyPingChannel("url_copy-ping.ipc");

/// Consumer wraps the receiver end of a channel
/// @see ChannelFactory
class Consumer {
private:
    friend class ChannelFactory;
    zmq::socket_t zmqSocket;

    Consumer(zmq::socket_t && socket);

public:
    /// Receive a message from the channel
    /// @param msg      Where to deserialize the message
    /// @param block    If true, this method will block until there is something to consume
    /// @return         true if something has been consumed
    bool receive(google::protobuf::Message *msg, bool block=true);
};

/// Producer wraps the producer end of a channel
/// @see ChannelFactory
class Producer {
private:
    friend class ChannelFactory;
    zmq::socket_t zmqSocket;

    Producer(zmq::socket_t && socket);

public:
    /// Send a message into the channel
    /// @param msg      The message to send
    /// @param block    If true, this method will block if the message can not be sent right away
    /// @return         true if the message has been sent
    bool send(const google::protobuf::Message &msg, bool block=true);
};

/// ChannelFactory facilitates the creation of consumer and producer ends
class ChannelFactory {
private:
    zmq::context_t zmqContext;
    boost::filesystem::path baseDir;

public:
    /// Constructor
    /// @param baseDir Where the sockets are to be created
    ChannelFactory(const std::string &baseDir);

    /// Destructor
    ~ChannelFactory();

    /// Create a new producer
    /// @param name     The channel name
    /// @param listen   If true, the producer will bind to the socket. If false, it will connect.
    /// @note           Make sure either the consumer or producer listens
    std::unique_ptr<Producer> createProducer(const std::string &name, bool listen = false);

    /// Create a new consumer
    /// @param name     The channel name
    /// @param listen   If true, the consumer will bind to the socket. If false, it will connect.
    /// @note           Make sure either the consumer or producer listens
    std::unique_ptr<Consumer> createConsumer(const std::string &name, bool listen = true);
};

} // end namespace msg
} // end namespace fts3

#endif // MSG_FACTORY_H

