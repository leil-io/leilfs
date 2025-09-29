#pragma once

#include <cstdint>
#include <memory>
#include <string>

class ChangelogDb {
public:
	ChangelogDb();
	virtual ~ChangelogDb();

	ChangelogDb(const ChangelogDb&) = delete;
	ChangelogDb& operator=(const ChangelogDb&) = delete;

	ChangelogDb(ChangelogDb&&) = delete;
	ChangelogDb& operator=(ChangelogDb&&) = delete;

	void put(uint64_t version, const std::string &entry);
	void flush();
	uint64_t getFirstLogVersion();
	uint64_t getLastLogVersion();
private:
	// Pimpl idiom
	struct ChangelogDbImpl;
	std::unique_ptr<ChangelogDbImpl> impl;
};
