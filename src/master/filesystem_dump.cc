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

#ifdef METARESTORE

#include "common/platform.h"

#include "master/filesystem_freenode.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"

void fs_dumpedge(FSNodeDirectory *parent, FSNode *child, const std::string &name) {
	if (parent == NULL) {
		if (child->type == FSNodeType::kTrash) {
			printf("E|p:     TRASH|c:%10" PRIiNode "|n:%s\n", child->id,
			       fsnodes_escape_name(name).c_str());
		} else if (child->type == FSNodeType::kReserved) {
			printf("E|p:  RESERVED|c:%10" PRIiNode "|n:%s\n", child->id,
			       fsnodes_escape_name(name).c_str());
		} else {
			printf("E|p:      NULL|c:%10" PRIiNode "|n:%s\n", child->id,
			       fsnodes_escape_name(name).c_str());
		}
	} else {
		printf("E|p:%10" PRIiNode "|c:%10" PRIiNode "|n:%s\n", parent->id, child->id,
		       fsnodes_escape_name(name).c_str());
	}
}

void fs_dumpnode(FSNode *f) {
	char c;
	uint32_t i, ch;

	c = '?';
	switch (f->type) {
	case FSNodeType::kDirectory:
		c = 'D';
		break;
	case FSNodeType::kSocket:
		c = 'S';
		break;
	case FSNodeType::kFifo:
		c = 'F';
		break;
	case FSNodeType::kBlockDev:
		c = 'B';
		break;
	case FSNodeType::kCharDev:
		c = 'C';
		break;
	case FSNodeType::kSymlink:
		c = 'L';
		break;
	case FSNodeType::kFile:
		c = '-';
		break;
	case FSNodeType::kTrash:
		c = 'T';
		break;
	case FSNodeType::kReserved:
		c = 'R';
		break;
	case FSNodeType::kUnknown:
		break;
	}

	printf("%c|i:%10" PRIiNode "|#:%" PRIu8 "|e:%1" PRIX16 "|m:%04" PRIo16 "|u:%10" PRIu32
	       "|g:%10" PRIu32 "|a:%10" PRIu32 ",m:%10" PRIu32 ",c:%10" PRIu32 "|t:%10" PRIu32,
	       c, f->id, f->goal, (uint16_t)(f->mode >> 12), (uint16_t)(f->mode & 0xFFF), f->uid,
	       f->gid, f->atime, f->mtime, f->ctime, f->trashtime);

	if (f->type == FSNodeType::kBlockDev || f->type == FSNodeType::kCharDev) {
		printf("|d:%5" PRIu32 ",%5" PRIu32 "\n", static_cast<FSNodeDevice*>(f)->rdev >> 16,
		       static_cast<FSNodeDevice*>(f)->rdev & 0xFFFF);
	} else if (f->type == FSNodeType::kSymlink) {
		printf("|p:%s\n", fsnodes_escape_name((std::string)static_cast<FSNodeSymlink*>(f)->path).c_str());
	} else if (f->type == FSNodeType::kFile || f->type == FSNodeType::kTrash ||
	           f->type == FSNodeType::kReserved) {
		FSNodeFile *node_file = static_cast<FSNodeFile*>(f);
		printf("|l:%20" PRIu64 "|c:(", node_file->length);
		ch = node_file->chunkCount();
		for (i = 0; i < ch; i++) {
			if (node_file->chunks[i] != 0) {
				printf("%016" PRIX64, node_file->chunks[i]);
			} else {
				printf("N");
			}
			if (i + 1 < ch) {
				printf(",");
			}
		}
		printf(")|r:(");
		i = 0;
		for(const auto &sessionid : node_file->sessionIds) {
			if (i > 0) {
				printf(",");
			}
			printf("%" PRIu32, sessionid);
			++i;
		}
		printf(")\n");
	} else {
		printf("\n");
	}
}

void fs_dumpnodes() {
	for (uint32_t i = 0; i < NODEHASHSIZE; i++) {
		for (const auto &[_, node] : gMetadata->nodeHash[i]) {
			fs_dumpnode(node);
		}
	}
}

void fs_dumpedgelist(FSNodeDirectory *parent) {
	for (const auto &entry : parent->entries) {
		fs_dumpedge(parent, entry.second, (std::string)(*entry.first));
	}
	if (parent->case_insensitive) {
		for (const auto &entry : parent->lowerCaseEntries) {
			fs_dumpedge(parent, entry.second, (std::string)(*entry.first));
		}
	}
}

void fs_dumpedgelist(const TrashPathContainer &data) {
	for (const auto &entry : data) {
		FSNode *child = fsnodes_id_to_node(entry.first.id);
		fs_dumpedge(nullptr, child, (std::string)entry.second);
	}
}

void fs_dumpedgelist(const ReservedPathContainer &data) {
	for (const auto &entry : data) {
		FSNode *child = fsnodes_id_to_node(entry.first);
		fs_dumpedge(nullptr, child, (std::string)entry.second);
	}
}

void fs_dumpedges(FSNodeDirectory *parent) {
	fs_dumpedgelist(parent);
	for (const auto &entry : parent->entries) {
		FSNode *child = entry.second;
		if (child->type == FSNodeType::kDirectory) {
			fs_dumpedges(static_cast<FSNodeDirectory*>(child));
		}
	}
	if (parent->case_insensitive) {
		for (const auto &entry : parent->lowerCaseEntries) {
			FSNode *child = entry.second;
			if (child->type == FSNodeType::kDirectory) {
				fs_dumpedges(static_cast<FSNodeDirectory *>(child));
			}
		}
	}
}

void fs_dumpfree() {
	for (const auto &n : gMetadata->inodePool) {
		printf("I|i:%10" PRIiNode "|f:%10" PRIu32 "\n", n.id, n.ts);
	}
}

void xattr_dump() {
	for (auto i = 0; i < XATTR_DATA_HASH_SIZE; i++) {
		for (const auto &xattrDataEntry : gMetadata->xattrDataHash[i]) {
			printf(
			    "X|i:%10" PRIiNode "|n:%s|v:%s\n", xattrDataEntry.get()->inode,
			    fsnodes_escape_name(std::string((char *)xattrDataEntry.get()->attributeName.data(),
			                                    xattrDataEntry.get()->attributeName.size()))
			        .c_str(),
			    fsnodes_escape_name(std::string((char *)xattrDataEntry.get()->attributeValue.data(),
			                                    xattrDataEntry.get()->attributeValue.size()))
			        .c_str());
		}
	}
}

void fs_dump(void) {
	fs_dumpnodes();
	fs_dumpedges(gMetadata->root);
	fs_dumpedgelist(gMetadata->trash);
	fs_dumpedgelist(gMetadata->reserved);
	fs_dumpfree();
	xattr_dump();
}

#endif
