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

#include "Channel.h"

namespace fts3 {
namespace events {


Consumer::Consumer(zmq::socket_t && socket): zmqSocket(std::move(socket))
{
}


bool Consumer::receive(google::protobuf::Message *msg, bool block)
{
    int flags = block ? 0:ZMQ_NOBLOCK;
    zmq::message_t message;
    if (!zmqSocket.recv(&message, flags)) {
        return false;
    }
    msg->ParseFromArray(message.data(), message.size());
    return true;
}


Producer::Producer(zmq::socket_t && socket): zmqSocket(std::move(socket))
{

}


bool Producer::send(const google::protobuf::Message &msg, bool block)
{
    int flags = block ? 0:ZMQ_NOBLOCK;
    std::string serialized = msg.SerializeAsString();
    zmq::message_t message(serialized.size());
    memcpy(message.data(), serialized.c_str(), serialized.size());
    return zmqSocket.send(message, flags);
}


ChannelFactory::ChannelFactory(const std::string &baseDir): zmqContext(1), baseDir(baseDir)
{
}


ChannelFactory::~ChannelFactory()
{
}


std::unique_ptr<Producer> ChannelFactory::createProducer(const std::string &name, bool listen)
{
    zmq::socket_t zmqSocket(zmqContext, ZMQ_PUSH);
    auto fullPath = baseDir / name;
    std::string address = std::string("ipc://") + fullPath.native();

    if (listen) {
        zmqSocket.bind(address.c_str());
    }
    else {
        zmqSocket.connect(address.c_str());
    }

    return std::unique_ptr<Producer>(new Producer{std::move(zmqSocket)});
}


std::unique_ptr<Consumer> ChannelFactory::createConsumer(const std::string &name, uint64_t limit, bool listen)
{
    zmq::socket_t zmqSocket(zmqContext, ZMQ_PULL);
    auto fullPath = baseDir / name;
    std::string address = std::string("ipc://") + fullPath.native();

    if (listen) {
        zmqSocket.bind(address.c_str());
    }
    else {
        zmqSocket.connect(address.c_str());
    }
    zmqSocket.setsockopt(ZMQ_HWM, &limit, sizeof(limit));
    return std::unique_ptr<Consumer>(new Consumer{std::move(zmqSocket)});
}


Poller::Poller()
{
}


void Poller::add(Consumer *consumer, CallbackFunction callback)
{
    zmq::pollitem_t pollItem;
    pollItem.events = ZMQ_POLLIN;
    pollItem.socket = static_cast<void*>(consumer->zmqSocket);
    zmqPollItems.push_back(pollItem);
    consumers.emplace_back(std::make_pair<Consumer*, CallbackFunction>(consumer, callback));
}


bool Poller::poll(boost::posix_time::time_duration timeout)
{
    zmq::poll(zmqPollItems.data(), zmqPollItems.size(), timeout.total_microseconds());
    bool ret = false;

    for (int i = 0; i < zmqPollItems.size(); ++i) {
        if (zmqPollItems[i].revents & ZMQ_POLLIN) {
            Consumer *consumer = consumers[i].first;
            CallbackFunction &callback = consumers[i].second;
            callback(consumer);
            ret = true;
        }
    }

    return ret;
}

} // end namespace msg
} // end namespace fts3
