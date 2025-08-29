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
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
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

	/// Adds a new observer (slot) bound to a member function on an object.
	template <typename T>
	void connect(T *object, void (T::*method)(Args...)) {
		slots_.emplace_back([object, method](Args... args) { (object->*method)(args...); });
	}

	/// Adds a new observer (slot) bound to a const member function on an object.
	template <typename T>
	void connect(const T *object, void (T::*method)(Args...) const) {
		slots_.emplace_back([object, method](Args... args) { (object->*method)(args...); });
	}

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

	/// Overloads to connect member functions directly to this property's signal.
	/// These are thin pass-throughs to the underlying Signal to keep the API ergonomic,
	/// so callers don't need to reach for getSignal().connect(...).
	///
	/// Non-const receiver overload taking const-ref parameters
	template <typename Obj>
	void connect(Obj *object, void (Obj::*method)(const T &, const T &)) {
		signal_.connect(object, method);
	}

	/// Const receiver overload taking const-ref parameters
	template <typename Obj>
	void connect(const Obj *object, void (Obj::*method)(const T &, const T &) const) {
		signal_.connect(object, method);
	}

	/// Adapters for member functions that take values (T, T) rather than const references.
	/// Signal emits (const T&, const T&), so these overloads bridge the signature by
	/// copying the values when invoking the member method.
	///
	/// Non-const receiver overload taking values
	template <typename Obj>
	void connect(Obj *object, void (Obj::*method)(T, T)) {
		signal_.connect([object, method](const T &oldValue, const T &newValue) {
			(object->*method)(oldValue, newValue);
		});
	}

	/// Const receiver overload taking values
	template <typename Obj>
	void connect(const Obj *object, void (Obj::*method)(T, T) const) {
		signal_.connect([object, method](const T &oldValue, const T &newValue) {
			(object->*method)(oldValue, newValue);
		});
	}

	/// Convenience method for slots that don't need the old/new values
	void connectSimple(const std::function<void()> &slot) {
		signal_.connect([slot](const T &, const T &) { slot(); });
	}

private:
	std::string name_;   ///< Property name, could be used as key for database or logging
	T value_;            ///< Current value of the property
	SignalType signal_;  ///< Signal to notify observers about changes
};

/// @brief Adds support for incrementing and decrementing integral properties.
template <typename T>
    requires std::integral<T>
class ObservableIntegralProperty : public ObservableProperty<T> {
public:
	ObservableIntegralProperty(std::string name, T initialValue)
	    : ObservableProperty<T>(std::move(name), initialValue) {}

	/// Increments the property value by the specified amount, with overflow check.
	void increment(T amount = 1) {
		if (amount == 0) { return; }

		T current = this->getValue();

		if constexpr (std::is_unsigned_v<T>) {
			// Unsigned: only non-negative amounts; check for overflow on addition.
			if (current > std::numeric_limits<T>::max() - amount) {
				throw std::overflow_error("Integral property overflow on increment.");
			}
			this->setValue(static_cast<T>(current + amount));
		} else {
			if (amount > 0) {
				// Overflow check: current + amount > max -> current > max - amount
				if (current > static_cast<T>(std::numeric_limits<T>::max() - amount)) {
					throw std::overflow_error("Integral property overflow on increment.");
				}
			} else {  // amount < 0 -> effectively a decrement by -amount
				// Underflow check: current + amount < min -> current < min - amount (amount
				// negative)
				if (current < static_cast<T>(std::numeric_limits<T>::min() - amount)) {
					throw std::underflow_error(
					    "Integral property underflow on increment with negative amount.");
				}
			}

			this->setValue(static_cast<T>(current + amount));
		}
	}

	/// Decrements the property value by the specified amount, with underflow check.
	void decrement(T amount = 1) {
		if (amount == 0) { return; }

		T current = this->getValue();

		if constexpr (std::is_unsigned_v<T>) {
			// Only non-negative amounts are meaningful for unsigned types
			if (current < amount) {
				throw std::underflow_error("Integral property underflow on decrement.");
			}
			this->setValue(static_cast<T>(current - amount));
		} else {
			if (amount > 0) {
				// Underflow check: current - amount < min -> current < min + amount
				if (current < static_cast<T>(std::numeric_limits<T>::min() + amount)) {
					throw std::underflow_error("Integral property underflow on decrement.");
				}
			} else {  // amount < 0 -> effectively an increment by -amount
				// Overflow check: current - amount > max  <=> current > max + amount (amount
				// negative)
				if (current > static_cast<T>(std::numeric_limits<T>::max() + amount)) {
					throw std::overflow_error(
					    "Integral property overflow on decrement with negative amount.");
				}
			}

			this->setValue(static_cast<T>(current - amount));
		}
	}

	/// Pre-increments the property value by one.
	ObservableIntegralProperty<T> &operator++() {
		increment(1);
		return *this;
	}

	/// Pre-decrements the property value by one.
	ObservableIntegralProperty<T> &operator--() {
		decrement(1);
		return *this;
	}

	/// Convenience function to returns the type size in bytes.
	static constexpr size_t typeSize() { return sizeof(T); }
};
