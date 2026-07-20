/*


   Copyright 2016 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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

#include <unistd.h>
#include <array>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver-common/hdd_utils.h"

/*!
 * Class representing an open chunk with acquired resources.
 * In its destructor, it is assumed that hdd_chunk_find/hdd_chunk_trylock was called
 * and chunk is in locked state, so its resources can be safely freed.
 */
class OpenChunk {
public:
	OpenChunk()
	    : chunk_(), metaFD_(-1), dataFD_(-1), crc_() {
	}

	OpenChunk(IChunk *chunk)
	    : chunk_(chunk),
	      metaFD_(chunk ? chunk->metaFD() : -1),
	      dataFD_(chunk ? chunk->dataFD() : -1),
	      crc_() {
		if (chunk && chunk->chunkFormat() == ChunkFormat::SPLIT) {
			crc_.reset(new CrcDataContainer{{}});
		}
	}

	OpenChunk(OpenChunk &&other) noexcept
	    : chunk_(other.chunk_),
	      metaFD_(other.metaFD_),
	      dataFD_(other.dataFD_),
	      crc_(std::move(other.crc_)) {
		other.chunk_ = nullptr;
		other.metaFD_ = -1;
		other.dataFD_ = -1;
	}

	/*!
	 * OpenChunk destructor.
	 * It is assumed that chunk_, if it exists, is properly locked.
	 */
	~OpenChunk() {
		if (chunk_) {
			bool closeError = false;

			if (chunk_->metaFD() >= 0) {
				if (::close(chunk_->metaFD()) < 0) {
					closeError = true;
				}
			}

			if (chunk_->dataFD() >= 0) {
				if (::close(chunk_->dataFD()) < 0) {
					closeError = true;
				}
			}

			if (closeError) {
				hddAddErrorAndPreserveErrno(chunk_);
				safs_silent_errlog(LOG_WARNING,"open_chunk: file:%s - close error",
				                   chunk_->fullMetaFilename().c_str());
				hddReportDamagedChunk(chunk_->id(), chunk_->type());
			}

			chunk_->setMetaFD(-1);
			chunk_->setDataFD(-1);
			// The chunk is leaving the open-chunks cache: drop I/O-speedup
			// caches (e.g. compression dictionaries) so idle chunks do not pin
			// memory. They are rebuilt lazily on the next I/O.
			chunk_->releaseCachedResources();
			hddChunkRelease(chunk_);
		} else if (metaFD_ >= 0) {
			::close(metaFD_);

			if (dataFD_ >= 0) {
				::close(dataFD_);
			}
		}
	}

	OpenChunk &operator=(OpenChunk &&other) noexcept {
		chunk_ = other.chunk_;
		metaFD_ = other.metaFD_;
		dataFD_ = other.dataFD_;
		crc_ = std::move(other.crc_);
		other.chunk_ = nullptr;
		other.metaFD_ = -1;
		other.dataFD_ = -1;
		return *this;
	}

	/*!
	 * Try to lock chunk in order to be able to remove it.
	 * \return true if chunk was successfully locked and can be removed, false otherwise.
	 */
	bool canRemove() {
		return hddChunkTryLock(chunk_);
	}

	/*!
	 * Remove a connection to chunk and prepare its resources to be freed.
	 * This function is called if chunk is to be inaccessible (deleted) in near future,
	 * but its resources (e.g. file descriptor) still need to be freed properly.
	 */
	void purge() {
		assert(chunk_);
		metaFD_ = chunk_->metaFD();
		dataFD_ = chunk_->dataFD();
		chunk_ = nullptr;
	}

	uint8_t *crcData() {
		assert(crc_);
		return crc_->data();
	}

private:
	IChunk *chunk_;
	int metaFD_;
	int dataFD_;
	std::unique_ptr<CrcDataContainer> crc_;
};
