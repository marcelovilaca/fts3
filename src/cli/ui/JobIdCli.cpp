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

#include "JobIdCli.h"

using namespace fts3::cli;

JobIdCli::JobIdCli()
{

    // add hidden options (not printed in help)
    hidden.add_options()
    ("jobid", po::value< std::vector<std::string> >()->multitoken(), "Specify job ID.")
    ;

    // all positional parameters go to jobid
    p.add("jobid", -1);
}

JobIdCli::~JobIdCli()
{
}


std::string JobIdCli::getUsageString(std::string tool) const
{
    return "Usage: " + tool + " [options] JOBID [JOBID...]";
}

std::vector<std::string> JobIdCli::getJobIds()
{

    // check whether jobid has been given as a parameter
    if (vm.count("jobid"))
        {
            return vm["jobid"].as< std::vector<std::string> >();
        }

    return std::vector<std::string>();
}
