/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/type_defs.h"
#include "kv/ikv_engine.h"

class MemoryMappedFile;

namespace kv {
class IKVEngine;
}

/// Metadata section used for bootstrapping from metadata.sfs
struct MetadataFileSection {
	bool matchesSectionTypeOf(const uint8_t *sectionPtr) const {
		return std::memcmp(sectionPtr, name.data(), name.size()) == 0;
	}

	std::string name;  ///< Name of the section

	std::function<int8_t(bool)> isBootstrapNeeded;  ///< Returns 1 if bootstrap is needed
	std::function<int8_t(bool)> loadFunction;       ///< Loads section into FDB
};

class MetadataSectionBootstrapFDB {
public:
	explicit MetadataSectionBootstrapFDB(kv::IKVEngine *kvEngine);
	bool bootstrapSections();

private:
	struct SectionMarker {
		size_t offset = 0;
		uint64_t length = 0;
	};

	bool prepare(const std::string &metadataFilePath);
	std::optional<SectionMarker> findSection(std::string_view name) const;
	int8_t saveMetadataHeader();

	void initMetadataFileSections();

	int8_t isSectionBootstrapNeeded(std::string_view prefix);
	int8_t loadNodesSection();
	int8_t loadChunkSection();
	int8_t loadEdgesSection();
	int8_t loadFreeSection();
	int8_t loadXAttrSection();
	int8_t loadQuotaSection();

	inode_t maxInodeId_ = 0;
	uint64_t metadataVersion_ = 0;
	uint32_t nextSessionId_ = 0;
	std::shared_ptr<MemoryMappedFile> metadataFile_;
	std::unordered_map<std::string, SectionMarker> sectionMarkers_;
	kv::IKVEngine *kvEngine_ = nullptr;

	/// Sections to bootstrap from metadata.sfs
	std::vector<MetadataFileSection> metadataFileSections_;
};
