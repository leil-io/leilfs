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

#include "mount/tweaks.h"
#include "common/platform.h"

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

class Variable {
public:
	virtual ~Variable() {}
	virtual void setValue(const std::string &value) = 0;
	virtual std::string getValue() const = 0;
};

template <typename T>
class VariableImpl : public Variable {
public:
	VariableImpl(std::atomic<T> &v) : value_(&v) {}

	void setValue(const std::string &value) override {
		std::stringstream ss(value);
		T t;

		ss >> std::boolalpha >> t;

		if (!ss.fail()) { value_->store(t, std::memory_order_release); }
	}

	std::string getValue() const override {
		std::stringstream ss;
		ss << std::boolalpha << value_->load();
		return ss.str();
	}

private:
	std::atomic<T> *value_;
};

template <typename T>
std::unique_ptr<Variable> makeVariable(std::atomic<T> &v) {
	return std::unique_ptr<Variable>(new VariableImpl<T>(v));
}

class StringVariableImpl : public Variable {
public:
	StringVariableImpl(std::string &value, std::mutex &mutex) : value_(&value), mutex_(&mutex) {}

	void setValue(const std::string &value) override {
		std::lock_guard<std::mutex> lock(*mutex_);
		*value_ = value;
	}

	std::string getValue() const override {
		std::lock_guard<std::mutex> lock(*mutex_);
		return *value_;
	}

private:
	std::string *value_;
	std::mutex *mutex_;
};

std::unique_ptr<Variable> makeVariable(std::string &value, std::mutex &mutex) {
	return std::make_unique<StringVariableImpl>(value, mutex);
}

class Tweaks::Impl {
public:
	struct VariableEntry {
		std::string name;
		std::unique_ptr<Variable> var;
		std::string optionName;
		std::atomic<uint64_t> lastChangeEpoch;

		VariableEntry(std::string n, std::unique_ptr<Variable> v, std::string o, uint64_t e)
		    : name(std::move(n)), var(std::move(v)), optionName(std::move(o)), lastChangeEpoch(e) {}

		VariableEntry(const VariableEntry &) = delete;
		VariableEntry &operator=(const VariableEntry &) = delete;
		VariableEntry(VariableEntry &&) = delete;
		VariableEntry &operator=(VariableEntry &&) = delete;
	};

	std::list<VariableEntry> variables;
	std::atomic<uint64_t> globalEpoch{0};
};

Tweaks::Tweaks() : impl_(new Impl) {}

Tweaks::~Tweaks() {}

void Tweaks::registerVariable(const std::string &name, std::atomic<bool> &variable,
                              const std::string optionName) {
	uint64_t newEpoch = impl_->globalEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
	impl_->variables.emplace_back(name, makeVariable(variable), optionName, newEpoch);
}

void Tweaks::registerVariable(const std::string &name, std::atomic<uint32_t> &variable,
                              const std::string optionName) {
	uint64_t newEpoch = impl_->globalEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
	impl_->variables.emplace_back(name, makeVariable(variable), optionName, newEpoch);
}

void Tweaks::registerVariable(const std::string &name, std::atomic<uint64_t> &variable,
                              const std::string optionName) {
	uint64_t newEpoch = impl_->globalEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
	impl_->variables.emplace_back(name, makeVariable(variable), optionName, newEpoch);
}

void Tweaks::registerVariable(const std::string &name, std::string &variable, std::mutex &mutex,
                              const std::string optionName) {
	uint64_t newEpoch = impl_->globalEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
	impl_->variables.emplace_back(name, makeVariable(variable, mutex), optionName, newEpoch);
}

void Tweaks::setValue(const std::string &name, const std::string &value) {
	for (auto &entry : impl_->variables) {
		if (entry.name == name) {
			entry.var->setValue(value);
			uint64_t newEpoch = impl_->globalEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
			entry.lastChangeEpoch.store(newEpoch, std::memory_order_release);
			break;
		}
	}
}

std::string Tweaks::getValue(const std::string &name) const {
	for (const auto &nameAndVariable : impl_->variables) {
		if (nameAndVariable.name == name) { return nameAndVariable.var->getValue(); }
	}
	return std::string();
}

std::string Tweaks::getValueByOptionName(const std::string &optionName) const {
	for (const auto &nameAndVariable : impl_->variables) {
		if (nameAndVariable.optionName == optionName) { return nameAndVariable.var->getValue(); }
	}
	return std::string();
}

std::string Tweaks::getAllValues() const {
	std::stringstream ss;
	for (const auto &nameAndVariable : impl_->variables) {
		ss << nameAndVariable.name << "\t" << nameAndVariable.var->getValue() << "\n";
	}
	return ss.str();
}

uint64_t Tweaks::getGlobalLastChangeEpoch() const {
	return impl_->globalEpoch.load(std::memory_order_acquire);
}

uint64_t Tweaks::getVarLastChangeEpochByName(const std::string &name) const {
	for (const auto &entry : impl_->variables) {
		if (entry.name == name) { return entry.lastChangeEpoch.load(std::memory_order_acquire); }
	}
	return 0;
}
