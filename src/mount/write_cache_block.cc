/*
   Copyright 2013-2019 Skytechnology sp. z o.o.
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
#include "mount/write_cache_block.h"

#include <cstring>
#include <utility>

#include "common/massert.h"
#include "protocol/SFSCommunication.h"

WriteCacheBlock::WriteCacheBlock(uint32_t chunkIndex, uint32_t blockIndex, Type type)
		: chunkIndex(chunkIndex),
		  blockIndex(blockIndex),
		  from(0),
		  to(0),
		  type(type) {
	sassert(blockIndex < SFSBLOCKSINCHUNK);
}

WriteCacheBlock::WriteCacheBlock(WriteCacheBlock&& block) noexcept {
	blockData = std::move(block.blockData);
	chunkIndex = block.chunkIndex;
	blockIndex = block.blockIndex;
	from = block.from;
	block.from = 0;
	to = block.to;
	block.to = 0;
	type = block.type;
}

WriteCacheBlock &WriteCacheBlock::operator=(WriteCacheBlock &&block) {
	std::swap(blockData, block.blockData);
	std::swap(chunkIndex, block.chunkIndex);
	std::swap(blockIndex, block.blockIndex);
	std::swap(from, block.from);
	std::swap(to, block.to);
	std::swap(type, block.type);
	return *this;
}

bool WriteCacheBlock::expand(uint32_t from, uint32_t to, const uint8_t *buffer) {
	if (size() == 0) {
		this->from = from;
		this->to = to;
		memcpy(blockData.data() + from, buffer, to - from);
		return true;
	}
	if (from > this->to || to < this->from) { // can't expand
		return false;
	}
	memcpy(blockData.data() + from, buffer, to - from);
	if (from < this->from) {
		this->from = from;
	}
	if (to > this->to) {
		this->to = to;
	}
	return true;
}

uint64_t WriteCacheBlock::offsetInFile() const {
	return static_cast<uint64_t>(chunkIndex) * SFSCHUNKSIZE + offsetInChunk();
}

uint32_t WriteCacheBlock::offsetInChunk() const {
	return blockIndex * SFSBLOCKSIZE + from;
}

uint32_t WriteCacheBlock::size() const {
	return to - from;
}

const uint8_t* WriteCacheBlock::data() const {
	return blockData.data() + from;
}

uint8_t* WriteCacheBlock::data() {
	return blockData.data() + from;
}

uint8_t* WriteCacheBlock::rawData(uint32_t offset) {
	assert(offset < SFSBLOCKSIZE);
	return blockData.data() + offset;
}

void WriteCacheBlock::overwriteWithReadBlock(const WriteCacheBlock& readBlock) {
	assert(readBlock.type == kReadBlock);
	assert(chunkIndex == readBlock.chunkIndex);
	assert(blockIndex == readBlock.blockIndex);
	assert(from >= readBlock.from);
	assert(to <= readBlock.to);
	assert(readBlock.to <= SFSBLOCKSIZE);

	if (from > readBlock.from) {
		auto copySize = from - readBlock.from;
		memcpy(blockData.data() + readBlock.from, readBlock.data(), copySize);
	}
	if (to < readBlock.to) {
		auto copySize = readBlock.to - to;
		memcpy(blockData.data() + to, readBlock.blockData.data() + to, copySize);
	}
}
