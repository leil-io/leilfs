// chunk_trash_manager.h
#pragma once

#include <filesystem>
#include <string>

class ChunkTrashManager {
public:
	constexpr static const std::string_view kTrashDirname = ".trash.bin";
	static std::string getDeletionTimeString();
	static int moveToTrash(const std::filesystem::path& filePath,
	                       const std::filesystem::path& diskPath, const std::string& deletionTime);
};
