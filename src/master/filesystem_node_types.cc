/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2016 Skytechnology sp. z o.o.
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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "master/filesystem_node_types.h"

FSNode *FSNode::create(FSNodeType type) {
	switch (type) {
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
		return new FSNodeFile(type);
	case FSNodeType::kDirectory:
		return new FSNodeDirectory();
	case FSNodeType::kSymlink:
		return new FSNodeSymlink();
	case FSNodeType::kFifo:
	case FSNodeType::kSocket:
		return new FSNode(type);
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		return new FSNodeDevice(type);
	case FSNodeType::kUnknown:
	default:
		assert(!"invalid node type");
	}
	return nullptr;
}

void FSNode::destroy(FSNode *node) {
	for (auto const &[_, handlePtr] : node->parents) { delete handlePtr; }

	delete node;
}
