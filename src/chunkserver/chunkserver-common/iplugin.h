/*
   Copyright 2023 Leil Storage

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

#include <boost/config.hpp>
#include <boost/dll/alias.hpp>
#include <boost/dll/import.hpp>
#include <boost/shared_ptr.hpp>

/// IPlugin is the base interface for all plugins.
/// It contains the basic information for all plugins.
class BOOST_SYMBOL_VISIBLE IPlugin {
public:
	/// Default constructor
	IPlugin() = default;
	/// Virtual destructor for correct polymorphism
	virtual ~IPlugin() = default;

	/// Should be called after instantiation to initialize the plugin.
	/// This way, the constructor can be simple and marked as noexcept if
	/// needed.
	virtual bool initialize() = 0;

	/// Called on chunkserver reload to refresh plugin configuration parameters.
	virtual void reload() = 0;

	/// Returns the plugin name
	virtual std::string name() = 0;
	/// Returns the plugin version
	virtual unsigned int version() = 0;
	/// String representation of this plugin
	virtual std::string toString() = 0;
};
