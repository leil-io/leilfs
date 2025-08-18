/*
   Copyright 2023 Leil Storage OÜ

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

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <random>
#include <unistd.h>
#include <vector>

int kChunkSize = 10000;

void showHelpMessageAndExit(char *progName, int status) {
	std::cerr
	    << "Usage:\n"
	       "    "
	    << progName
	    << " <TESTING_PATH> <NUMBER_OF_FILES> [--rev|--rand|--create-delete-only]\n\n"
	       "    Performs the following operations:\n"
	       "       - create a folder "
	       "'big_sessions_metadata_benchmark_<TIMESTAMP>' in the provided "
	       "path.\n"
	       "       - create NUMBER_OF_FILES empty files while keeping all of "
	       "them open.\n"
	       "       - close all of those new files.\n"
	       "       - delete all the files together with the folder.\n\n"
	       "    The time for creating/opening, closing and deleting is "
	       "reported.\n\n"
	       "    Options:\n"
	       "        --rev     Reverse the order of closing and deleting the "
	       "files.\n"
	       "        --rand    Randomize the order of closing and deleting the "
	       "files.\n"
	       "        --create-delete-only  Only create and delete files.\n\n"
	       "    Note: NUMBER_OF_FILES must be positive.\n"
	    << std::endl;
	exit(status);
}

int main(int argc, char **argv) {
	if (argc < 3 || argc > 4) {
		showHelpMessageAndExit(argv[0], 1);
	}

	std::string testingPath(argv[1]);
	int numberOfFiles = atoi(argv[2]);
	bool reverseLastOperations = false;
	bool randomizeLastOperations = false;
	bool onlyCreateDeleteFiles = false;

	if (argc == 4) {
		if (strcmp(argv[3], "--rev") == 0) {
			reverseLastOperations = true;
		} else if (strcmp(argv[3], "--rand") == 0) {
			randomizeLastOperations = true;
		} else if (strcmp(argv[3], "--create-delete-only") == 0) {
			onlyCreateDeleteFiles = true;
		} else {
			showHelpMessageAndExit(argv[0], 1);
		}
	}

	if (numberOfFiles <= 0) {
		showHelpMessageAndExit(argv[0], 1);
	}

	std::string folderPath = testingPath + "/big_sessions_metadata_benchmark_" +
	                         std::to_string(time(0));
	if (!std::filesystem::create_directory(folderPath)) {
		std::cerr << "Failed to create folder '" << folderPath << "'."
		          << std::endl;
		return 1;
	}
	std::cout << "Created folder '" << folderPath << "' successfully."
	          << std::endl;

	std::vector<int> testFilesFds(numberOfFiles);
	std::vector<int> filesOrder(numberOfFiles);
	auto start = std::chrono::high_resolution_clock::now();
	auto chunkStart = start;
	for (int i = 0; i < numberOfFiles; i++) {
		std::string filePath = folderPath + "/file_" + std::to_string(i);
		testFilesFds[i] =
		    open(filePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		filesOrder[i] = i;

		if (testFilesFds[i] == -1) {
			std::cerr << "Failed to create/open file '" << filePath << "'."
			          << std::endl;
			return 1;
		}
		if ((i + 1) % kChunkSize == 0) {
			auto chunkEnd = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> elapsedTime = chunkEnd - chunkStart;
			std::cout << "Time for creating/opening from " << i - kChunkSize + 2
			          << "-th to " << i + 1 << "-th file, in total "
			          << kChunkSize << " files: " << elapsedTime.count()
			          << " seconds.\n";
			chunkStart = chunkEnd;
		}

		if (onlyCreateDeleteFiles) {
			if (close(testFilesFds[i]) == -1) {
				std::cerr << "Failed to close file '" << filePath << "'." << std::endl;
				return 1;
			}
		}
	}
	auto afterOpen = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsedTime = afterOpen - start;
	std::cout << "Time for creating/opening " << numberOfFiles
	          << " files: " << elapsedTime.count() << " seconds.\n";

	if (reverseLastOperations) {
		std::reverse(filesOrder.begin(), filesOrder.end());
	}
	if (randomizeLastOperations) {
		std::mt19937 rng(time(0));
		std::shuffle(filesOrder.begin(), filesOrder.end(), rng);
	}

	if (!onlyCreateDeleteFiles) {
		// to make sure the reversing or randomizing is not included
		afterOpen = std::chrono::high_resolution_clock::now();

		for (int i = 0; i < numberOfFiles; i++) {
			// closing the filesOrder[i]-th file
			auto fd = testFilesFds[filesOrder[i]];

			if (close(fd) == -1) {
				std::cerr << "Failed to close file '" << folderPath << "/file_" << filesOrder[i]
				          << "'." << std::endl;
				return 1;
			}
		}
	}

	auto afterClose = std::chrono::high_resolution_clock::now();
	elapsedTime = afterClose - afterOpen;
	std::cout << "Time for closing " << numberOfFiles
	          << " files: " << elapsedTime.count() << " seconds.\n";
	chunkStart = afterClose;
	for (int i = 0; i < numberOfFiles; i++) {
		// deleting the filesOrder[i]-th file
		std::string filePath =
		    folderPath + "/file_" + std::to_string(filesOrder[i]);

		if (unlink(filePath.c_str()) != 0) {
			std::cerr << "Failed to delete file '" << filePath << "'."
			          << std::endl;
			return 1;
		}

		if ((i + 1) % kChunkSize == 0) {
			auto chunkEnd = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> elapsedTime = chunkEnd - chunkStart;
			std::cout << "Time for deleting from "
			          << numberOfFiles - i + kChunkSize - 1 << "-th to "
			          << numberOfFiles - i << "-th file, in total "
			          << kChunkSize << " files: " << elapsedTime.count()
			          << " seconds.\n";
			chunkStart = chunkEnd;
		}
	}

	auto afterUnlink = std::chrono::high_resolution_clock::now();
	elapsedTime = afterUnlink - afterClose;
	std::cout << "Time for deleting " << numberOfFiles
	          << " files: " << elapsedTime.count() << " seconds.\n";

	if (!std::filesystem::remove_all(folderPath)) {
		std::cerr << "Failed to remove folder '" << folderPath << "'."
		          << std::endl;
		return 1;
	}

	return 0;
}
