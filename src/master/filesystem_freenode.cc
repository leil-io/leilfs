/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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

#include "common/platform.h"

#include "master/filesystem_freenode.h"

#include "master/filesystem_metadata.h"

inode_t fsnodes_get_next_id(uint32_t ts, inode_t req_inode) {
	if(req_inode == 0 || !gMetadata->inodePool.markAsAcquired(req_inode,ts)) {
		req_inode = gMetadata->inodePool.acquire(ts);
	}
	if (req_inode == 0) {
		mabort("Out of free inode numbers");
	}
	if (req_inode > gMetadata->maxInodeId().getValue()) {
		gMetadata->maxInodeId().setValue(req_inode);
	}

	return req_inode;
}

uint8_t fs_apply_freeinodes(uint32_t /*ts*/, inode_t /*freeinodes*/) {
	// left for compatibility when reading from old metadata change log
	gMetadata->metadataVersion++;
	return 0;
}
