/*
 * Copyright (c) CERN 2016
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

#include <boost/test/unit_test_suite.hpp>
#include <boost/test/test_tools.hpp>
#include <boost/filesystem.hpp>
#include <boost/function.hpp>

#include <msg-bus/Channel.h>
#include <msg-bus/events.h>
#include <boost/bind.hpp>

using namespace fts3::events;


namespace boost {
    bool operator==(const google::protobuf::Message &a, const google::protobuf::Message &b)
    {
        return a.SerializeAsString() == b.SerializeAsString();
    }
}


namespace std {
    std::ostream &operator<<(std::ostream &os, const google::protobuf::Message &msg)
    {
        os << msg.DebugString();
        return os;
    }
}


BOOST_AUTO_TEST_SUITE(MsgBusTest)


class MsgBusFixture {
protected:
    static const std::string TEST_PATH;
    ChannelFactory factory;

    std::vector<MessageUrlCopy> receivedStatus;
    std::vector<MessageUrlCopyPing> receivedPing;

public:
    MsgBusFixture(): factory(TEST_PATH) {
        boost::filesystem::create_directories(TEST_PATH);
    }

    ~MsgBusFixture() {
        boost::filesystem::remove_all(TEST_PATH);
    }

    void consumeStatus(void *data, size_t size) {
        MessageUrlCopy msg;
        msg.ParseFromArray(data, size);
        receivedStatus.emplace_back(msg);
    }

    void consumePing(void *data, size_t size) {
        MessageUrlCopyPing msg;
        msg.ParseFromArray(data, size);
        receivedPing.emplace_back(msg);
    }
};

const std::string MsgBusFixture::TEST_PATH("/tmp/MsgBusTest");


BOOST_FIXTURE_TEST_CASE (simpleStatus, MsgBusFixture)
{
    auto producer = factory.createProducer(UrlCopyStatusChannel);
    auto consumer = factory.createConsumer(UrlCopyStatusChannel);

    MessageUrlCopy original;
    original.set_job_id("1906cc40-b915-11e5-9a03-02163e006dd0");
    original.set_transfer_status("FAILED");
    original.set_transfer_message("TEST FAILURE, EVERYTHING IS TERRIBLE");
    original.set_source_se("mock://source/file");
    original.set_dest_se("mock://source/file2");
    original.set_file_id(42);
    original.set_process_id(1234);
    original.set_time_in_secs(55);
    original.set_filesize(1023);
    original.set_nostreams(33);
    original.set_timeout(22);
    original.set_buffersize(1);
    original.set_retry(5);
    original.set_retry(0.2);
    original.set_timestamp(15689);
    original.set_throughput(0.88);

    BOOST_CHECK(producer->send(original));

    // First attempt must return the single message
    Poller poller;
    poller.add(consumer.get(), boost::bind(&MsgBusFixture::consumeStatus, this, _1, _2));
    BOOST_CHECK(poller.poll(boost::posix_time::seconds(1)));

    BOOST_CHECK_EQUAL(receivedStatus[0], original);

    // Second attempt must return empty (already consumed)
    receivedStatus.clear();
    BOOST_CHECK(!poller.poll(boost::posix_time::milliseconds(10)));
    BOOST_CHECK_EQUAL(0, receivedStatus.size());
}


BOOST_FIXTURE_TEST_CASE (pollOne, MsgBusFixture)
{
    auto producerStatus = factory.createProducer(UrlCopyStatusChannel);
    auto producerPing = factory.createProducer(UrlCopyPingChannel);

    auto consumerStatus = factory.createConsumer(UrlCopyStatusChannel);
    auto consumerPing = factory.createConsumer(UrlCopyPingChannel);

    Poller poller;
    poller.add(consumerStatus.get(), boost::bind(&MsgBusFixture::consumeStatus, this, _1, _2));
    poller.add(consumerPing.get(), boost::bind(&MsgBusFixture::consumePing, this, _1, _2));

    MessageUrlCopy original;
    original.set_job_id("1906cc40-b915-11e5-9a03-02163e006dd0");
    original.set_transfer_status("FAILED");
    original.set_transfer_message("TEST FAILURE, EVERYTHING IS TERRIBLE");
    original.set_source_se("mock://source/file");
    original.set_dest_se("mock://source/file2");
    original.set_file_id(42);
    original.set_process_id(1234);
    original.set_time_in_secs(55);
    original.set_filesize(1023);
    original.set_nostreams(33);
    original.set_timeout(22);
    original.set_buffersize(1);
    original.set_retry(5);
    original.set_retry(0.2);
    original.set_timestamp(15689);
    original.set_throughput(0.88);
    BOOST_CHECK(producerStatus->send(original));

    BOOST_CHECK(poller.poll(boost::posix_time::seconds(1)));

    BOOST_CHECK_EQUAL(1, receivedStatus.size());
    BOOST_CHECK_EQUAL(0, receivedPing.size());
    BOOST_CHECK_EQUAL(original, receivedStatus[0]);
}


BOOST_FIXTURE_TEST_CASE (pollTwo, MsgBusFixture)
{
    auto producerStatus = factory.createProducer(UrlCopyStatusChannel);
    auto producerPing = factory.createProducer(UrlCopyPingChannel);

    auto consumerStatus = factory.createConsumer(UrlCopyStatusChannel);
    auto consumerPing = factory.createConsumer(UrlCopyPingChannel);

    Poller poller;
    poller.add(consumerStatus.get(), boost::bind(&MsgBusFixture::consumeStatus, this, _1, _2));
    poller.add(consumerPing.get(), boost::bind(&MsgBusFixture::consumePing, this, _1, _2));

    MessageUrlCopy original;
    original.set_job_id("1906cc40-b915-11e5-9a03-02163e006dd0");
    original.set_transfer_status("FAILED");
    original.set_transfer_message("TEST FAILURE, EVERYTHING IS TERRIBLE");
    original.set_source_se("mock://source/file");
    original.set_dest_se("mock://source/file2");
    original.set_file_id(42);
    original.set_process_id(1234);
    original.set_time_in_secs(55);
    original.set_filesize(1023);
    original.set_nostreams(33);
    original.set_timeout(22);
    original.set_buffersize(1);
    original.set_retry(5);
    original.set_retry(0.2);
    original.set_timestamp(15689);
    original.set_throughput(0.88);
    BOOST_CHECK(producerStatus->send(original));

    MessageUrlCopyPing originalPing;
    originalPing.set_job_id("7b747d24-8d8e-11e7-b4c3-02163e006dd0");
    originalPing.set_file_id(1234);
    originalPing.set_timestamp(15689);
    originalPing.set_process_id(1);
    originalPing.set_throughput(0.5);
    originalPing.set_transferred(4242);
    BOOST_CHECK(producerPing->send(originalPing));

    BOOST_CHECK(poller.poll(boost::posix_time::seconds(1)));

    BOOST_CHECK_EQUAL(1, receivedStatus.size());
    BOOST_CHECK_EQUAL(1, receivedPing.size());
    BOOST_CHECK_EQUAL(original, receivedStatus[0]);
    BOOST_CHECK_EQUAL(originalPing, receivedPing[0]);
}


BOOST_AUTO_TEST_SUITE_END()
