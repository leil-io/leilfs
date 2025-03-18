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

#include "common/platform.h"
#include "mount/tweaks.h"

#include <list>
#include <sstream>

class Variable {
public:
	virtual ~Variable() {}
	virtual void setValue(const std::string& value) = 0;
	virtual std::string getValue() const = 0;
};

template <typename T>
class VariableImpl : public Variable {
public:
	VariableImpl(std::atomic<T>& v) : value_(&v) {}

	void setValue(const std::string& value) override {
		std::stringstream ss(value);
		T t;
		ss >> std::boolalpha >> t;
		if (!ss.fail()) {
			value_->store(t);
		}
	}

	std::string getValue() const override {
		std::stringstream ss;
		ss << std::boolalpha << value_->load();
		return ss.str();
	}

private:
	std::atomic<T>* value_;
};

template <typename T>
std::unique_ptr<Variable> makeVariable(std::atomic<T>& v) {
	return std::unique_ptr<Variable>(new VariableImpl<T>(v));
}

class Tweaks::Impl {
public:
	std::list<std::tuple<std::string, std::unique_ptr<Variable>, std::string>> variables;
};

Tweaks::Tweaks() : impl_(new Impl) {}

Tweaks::~Tweaks() {}

void Tweaks::registerVariable(const std::string& name, std::atomic<bool>& variable, const std::string optionName) {
	impl_->variables.push_back({name, makeVariable(variable), optionName});
}

void Tweaks::registerVariable(const std::string& name, std::atomic<uint32_t>& variable, const std::string optionName) {
	impl_->variables.push_back({name, makeVariable(variable), optionName});
}

void Tweaks::registerVariable(const std::string& name, std::atomic<uint64_t>& variable, const std::string optionName) {
	impl_->variables.push_back({name, makeVariable(variable), optionName});
}

void Tweaks::setValue(const std::string& name, const std::string& value) {
	for (auto& nameAndVariable : impl_->variables) {
		if (std::get<0>(nameAndVariable) == name) {
			std::get<1>(nameAndVariable)->setValue(value);
		}
	}
}

std::string Tweaks::getValue(const std::string& name) const {
	for (const auto& nameAndVariable : impl_->variables) {
		if (std::get<0>(nameAndVariable) == name) {
			return std::get<1>(nameAndVariable)->getValue();
		}
	}
	return std::string();
}

std::string Tweaks::getValeByOptionName(const std::string& optionName) const {
	for (const auto& nameAndVariable : impl_->variables) {
		if (std::get<2>(nameAndVariable) == optionName) {
			return std::get<1>(nameAndVariable)->getValue();
		}
	}
	return std::string();
}

std::string Tweaks::getAllValues() const {
	std::stringstream ss;
	for (const auto& nameAndVariable : impl_->variables) {
		ss << std::get<0>(nameAndVariable) << "\t" << std::get<1>(nameAndVariable)->getValue() << "\n";
	}
	return ss.str();
}
