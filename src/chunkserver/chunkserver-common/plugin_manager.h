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

#pragma once

#include "common/platform.h"

#include <map>
#include <string>

#include <boost/dll/import.hpp>
#include <boost/filesystem.hpp>
#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>

#include "chunkserver-common/disk_plugin.h"

using IPlugin_t = boost::shared_ptr<IPlugin>();

/// Responsible for loading and managing plugins and for creating the concrete
/// Disks for the given prefixes.
class PluginManager {
public:
	/// Function for loading plugins from a given directory.
	/// The function returns true if all plugins were loaded successfully.
	bool loadPlugins(const std::string &directory);

	/// Returns a concrete Disk using \p configuration if the PluginManager
	/// contains a handler for this \p configuration prefix.
	IDisk *createDisk(const disk::Configuration &configuration);

	/// Shows information about the loaded plugins.
	void showLoadedPlugins();

	/// Returns the hdd.cfg prefixes a Disk plugin is loaded for, comma
	/// separated, so an unresolved prefix can be reported against them.
	std::string loadedDiskPrefixes() const;

	/// True if this plugin manager can handle the plugin based on the version.
	bool checkVersion(IPlugin *plugin);

	/// Reload plugins options for all the needed plugins.
	void reloadPlugins();

	/// Cleanup resources for all the needed plugins.
	void cleanupPlugins();

private:
	/// Creators should never be out of scope or the plugin will be unloaded
	std::map<std::string, boost::function<IPlugin_t>> creators_;

	/// Container for all the loaded plugins
	std::map<std::string, boost::shared_ptr<IPlugin>> allPlugins_;

	/// Container for the loaded disk plugins
	std::map<std::string, boost::shared_ptr<DiskPlugin>> diskPlugins_;
};
