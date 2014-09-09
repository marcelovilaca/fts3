/*
 *	Copyright notice:
 *	Copyright © Members of the EMI Collaboration, 2010.
 *
 *	See www.eu-emi.eu for details on the copyright holders
 *
 *	Licensed under the Apache License, Version 2.0 (the "License");
 *	you may not use this file except in compliance with the License.
 *	You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 *
 * SetCfgCli.h
 *
 *  Created on: Apr 2, 2012
 *      Author: Michal Simon
 */

#ifndef NAMEVALUECLI_H_
#define NAMEVALUECLI_H_

#include "CliBase.h"

#include <vector>
#include <tuple>
#include <boost/optional.hpp>

#include "common/CfgParser.h"


namespace fts3
{
namespace cli
{

using namespace boost;
using namespace fts3::common;

/**
 * SetCfgCli provides the user interface for configuring SEs
 *
 * 		- cfg, positional parameter, the configuration in JSON format
 * 			corresponding to a SE (maybe given multiple times to configure
 * 			multiple SEs)
 *
 */
class SetCfgCli: public CliBase
{

public:

    /**
     * Default constructor.
     *
     * Creates the command line interface for retrieving JSON configurations.
     * The name-value pair is market as both: hidden and positional.
     */
    SetCfgCli(bool spec = true);

    /**
     * Destructor.
     */
    virtual ~SetCfgCli();

    /**
     * Initializes the object with command line options.
     *
     * In addition checks if single quotation marks where used
     * around each JSON configuration that was given as an argument.
     *
     * @param ac - argument count
     * @param av - argument array
     *
     * @see CliBase::initCli(int, char*)
     */
    virtual void parse(int ac, char* av[]);

    /**
     * Validates command line options
     * 1. Checks the endpoint
     * 2. If -h or -V option were used respective informations are printed
     * 3. GSoapContexAdapter is created, and info about server requested
     * 4. Additional check regarding server are performed
     * 5. If verbal additional info is printed
     *
     * @return GSoapContexAdapter instance, or null if all activities
     * 				requested using program options have been done.
     */
    virtual bool validate();

    /**
     * Gives the instruction how to use the command line tool.
     *
     * @return a string with instruction on how to use the tool
     */
    string getUsageString(string tool);

    /**
     * Gets a vector with SE configurations.
     *
     * @return if SE configurations were given as command line parameters a
     * 			vector containing the value names, otherwise an empty vector
     */
    vector<string> getConfigurations();

    /**
     * Checks the drain option
     *
     * @return true if on was used and false if off was used, otherwise the optional is not initialised
     */
    optional<bool> drain();

    /**
     * Checks the show-user-dn option
     *
     * @return true if on was used and false if off was used, otherwise the optional is not initialised
     */
    optional<bool> showUserDn();

    /**
     * Check the retry option
     *
     * @return the number of retries if it has been set, otherwise the optional is not initialised
     */
    optional< pair<string, int> > retry();

    /**
     * Check the optimizer-mode option
     *
     * @return the optimizer mode if it has been set, otherwise the optional is not initialised
     */
    optional<int> optimizer_mode();

    /**
     * Check the queue timeout
     *
     * @return the queue timeout if it has been set, otherwise the optional is not initialised
     */
    optional<unsigned> queueTimeout();

    /**
     * Get the bring-online settings
     *
     * @return SE name - value mapping
     */
    std::vector< std::pair<std::string, int> > getBringOnline();

    /**
     * Get the bandwidth limitation
     *
     * @return SE name - value mapping
     */
    optional<std::tuple<string, string, int> > getBandwidthLimitation();

    /**
     * Get the fixed number of actives for a pair
     */
    optional<std::tuple<string, string, int> > getActiveFixed();

    /**
     * Get the udt protocol settings
     *
     * @return udl protocol setting for given SE
     */
    optional< std::tuple<string, string, string> > getProtocol();

    /**
     * Gets the max number of active for given source SE
     *
     * @return SE name - max active pair if set by user, or not initialised optional otherwise
     */
    optional< pair<string, int> > getMaxSrcSeActive();

    /**
     * Gets the max number of active for given destination SE
     *
     * @return SE name - max active pair if set by user, or not initialised optional otherwise
     */
    optional< pair<string, int> > getMaxDstSeActive();

    /**
     * Gets the global timeout settings
     *
     * @return global timeout, or an uninitialised optional
     */
    optional<int> getGlobalTimeout();

    /**
     * Gets seconds per MB
     *
     * @return seconds per MB, or an uninitialised optional
     */
    optional<int> getSecPerMb();

    /**
     * @return : tuple with S3 access key, secret key, and VO name if specified by the user
     */
    optional< std::tuple<std::string, std::string, std::string> > s3();

private:
    /// helper function for handling max source and destination active
    optional< pair<string, int> > getMaxSeActive(string option);

    /// parses the multiple parameters that were provided by the user and creates a SE name - value mapping
    void parseBringOnline();

    // parses parameters for max bandwidth
    void parseMaxBandwidth();

    // parses active fixed parameters
    void parseActiveFixed();

    /// JSON configurations specified by user
    vector<string> cfgs;

    /// SE name + VO name and the respective value of maximum concurrent files in staging process
    std::vector< std::pair<std::string, int> > bring_online;

    // Source, dest, limit
    optional<std::tuple<string, string, int> > bandwidth_limitation;

    // source, dest, active
    optional<std::tuple<string, string, int> > active_fixed;

    CfgParser::CfgType type;
};

}
}

#endif /* NAMEVALUECLI_H_ */
