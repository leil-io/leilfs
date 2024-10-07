// chunk_trash_manager.h
#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <filesystem>
#pragma GCC diagnostic pop
#include <string>

class ChunkTrashManager {
public:
	constexpr static const std::string_view kTrashDirname = ".trash.bin";
	static std::string getDeletionTimeString();
	static int moveToTrash(const std::filesystem::path& filePath,
	                       const std::filesystem::path& diskPath, const std::string& deletionTime);
};
