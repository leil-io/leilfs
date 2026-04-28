/*
	Copyright 2023-2024 Leil Storage OÜ

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

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <memory>
#include <string>

/**
 * MemoryFile is a class that represents a file in memory.
 * It is used to store the contents of a file in memory.
 * It is a RAII wrapper around mmap
 */
class MemoryMappedFile {
public:
	/// Default constructor
	MemoryMappedFile();

	/**
	 * Constructor
	 * @param path The path to the file
	 */
	explicit MemoryMappedFile(const std::string &path);

	/// Destructor
	virtual ~MemoryMappedFile();

	/**
	 * We don't want to allow copying or moving
	 */
	MemoryMappedFile(const MemoryMappedFile &) = delete;
	MemoryMappedFile &operator=(const MemoryMappedFile &) = delete;
	MemoryMappedFile(MemoryMappedFile &&) noexcept = delete;
	MemoryMappedFile &operator=(MemoryMappedFile &&) noexcept = delete;

	/**
	 * Get a pointer to the memory mapped contents of the file
	 * at the given offset
	 * @param offset The offset to the memory mapped contents
	 * @return A pointer to the memory mapped contents
	 */
	[[nodiscard]] uint8_t *seek(size_t offset) const;

	/**
	 * Read the given number of bytes from the memory mapped file
	 * at the given offset
	 * @param offset The offset to the memory mapped contents
	 * @param buf The buffer to read into
	 * @param size The number of bytes to read
	 * @param moveOffset Whether to advance the offset
	 * @return The number of bytes read
	 */
	size_t read(size_t &offset, uint8_t *buf, size_t size,
	            bool = false) const;

	/** Get the offset of the given pointer into the memory mapped file
	 * relative to the start of the file
	 * @param ptr The pointer to get the offset of
	 * @return The offset of the pointer
	 */
	[[nodiscard]] size_t offset(const uint8_t *ptr) const;

	/**
	 * Get the file path
	 * @return The file path
	 */
	[[nodiscard]] const std::string &filename() const;

private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
};
