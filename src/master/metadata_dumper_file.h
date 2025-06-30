/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#pragma once

#include "common/platform.h"

#include <poll.h>
#include <syslog.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <memory>

#include "common/time_utils.h"
#include "master/metadata_dumper_interface.h"

// Forward declaration for threaded metadata dumper
class ThreadedMetadataDumper;

class MetadataDumperFile : public IMetadataDumper {
public:
	MetadataDumperFile(const std::string &metadataFilename, const std::string &metadataTmpFilename);
	~MetadataDumperFile(); // Add explicit destructor declaration

	bool dumpSucceeded() const override;
	bool inProgress() const override;
	bool useMetarestore() const override;

	void setMetarestorePath(const std::string& path) override;
	void setUseMetarestore(bool val) override;

	/// returns true and modifies dumpType (to FOREGROUND_DUMP) if we return as a child
	bool start(DumpType& dumpType, uint64_t checksum) override;

	// for poll
	void pollDesc(std::vector<pollfd> &pdesc) override;
	void pollServe(const std::vector<pollfd> &pdesc) override;

	/// waits until the metadumper finishes
	void waitUntilFinished() override;

private:
	/// waits until the metadumper finishes but not longer than timeout
	void waitUntilFinished(SteadyDuration timeout);

	void dumpingFinished();

	/// how long can the decimal representation of a(n) (u)int64 be
	static const uint32_t kInt64MaxDecimalLength = 21;

	/// should the metarestore be used at all
	bool useMetarestore_;

	/// if last dump was unsuccessful, now dump by master
	bool dumpingSucceeded_;

	/// fd of the reading end of the pipe (connected to stdout of the dumping process)
	int dumpingProcessFd_;

	/// pos in `pollfd`s array
	int32_t dumpingProcessPollFdsPos_;

	/// the dumping process has written something
	bool dumpingProcessOutputEmpty_;

	std::string metarestorePath_;
	std::string metadataFilename_;
	std::string metadataTmpFilename_;

	/// Thread-based metadata dumper to replace fork usage
	std::unique_ptr<ThreadedMetadataDumper> threaded_dumper_;
};
