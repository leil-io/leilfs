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

#include <master/metadata_loader.h>

#include <master/metadata_backend_interface.h>
#include <slogger/slogger.h>
#include <common/scoped_timer.h>

bool MetadataLoader::loadSection(const MetadataSection &section, Options options) {
	try {
		auto message = fmt::format("Section loaded successfully ({})", section.name.data());
		util::ScopedTimer timer(message);
		if (section.load(options)) {
			return true;
		}

		safs::log_err("error reading section ({})", section.name);
	} catch (const std::exception &e) {
		safs::log_err("Exception while processing section ({})", section.name);
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
