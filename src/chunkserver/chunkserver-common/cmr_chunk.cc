/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include "chunkserver-common/cmr_chunk.h"

#include "chunkserver-common/subfolder.h"
#include "common/slice_traits.h"

CmrChunk::CmrChunk(uint64_t chunkId, ChunkPartType type, ChunkState state)
    : FDChunk(chunkId, type, state) {}

std::string CmrChunk::generateDataFilenameForVersion(uint32_t _version) const {
	return generateFilenameForVersion(_version, false);
}

int CmrChunk::renameChunkFile(uint32_t new_version) {
	const std::string oldMetaFilename = fullMetaFilename();
	const std::string oldDataFilename = fullDataFilename();

	std::string newMetaBasename =
	    generateMetadataFilenameForVersion(new_version);
	std::string newDataBasename = generateDataFilenameForVersion(new_version);

	const std::string newMetaFilename =
	    owner()->metaPath() + Subfolder::getSubfolderNameGivenChunkId(id()) +
	    "/" + newMetaBasename;
	const std::string newDataFilename =
	    owner()->dataPath() + Subfolder::getSubfolderNameGivenChunkId(id()) +
	    "/" + newDataBasename;

	int status = ::rename(oldMetaFilename.c_str(), newMetaFilename.c_str());

	if (status < 0) {
		return status;
	}

	status = ::rename(oldDataFilename.c_str(), newDataFilename.c_str());

	if (status < 0) {
		return status;
	}

	setVersion(new_version);

	return 0;
}

uint8_t *CmrChunk::getChunkHeaderBuffer() const {
#ifdef SAUNAFS_HAVE_THREAD_LOCAL
	static thread_local std::array<uint8_t, kMaxHeaderSize> hdrbuffer;
	return hdrbuffer.data();
#else  // SAUNAFS_HAVE_THREAD_LOCAL
	uint8_t *hdrbuffer =
	    static_cast<uint8_t *>(pthread_getspecific(hdrbufferkey));
	if (hdrbuffer == NULL) {
		hdrbuffer = static_cast<uint8_t *>(malloc(kMaxHeaderSize));
		passert(hdrbuffer);
		zassert(pthread_setspecific(hdrbufferkey, hdrbuffer));
	}
	return hdrbuffer;
#endif  // SAUNAFS_HAVE_THREAD_LOCAL
}

size_t CmrChunk::getHeaderSize() const {
	auto chunkType = type();

	if (slice_traits::isStandard(chunkType)) {
		return kMaxSignatureBlockSize + kCrcSize * maxBlocksInFile();
	}

	assert(slice_traits::isXor(chunkType) || slice_traits::isEC(chunkType));

	const uint32_t requiredHeaderSize =
	    kMaxSignatureBlockSize + kCrcSize * maxBlocksInFile();
	// header size is equal to the requiredHeaderSize rounded up to typical
	// disk block size
	const uint32_t diskBlockSize = kDiskBlockSize;
	const off_t dataOffset = (requiredHeaderSize + diskBlockSize - 1) /
	                         diskBlockSize * diskBlockSize;
	return dataOffset;
}

off_t CmrChunk::getCrcOffset() const { return kMaxSignatureBlockSize; }

void CmrChunk::shrinkToBlocks(uint16_t newBlocks) { (void)newBlocks; }

std::string CmrChunk::toString() const {
	std::stringstream result;

	result << "{id: " << id() << ", version: " << version()
	       << ", type: " << type().toString() << ", blocks: " << blocks()
	       << "}";

	return result.str();
}
