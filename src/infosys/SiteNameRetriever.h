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

#ifndef SITENAMERETRIEVER_H_
#define SITENAMERETRIEVER_H_

#include <map>
#include <string>

#include <boost/thread.hpp>

#include "BdiiBrowser.h"

#ifndef WITHOUT_PUGI
#  include "OsgParser.h"
#  include "BdiiCacheParser.h"
#endif

#include "../common/Singleton.h"

namespace fts3
{
namespace infosys
{

/**
 *
 */
class SiteNameRetriever: public Singleton<SiteNameRetriever>
{

    friend class Singleton<SiteNameRetriever>;

public:

    virtual ~SiteNameRetriever();

    std::string getSiteName(std::string se);

private:

    SiteNameRetriever() {};
    SiteNameRetriever(SiteNameRetriever const&);
    SiteNameRetriever& operator=(SiteNameRetriever const&);

    // glue1 attributes
    static const char* ATTR_GLUE1_SERVICE;
    static const char* ATTR_GLUE1_SERVICE_URI;
    static const char* ATTR_GLUE1_LINK;
    static const char* ATTR_GLUE1_SITE;
    static const char* ATTR_GLUE1_HOSTINGORG;

    static const std::string FIND_SE_SITE_GLUE1(std::string se);
    static const char* FIND_SE_SITE_ATTR_GLUE1[];

    // glue2 attribute
    static const char* ATTR_GLUE2_SERVICE;
    static const char* ATTR_GLUE2_SITE;

    static const std::string FIND_SE_SITE_GLUE2(std::string se);
    static const char* FIND_SE_SITE_ATTR_GLUE2[];

    std::string getFromBdii(std::string se);

    boost::mutex m;
    std::map<std::string, std::string> seToSite;
};

} /* namespace infosys */
} /* namespace fts3 */
#endif /* SITENAMERETRIEVER_H_ */
