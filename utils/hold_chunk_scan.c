/*
   Copyright 2026      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

/* Test helper, loaded with LD_PRELOAD into one chunkserver: holds its disk scan.
 * opendir() of a path containing $HOLD_CHUNK_SCAN_PATH waits while the file
 * $HOLD_CHUNK_SCAN_FILE exists. The scan opens each chunk subfolder with opendir(), so it reads
 * no folder meanwhile; registration and the answers to the metadata server keep running. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef DIR *(*opendir_t)(const char *name);

DIR *opendir(const char *name) {
	/* Per thread: the scan of each disk runs in its own thread. */
	static _Thread_local opendir_t real_opendir = NULL;
	const char *held_path = getenv("HOLD_CHUNK_SCAN_PATH");
	const char *hold_file = getenv("HOLD_CHUNK_SCAN_FILE");

	if (!real_opendir) { real_opendir = (opendir_t)dlsym(RTLD_NEXT, "opendir"); }

	if (held_path && hold_file && strstr(name, held_path)) {
		while (access(hold_file, F_OK) == 0) { usleep(50000); }
	}

	return real_opendir(name);
}
