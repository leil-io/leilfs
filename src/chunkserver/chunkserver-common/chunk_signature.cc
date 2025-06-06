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

#include "chunk_signature.h"

#include <unistd.h>
#include <cstring>

#include "chunkserver-common/disk_interface.h"
#include "common/slice_traits.h"

ChunkSignature::ChunkSignature()
		: chunkId_(0),
		  chunkVersion_(0),
		  chunkType_(0),
		  hasValidSignatureId_(false) {
}

ChunkSignature::ChunkSignature(uint64_t chunkId, uint32_t chunkVersion,
                               ChunkPartType chunkType)
    : chunkId_(chunkId), chunkVersion_(chunkVersion), chunkType_(chunkType),
      hasValidSignatureId_(true) {}

bool ChunkSignature::readFromDescriptor(IDisk * /*disk*/, int fileDescriptor,
                                        off_t offset) {
	const ssize_t signatureLength = signatureSize();
	std::vector<uint8_t> buff;
	buff.resize(signatureLength);

	uint8_t *buffer = buff.data();
	const ssize_t ret = pread(fileDescriptor, buffer, signatureLength, offset);
	if (ret != signatureLength) {
		return false;
	}

	const uint8_t* ptr = buffer + kSignatureIdSize;
	chunkId_ = get64bit(&ptr);
	get32bit(&ptr, chunkVersion_);
	chunkType_ = slice_traits::standard::ChunkPartType();

	if (memcmp(buffer, kSauSignatureId, kSignatureIdSize) == 0) {
		hasValidSignatureId_ = true;
		try {
			::deserialize(ptr, sizeof(ChunkPartType), chunkType_);
		} catch (Exception& ex) {
			return false;
		}
	} else {
		hasValidSignatureId_ = false;
	}

	return true;
}

uint32_t ChunkSignature::serializedSize() const {
	return kSignatureIdSize +
	       ::serializedSize(chunkId_, chunkVersion_, chunkType_);
}

void ChunkSignature::serialize(uint8_t **destination) const {
	memcpy(*destination, kSauSignatureId, kSignatureIdSize);
	*destination += kSignatureIdSize;
	::serialize(destination, chunkId_, chunkVersion_, chunkType_);
}

std::string ChunkSignature::toString() const {
	std::stringstream result;

	result << "{ isValid: " << hasValidSignatureId_
	   << ", id: " << chunkId_
	   << ", version: " << chunkVersion_ << "}";

	return result.str();
}
