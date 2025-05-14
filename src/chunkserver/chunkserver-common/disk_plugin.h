#pragma once

#include "chunkserver-common/disk_interface.h"
#include "chunkserver-common/iplugin.h"

/// DiskPlugin is an interface for all Disk plugins.
class BOOST_SYMBOL_VISIBLE DiskPlugin : public IPlugin {
public:
	/// Default constructor
	DiskPlugin();

	/// Virtual destructor
	virtual ~DiskPlugin();

	/// To keep simple the constructor
	bool initialize() override;

	/// Initializes the logger for this plugin.
	void initializeLogger();

	/// Matches the versioning system of the other packages.
	/// Override this method if you need to change the version for testing.
	unsigned int version() override { return SAUNAFS_VERSHEX; }

	/// Returns the disk prefix handled by this plugin.
	/// For instance, to handle the following hdd.conf line:
	/// zonefs:/mnt/saunafs/meta/nvme1 | /mnt/saunafs/data/smr1
	/// the prefix must be 'zonefs'.
	virtual std::string prefix() = 0;

	/// Returns a newly created concrete Disk with the given configuration.
	virtual IDisk *createDisk(const disk::Configuration &configuration) = 0;

	/// Returns a string representation of the plugin. Useful for logging and
	/// debugging purposes.
	std::string toString() override;

	/// Do specific resources cleanup.
	virtual void cleanup() = 0;

private:
	/// Needed for correct logging after upgrade to c++23
	std::shared_ptr<spdlog::logger> logger_;
};
