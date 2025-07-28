/*
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

#include <concepts>
#include <functional>
#include <string>
#include <utility>
#include <vector>

/// @file Implements a signal-slot mechanism to decouple property changes from their observers.

/// @brief Variadic template Signal that can handle variable type parameters
/// @tparam Args Types of parameters that the signal can emit.
/// @note This class is not thread-safe yet.
template <typename... Args>
class Signal {
public:
	using SlotType = std::function<void(Args...)>;

	/// Adds a new observer (slot) to the signal.
	/// @param slot The function to be called when the signal is emitted
	void connect(SlotType slot) { slots_.push_back(std::move(slot)); }

	/// Emits the signal, calling all connected slots with the provided arguments.
	/// @param args Arguments to pass to the connected slots
	/// @note All slots are called in the order they were connected.
	void emit(Args... args) {
		for (auto &slot : slots_) { slot(args...); }
	}

	/// Clears all connected slots.
	void clear() { slots_.clear(); }

	/// Returns the number of connected slots.
	size_t size() const { return slots_.size(); }

private:
	/// List of connected slots (observers)
	std::vector<SlotType> slots_;
};

/// @brief Observable property that emits signals when its value changes.
/// @tparam T Type of the property value, must be equality comparable.
/// @note This class is not thread-safe yet.
template <typename T>
    requires std::equality_comparable<T>
class ObservableProperty {
public:
	using SignalType = Signal<const T &, const T &>;  // Signal with old and new values
	using SlotType = typename SignalType::SlotType;

	ObservableProperty(std::string name, T initialValue)
	    : name_(std::move(name)), value_(std::move(initialValue)), signal_() {}

	/// Sets a new value for the property and emits a signal if the value has changed.
	/// @param newValue The new value to set
	void setValue(T newValue) {
		if (value_ == newValue) { return; }

		T oldValue = std::move(value_);
		value_ = std::move(newValue);
		signal_.emit(oldValue, value_);
	}

	/// Retrieves the current value of the property.
	const T &getValue() const { return value_; }

	/// Returns the name of the property.
	const std::string &getName() const { return name_; }

	/// Returns a reference to the signal associated with this property.
	SignalType &getSignal() { return signal_; }

	/// Connects a slot to the signal, allowing it to be notified of changes.
	/// @param slot The function to be called when the property changes.
	/// @note The slot receives the old and new values of the property.
	/// @note The slot can be a lambda or any callable that matches the signature.
	void connect(SlotType slot) { signal_.connect(std::move(slot)); }

	/// Convenience method for slots that don't need the old/new values
	void connectSimple(const std::function<void()> &slot) {
		signal_.connect([slot](const T &, const T &) { slot(); });
	}

private:
	std::string name_;   ///< Property name, could be used as key for database or logging
	T value_;            ///< Current value of the property
	SignalType signal_;  ///< Signal to notify observers about changes
};
