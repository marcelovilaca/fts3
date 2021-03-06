/*
 * Copyright (c) CERN 2015
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
#include <fstream>

#include "common/Logger.h"


BOOST_AUTO_TEST_SUITE(common)
BOOST_AUTO_TEST_SUITE(LoggerTest)


struct LoggerTestHelper
{
    mutable int counter;
    LoggerTestHelper(): counter(0) {}
};


std::ostream& operator << (std::ostream &os, const LoggerTestHelper &helper)
{
    ++helper.counter;
    os << "TEST";
    return os;
}


BOOST_AUTO_TEST_CASE(logLevel)
{
    fts3::common::Logger &logger = fts3::common::theLogger();
    logger.setLogLevel(fts3::common::Logger::WARNING);

    LoggerTestHelper helper;
    FTS3_COMMON_LOGGER_NEWLOG(WARNING) << helper << fts3::common::commit;
    BOOST_CHECK_EQUAL(helper.counter, 1);

    FTS3_COMMON_LOGGER_NEWLOG(DEBUG) << helper << fts3::common::commit;
    BOOST_CHECK_EQUAL(helper.counter, 1);

    FTS3_COMMON_LOGGER_NEWLOG(WARNING) << helper << fts3::common::commit;
    BOOST_CHECK_EQUAL(helper.counter, 2);

    FTS3_COMMON_LOGGER_NEWLOG(DEBUG) << helper << fts3::common::commit;
    BOOST_CHECK_EQUAL(helper.counter, 2);

    logger.setLogLevel(fts3::common::Logger::DEBUG);
    FTS3_COMMON_LOGGER_NEWLOG(DEBUG) << helper << fts3::common::commit;
    BOOST_CHECK_EQUAL(helper.counter, 3);
}


BOOST_AUTO_TEST_CASE(redirect)
{
    const std::string logPath("/tmp/fts3tests.log");

    try {
        boost::filesystem::remove(logPath);
    }
    catch (...) {
        // Ignore
    }

    // Save current stdout and stderr
    int oldOut = dup(STDOUT_FILENO);
    int oldErr = dup(STDERR_FILENO);
    BOOST_CHECK_GT(oldOut, -1);
    BOOST_CHECK_GT(oldErr, -1);

    // Do the test
    fts3::common::Logger &logger = fts3::common::theLogger();
    BOOST_CHECK_EQUAL(logger.redirect(logPath, logPath), 0);

    logger.setLogLevel(fts3::common::Logger::WARNING);
    FTS3_COMMON_LOGGER_NEWLOG(WARNING) << "APPEAR" << fts3::common::commit;
    FTS3_COMMON_LOGGER_NEWLOG(DEBUG) << "DOES NOT" << fts3::common::commit;

    std::ifstream read(logPath);

    bool appear = false;
    bool doesNot = false;

    while (!read.eof()) {
        char line[1024];
        read.getline(line, sizeof(line));
        if (strstr(line, "APPEAR")) {
            appear = true;
        }
        if (strstr(line, "DOES NOT")) {
            doesNot = true;
        }
    }

    BOOST_CHECK(appear);
    BOOST_CHECK(!doesNot);

    // Recover stdout and stderr
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    dup2(oldOut, STDOUT_FILENO);
    dup2(oldErr, STDERR_FILENO);
    close(oldOut);
    close(oldErr);

    // Clean
    BOOST_CHECK_NO_THROW(boost::filesystem::remove(logPath));
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()
