/*
   Copyright 2023 Leil Storage

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

#include "chunkserver-common/disk_chunks.h"

#include "chunkserver-common/chunk_interface.h"
#include "common/random.h"

const DiskChunks::Index DiskChunks::kInvalidIndex;

void DiskChunks::insert(IChunk* newChunk) {
	sassert(newChunk->indexInDisk() == kInvalidIndex);
	chunks_.push_back(newChunk);
	newChunk->setIndexInDisk(chunks_.size() - 1);

	// We consider a new chunk to be tested, so we need to bring it into the
	// "tested" section.
	markAsTestedInternal(newChunk);
}

void DiskChunks::remove(IChunk *removedChunk) {
	// First, we have to make sure that the removed chunk is in the untested
	// section.
	if (removedChunk->indexInDisk() < firstUntestedChunk_) {
		swap(removedChunk->indexInDisk(), firstUntestedChunk_ - 1);
		--firstUntestedChunk_;
	}

	// We are now sure that the removed chunk is in the "untested" section.
	// We can just swap it with the last element and then remove it.
	swap(removedChunk->indexInDisk(), chunks_.size() - 1);
	chunks_.pop_back();
	removedChunk->setIndexInDisk(kInvalidIndex);
}

void DiskChunks::markAsTested(IChunk *testedChunk) {
	markAsTestedInternal(testedChunk);
}

IChunk *DiskChunks::getRandomChunk() const {
	if (chunks_.empty())
		return NO_CHUNKS_IN_COLLECTION;

	return chunks_[rnd_ranged(Index(0), chunks_.size() - 1)];
}

IChunk *DiskChunks::chunkToTest() const {
	if (chunks_.empty())
		return NO_CHUNKS_IN_COLLECTION;

	if (firstUntestedChunk_ == chunks_.size())
		firstUntestedChunk_ = 0; // Start a new chunk test loop.

	return chunks_[firstUntestedChunk_];
}

void DiskChunks::shuffle() {
	if (chunks_.size() <= 1)
		return;

	// This is the regular Fisher-Yates shuffle but we need to update
	// the `indexInDisk` values for chunks so we're using our own `swap()`.
	for (Index i = chunks_.size() - 1; i >= 1; --i) {
		swap(i, rnd_ranged(Index(0), i));
	}

	firstUntestedChunk_ = 0;
}

size_t DiskChunks::size() const {
	return chunks_.size();
}

void DiskChunks::swap(Index lhsIndex, Index rhsIndex) {
	IChunk* lhsChunk = chunks_[lhsIndex];
	IChunk* rhsChunk = chunks_[rhsIndex];

	auto oldLhsIndex = lhsChunk->indexInDisk();
	lhsChunk->setIndexInDisk(rhsChunk->indexInDisk());
	rhsChunk->setIndexInDisk(oldLhsIndex);

	std::swap(chunks_[lhsIndex], chunks_[rhsIndex]);
}

void DiskChunks::markAsTestedInternal(IChunk *testedChunk) {
	if (testedChunk->indexInDisk() >= firstUntestedChunk_) {
		swap(testedChunk->indexInDisk(), firstUntestedChunk_);
		++firstUntestedChunk_;
	}
}
