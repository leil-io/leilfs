/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include <master/exceptions.h>
#include <master/metadata_loader.h>
#include <slogger/slogger.h>

bool MetadataLoader::loadSection(const MetadataSection &section,
                                 Options options) {
	try {
		if (section.load(options)) {
			safs_pretty_syslog(LOG_INFO, "Section loaded successfully (%s)",
			                   section.name.data());
			return true;
		} else {
			safs_pretty_syslog(LOG_ERR, "error reading section (%s)",
			                   section.name.data());
		}
	} catch (const std::exception &e) {
		safs_pretty_syslog(LOG_ERR, "Exception while processing section (%s)",
		                   section.name.data());
		throw MetadataConsistencyException(e.what());
	}
	return false;
}

std::future<bool> MetadataLoader::loadSectionAsync(
    const MetadataSection &section, Options options) {
	return std::async(std::launch::async, loadSection, section, options);
}

void MetadataLoader::loadSectionAsync(const MetadataSection &section,
                                      Options options, Futures &futures) {
	MetadataLoaderFuture future;
	future.sectionName = section.name;
	future.sectionDescription = section.description;
	future.future = loadSectionAsync(section, options);
	futures.push_back(std::move(future));
}
