/*
   Copyright 2023-2024  Leil Storage OÜ

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

#include <cassert>

#include "chunk_trash_manager.h"
#include "chunk_trash_manager_impl.h"
#include "common/cfg.h"

u_short ChunkTrashManager::isEnabled = 1;

ChunkTrashManager::ImplentationPtr ChunkTrashManager::pImpl =
		std::make_shared<ChunkTrashManagerImpl>();

ChunkTrashManager &ChunkTrashManager::instance(ImplentationPtr newImpl) {
	static ChunkTrashManager instance;
	if (newImpl) {
		pImpl = newImpl;
	}
	return instance;
}

int ChunkTrashManager::moveToTrash(const std::filesystem::path &filePath,
                                   const std::filesystem::path &diskPath,
                                   const std::time_t &deletionTime) {
	if(!isEnabled) {
		return 0;
	}
	assert(pImpl && "Implementation should be set");
	return pImpl->moveToTrash(filePath, diskPath, deletionTime);
}

int ChunkTrashManager::init(const std::string &diskPath) {
	reloadConfig();
	assert(pImpl && "Implementation should be set");
	return pImpl->init(diskPath);
}

void ChunkTrashManager::collectGarbage() {
	if(!isEnabled) {
		return;
	}
	assert(pImpl && "Implementation should be set");
	pImpl->collectGarbage();
}

void ChunkTrashManager::reloadConfig() {
	assert(pImpl && "Implementation should be set");
	isEnabled = cfg_get("CHUNK_TRASH_ENABLED", isEnabled);
	pImpl->reloadConfig();
}
