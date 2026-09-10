/*
   2016 Skytechnology sp. z o.o.

   This file is part of LeilFS.

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

#include <cstdio>

#include "master/metadata_loader.h"

bool fs_load_legacy_acls(MetadataLoader::Options);

bool fs_load_posix_acls(MetadataLoader::Options);

bool fs_load_acls(MetadataLoader::Options);

void fs_store_acls(FILE *fd);
