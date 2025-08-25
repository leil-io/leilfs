/*
   Copyright 2025      Leil Storage OÜ

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

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

struct CapabilityVersion {
	uint16_t minimum;
	uint16_t latest;
};

using CapabilityList = std::map<std::string, CapabilityVersion>;

class InvalidCapString : std::exception {
public:
	const char* what() const noexcept override
	{
		return "Invalid capabilities format";
	}
};

class WrongCapVersion: std::exception {
public:
	const char* what() const noexcept override
	{
		return "Minimum greater than latest version";
	}
};

class Capabilities {
public:
	Capabilities(CapabilityList capabilities) : capabilities(std::move(capabilities)) {}
	Capabilities(const std::string& capabilitiesText) : capabilities({}) {
		auto strStream = std::stringstream(capabilitiesText);
		std::string line;
		while (std::getline(strStream, line)) {
			auto pos = line.find(':');
			if (pos == std::string::npos) {
				throw InvalidCapString();
			}
			auto key = line.substr(0, pos);
			auto start = line.find('(');
			auto end = line.find(')');
			if (start != std::string::npos && end != std::string::npos && end > start) {
				std::string inside = line.substr(start + 1, end - start - 1);
				std::stringstream stream(inside);
				char comma = 0;
				CapabilityVersion version{};

				stream >> version.minimum >> comma >> version.latest;
				if (version.minimum > version.latest) {
					throw WrongCapVersion();
				}
				capabilities.insert({key, version});
			} else {
				throw InvalidCapString();
			}
		}
	}

	bool hasCapability(const std::string &capabilityName, uint16_t version) {
		try {
			auto capabilityVersion = capabilities.at(capabilityName);
			return capabilityVersion.latest >= version && !(capabilityVersion.minimum > version);
		} catch (const std::out_of_range &e) { return false; }
	}

	 std::string toString() {
		const size_t kiloByte = 1024;
		std::string output;
		// Make sure to reserve, so we don't constantly allocate
		output.reserve(4 * kiloByte);
		for (const auto &capability : capabilities) {
			auto minimum = capability.second.minimum;
			auto latest = capability.second.latest;
			output += capability.first + ": (" + std::to_string(minimum) + ", " +
			          std::to_string(latest) + ")\n";
		};
		if (output.size() > 0) {
			// Strip last newline
			output.pop_back();
		}
		return output;
	}

private:
	CapabilityList capabilities;
};
