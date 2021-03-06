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

#include "ConcurrentQueue.h"


namespace fts3 {
namespace common {


ConcurrentQueue *ConcurrentQueue::single = NULL;


ConcurrentQueue *ConcurrentQueue::getInstance()
{
    if (!single) {
        single = new ConcurrentQueue();
        return single;
    }
    else {
        return single;
    }
}


ConcurrentQueue::ConcurrentQueue()
{
}


std::string ConcurrentQueue::pop(int wait)
{
    boost::unique_lock<boost::mutex> lock(mutex);

    if (theQueue.empty()) {
        if (wait == 0) {
            return std::string();
        }
        if (!cv.timed_wait(lock, boost::posix_time::seconds(wait))) {
            return std::string();
        }
    }

    std::string ret = theQueue.front();
    theQueue.pop();
    return ret;
}


void ConcurrentQueue::push(const std::string &val)
{
    boost::lock_guard<boost::mutex> lock(mutex);
    if (theQueue.size() < ConcurrentQueue::MaxElements)
        theQueue.push(val);
    cv.notify_one();
}


size_t ConcurrentQueue::size()
{
    boost::lock_guard<boost::mutex> lock(mutex);
    return theQueue.size();
}


bool ConcurrentQueue::empty()
{
    boost::lock_guard<boost::mutex> lock(mutex);
    return theQueue.empty();
}

}
}
