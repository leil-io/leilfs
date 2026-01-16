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

#include <atomic>
#include <memory>
#include <string>

/**
 * A class which provides an interface for modifying different registered variables.
 */
class Tweaks {
public:
	Tweaks();
	~Tweaks();

	/// Adds a new bool variable.
	void registerVariable(const std::string& name, std::atomic<bool>& variable, const std::string optionName = "");

	/// Adds a new uint32_t variable.
	void registerVariable(const std::string& name, std::atomic<uint32_t>& variable,  const std::string optionName = "");

	/// Adds a new uint64_t variable.
	void registerVariable(const std::string& name, std::atomic<uint64_t>& variable, const std::string optionName = "");

	/// Adds a new string variable.
	void registerVariable(const std::string &name, std::string &variable, std::mutex &mutex,
	                      const std::string optionName = "");

	/// Changes value of all variables with the given name.
	void setValue(const std::string& name, const std::string& value);

	std::string getValue(const std::string& name) const;

	std::string getValueByOptionName(const std::string& optionName) const;

	/// Returns values of all the registered variables.
	std::string getAllValues() const;

	/// Returns the global epoch counter.
	///
	/// The global epoch is a monotonically increasing counter that is
	/// incremented on every successful call to setValue() or registerVariable()
	/// on the Tweaks instance, regardless of whether the underlying value actually
	/// changed. It represents an update event, not strictly a value
	/// transition.
	///
	/// The initial epoch value is 0. The first call to setValue() will
	/// increment it to 1.
	///
	/// This method is thread-safe and lock-free.
	uint64_t getGlobalLastChangeEpoch() const;

	/// Returns the last change epoch of a variable identified by name.
	///
	/// The returned epoch corresponds to the global epoch value at the time
	/// setValue() or registerVariabbll() was last called for the specified
	/// variable. Epochs are updated on every setValue() call, even if the
	/// value is set to the same value as before.
	///
	/// If no variable with the given name exists, this method returns 0.
	///
	/// This method is thread-safe and lock-free.
	uint64_t getVarLastChangeEpochByName(const std::string &name) const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

inline Tweaks gTweaks;
