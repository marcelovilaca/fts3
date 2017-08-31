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

#ifndef DIRQ_H
#define DIRQ_H

#include <cstring>
#include <fstream>
#include <sstream>

#include <dirq.h>
#include <boost/filesystem.hpp>

#include "Exceptions.h"


class DirQ {
private:
    dirq_t dirq;
    std::string path;

public:
    class Iterator {
    private:
        friend class DirQ;

        dirq_t dirq;
        const char *i;

        Iterator(dirq_t dirq, const char *i): dirq(dirq), i(i) {}

    public:
        bool operator == (const Iterator &b) {
            return i == b.i || (i != NULL && b.i != NULL && strcmp(i, b.i));
        }

        bool operator != (const Iterator &b) {
            return !(*this == b);
        }

        void operator ++ () {
            i = dirq_next(dirq);
        }

        bool lock() {
            return dirq_lock(dirq, i, 0) == 0;
        }

        const char *get() {
            return dirq_get_path(dirq, i);
        };

        bool remove() {
            return dirq_remove(dirq, i) >= 0;
        }
    };


    DirQ(const std::string &path): path(path) {
        dirq = dirq_new(path.c_str());
        if (dirq_get_errcode(dirq)) {
            std::stringstream msg;
            msg << "Could not create dirq instance for " << path
                << "(" << dirq_get_errstr(dirq) << ")";
            throw fts3::common::SystemError(msg.str());
        }
    }

    ~DirQ() {
        dirq_free(dirq);
        dirq = NULL;
    }

    DirQ(const DirQ&) = delete;
    DirQ& operator = (DirQ&) = delete;

    const std::string &getPath(void) const throw() {
        return path;
    }

    void clearError() {
        dirq_clear_error(dirq);
    }

    Iterator begin() {
        return Iterator(dirq, dirq_first(dirq));
    }

    Iterator end() {
        return Iterator(dirq, NULL);
    }

    const char *errstr() {
        return dirq_get_errstr(dirq);
    }

    bool purge() {
        return dirq_purge(dirq) >= 0;
    }

    int send(std::string const &msg) {
        boost::filesystem::path temp = boost::filesystem::unique_path(path + "/%%%%-%%%%-%%%%-%%%%");

        std::ofstream fd(temp.native().c_str());
        if (!fd.good()) {
            return errno;
        }
        fd << msg;
        if (!fd.good()) {
            return errno;
        }
        fd.close();

        if (dirq_add_path(dirq, temp.native().c_str()) == NULL) {
            return dirq_get_errcode(dirq);
        }

        return 0;
    }
};

#endif //DIRQ_H
