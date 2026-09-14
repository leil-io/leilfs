/*
	Copyright 2023-2024 Leil Storage OÜ

	LeilFS is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, version 3.

	LeilFS is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <sys/syslog.h>
#include <cstring>
#include <future>
#include <vector>

#include "common/memory_mapped_file.h"

/// @brief Options used to load a section.
struct MetadataSectionLoaderOptions {
	MetadataSectionLoaderOptions(std::shared_ptr<MemoryMappedFile> metadataFile_,
	                             size_t offset_, int ignoreFlag_,
	                             uint64_t sectionLength_, bool loadLockIds_)
	    : metadataFile(std::move(metadataFile_)),
	      offset(offset_),
	      ignoreFlag(ignoreFlag_),
	      sectionLength(sectionLength_),
	      loadLockIds(loadLockIds_) {}

	/// @brief Pointer to the memory mapped file containing the metadata.
	std::shared_ptr<MemoryMappedFile> metadataFile;

	/// @brief Offset of the section in the metadata file.
	size_t offset;

	/// @brief Flag indicating whether to ignore some errors.
	int ignoreFlag;

	/// @brief Length of the section.
	uint64_t sectionLength;

	/// @brief Legacy to be deprecated. Used when loading chunks if metadata versions is lower than v2.9.
	bool loadLockIds;
};

/// @brief A metadata section.
struct MetadataSection {
	/// @brief The callback function used to load different sections.
	using LoadImplMethod = std::function<bool(MetadataSectionLoaderOptions)>;

	MetadataSection(std::string_view name_, std::string_view description_,
	                LoadImplMethod load_, bool asyncLoad_ = true,
	                bool isLegacy_ = false)
	    : name(name_),
	      description(description_),
	      load(std::move(load_)),
	      asyncLoad(asyncLoad_),
	      isLegacy(isLegacy_) {}

	/**
	 * @brief Check if the section name is a match for the section to be loaded.
	 *
	 * @param sectionPtr A pointer to the section beginning.
	 * @return True if the section name matches, or false otherwise.
	 */
	bool matchesSectionTypeOf(const uint8_t *sectionPtr) const {
		return std::memcmp(sectionPtr, name.data(), name.size()) == 0;
	}

	/// @brief Short name of the section.
	std::string_view name;

	/// @brief Description of the section (for logging purposes).
	std::string_view description;

	/// @brief Callback function used to load the section.
	LoadImplMethod load;

	/// Whether to load the section asynchronously or not (default: true).
	bool asyncLoad;

	/// Whether the section is legacy and won't be loaded by default (default: false).
	bool isLegacy;
};

/// Helper struct to store information about a section being loaded asynchronously.
struct MetadataLoaderFuture {
	/// @brief The name of the section.
	std::string sectionName;
	/// @brief The description of the section.
	std::string sectionDescription;
	/// @brief The future that will be set to true if the section was loaded
	std::future<bool> future;
};

/// @brief Loads metadata sections.
class MetadataLoader {
public:
	/// @brief A vector of futures used to load sections asynchronously.
	using Futures = std::vector<MetadataLoaderFuture>;
	/// @brief The type of options used to load sections.
	using Options = MetadataSectionLoaderOptions;

	/**
	 * @brief Load a section synchronously by calling the load callback
	 *
	 * @param section The section to load.
	 * @param options The options to use when loading the section.
	 * @return True if the section was loaded successfully, or false otherwise.
	 */
	static bool loadSection(const MetadataSection &, Options);

	/**
	 * @brief Load a section asynchronously.
	 *
	 * @param section The section to load.
	 * @param options The options to use when loading the section.
	 * @return A future that will be set to true if the section was loaded
	 *         successfully, or false otherwise.
	 */
	static std::future<bool> loadSectionAsync(const MetadataSection &, Options);

	/**
	 * @brief Load a section asynchronously and store its result in futures
	 *
	 * @param section The section to load.
	 * @param options The options to use when loading the section.
	 * @param futures The futures to use when loading the section.
	 */
	static void loadSectionAsync(const MetadataSection &, Options, Futures &);
};
