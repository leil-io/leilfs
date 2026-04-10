/*
   Copyright 2026      Leil Storage OÜ

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

#include "common/aligned_allocator.h"

#include "utils/data_generator.h"

#ifndef SFSCHUNKSIZE
#define SFSCHUNKSIZE (SFSBLOCKSIZE * SFSBLOCKSINCHUNK)
#endif

int main(int argc, char** argv) {
	if (argc != 3) {
		std::cerr << "Usage:\n"
		             "    "
		          << argv[0]
		          << " <file> <SLEEP_TIME>\n"
		             "Creates a file with the specified name and fills it with generated data.\n"
		             "Afterwards reads and reads again some chunks from it.\n"
		             "Sleeps for the specified time in seconds and creates a notification file.\n"
					 "At the end closes the file descriptor.\n";
		return 1;
	}

	uint64_t fileSize = 30 * SFSCHUNKSIZE;  // 1920 MiB (30 chunks of 64 MiB)
	DataGenerator generator(0);
	generator.createFile(argv[1], fileSize);
	uint32_t sleepTime = std::stoul(argv[2]);
	std::cerr << "File created successfully.\n";

	int fd = open(argv[1], O_RDONLY | O_DIRECT);
	utils_passert(fd >= 0);

	// Read the last two chunks
	std::vector<char, AlignedAllocator<char, SFSBLOCKSIZE>> buffer(SFSBLOCKSIZE);
	for (uint64_t offset = fileSize - 2 * SFSCHUNKSIZE; offset < fileSize; offset += SFSBLOCKSIZE) {
		ssize_t bytesRead = pread(fd, buffer.data(), buffer.size(), offset);
		utils_passert(bytesRead == (ssize_t)buffer.size());
	}
	std::cerr << "Last two chunks read successfully.\n";

	// Read first three chunks
	for (uint64_t offset = 0; offset < 3 * SFSCHUNKSIZE; offset += SFSBLOCKSIZE) {
		ssize_t bytesRead = pread(fd, buffer.data(), buffer.size(), offset);
		utils_passert(bytesRead == (ssize_t)buffer.size());
	}

	std::cerr << "First three and last two chunks read successfully.\n";
	// Sleep for a while to process some of the readahead requests
	usleep(600000);

	// Read last chunk again
	for (uint64_t offset = fileSize - SFSCHUNKSIZE; offset < fileSize; offset += SFSBLOCKSIZE) {
		ssize_t bytesRead = pread(fd, buffer.data(), buffer.size(), offset);
		utils_passert(bytesRead == (ssize_t)buffer.size());
	}
	std::cerr << "Reads completed successfully.\n";

	sleep(sleepTime);

	auto notify_file_reread = open("notify_file_reread", O_CREAT | O_WRONLY, 0644);
	utils_passert(notify_file_reread >= 0);
	utils_zassert(close(notify_file_reread));

	sleep(1);

	utils_zassert(close(fd));
	std::cerr << "FD closed successfully.\n";
	unlink("notify_file_reread");

	return 0;
}
