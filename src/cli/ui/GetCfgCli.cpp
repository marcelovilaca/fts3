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
 * GetCfgCli.cpp
 *
 *  Created on: Jul 25, 2012
 *      Author: simonm
 */

#include "GetCfgCli.h"

using namespace fts3::cli;

GetCfgCli::GetCfgCli(): VoNameCli(false) {

	specific.add_options()
					("name,n", value<string>(), "Restrict to specific SE or SE group.")
					;
}

GetCfgCli::~GetCfgCli() {
}

string GetCfgCli::getUsageString(string tool) {
	return "Usage: " + tool + " [options]";
}

string GetCfgCli::getName() {

	if (vm.count("se")) {
		return vm["se"].as<string>();
	}

	return string();
}

