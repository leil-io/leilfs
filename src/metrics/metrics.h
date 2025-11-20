/*

   Copyright 2025 Leil Storage OÜ

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

#include <endian.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>
#include <limits>

namespace metrics {

template <typename E> struct enum_traits;   // to be specialized by the app

template <typename E>
constexpr std::string_view to_string(E enu) {
    static_assert(std::is_enum_v<E>, "to_string only works on enums");

    using traits = enum_traits<E>;
    const auto idx = static_cast<std::size_t>(enu);

    if (idx < traits::names.size()) {
        return traits::names[idx];
    }
    return "Unknown";
}

template<typename E>
struct Metrics {
public:
	static_assert(std::is_enum_v<E>, "Metrics<E> requires an enum type");
    static constexpr size_t count = static_cast<std::size_t>(E::Count);
	static_assert(std::numeric_limits<double>::is_iec559, "Need IEEE-754 double");
	static_assert(sizeof(double) == 8, "Need 64-bit double");
	static constexpr auto &metricNames = enum_traits<E>::names;

	void incrementMetric(E metric, double value = 1) {
		assert(static_cast<size_t>(metric) < count);
		metricStats[static_cast<size_t>(metric)] += value;
	}

	void setMetric(E metric, double value) {
		assert(static_cast<size_t>(metric) < count);
		metricStats[static_cast<size_t>(metric)] = value;
	}

	double getMetric(E metric) {
		assert(static_cast<size_t>(metric) < count);
		return metricStats[static_cast<size_t>(metric)];
	}

	std::vector<unsigned char> getMetricsBinary() {
		std::vector<unsigned char> output;
		for (size_t i = 0; i < count; i++) {
			auto name = metricNames[i];
			// std::cout << name << '\n';
			auto nameLength = name.size();

			uint64_t len_be = htobe64(nameLength);
			auto len_bytes = reinterpret_cast<const char *>(&len_be);
			output.insert(output.end(), len_bytes, len_bytes + sizeof(len_be));
			output.insert(output.end(), name.data(), name.data() + name.length());
			output.push_back('\0');

			uint64_t value_be = htobe64(metricStats[i]);
			auto value_bytes = reinterpret_cast<const char *>(&value_be);
			output.insert(output.end(), value_bytes, value_bytes + sizeof(value_be));
		}
		return output;
	}

private:
	std::array<double, count> metricStats = {};
};

// 2) Helpers for enum values and names
#define ENUM_VALUE(name) name,
#define ENUM_NAME(name) std::string_view{#name},

// 3) One macro that generates enum + traits from the list macro
#define DEFINE_ENUM_WITH_TRAITS(EnumName, ITEMS_MACRO)                         \
    enum class EnumName : uint8_t {                                            \
        ITEMS_MACRO(ENUM_VALUE)                                                \
        Count,                                                                 \
    };                                                                         \
                                                                               \
    template <>                                                                \
    struct enum_traits<EnumName> {                                             \
        static constexpr std::array<std::string_view,                          \
            static_cast<std::size_t>(EnumName::Count)> names{                  \
            ITEMS_MACRO(ENUM_NAME)                                             \
        };                                                                     \
    }

#define MASTER_METRICS(X)                                                                 \
	X(COUNT_CHUNK_DELETE)              /* Chunk deletion operations */                    \
	X(COUNT_CHUNK_REPLICATE)           /* Chunk replication operations */                 \
	X(COUNT_FS_STATFS)                 /* Filesystem STATFS operations */                 \
	X(COUNT_FS_GETATTR)                /* Filesystem GETATTR operations */                \
	X(COUNT_FS_SETATTR)                /* Filesystem SETATTR operations */                \
	X(COUNT_FS_LOOKUP)                 /* Filesystem LOOKUP operations */                 \
	X(COUNT_FS_MKDIR)                  /* Filesystem MKDIR operations */                  \
	X(COUNT_FS_RMDIR)                  /* Filesystem RMDIR operations */                  \
	X(COUNT_FS_SYMLINK)                /* Filesystem SYMLINK operations */                \
	X(COUNT_FS_READLINK)               /* Filesystem READLINK operations */               \
	X(COUNT_FS_MKNOD)                  /* Filesystem MKNOD operations */                  \
	X(COUNT_FS_UNLINK)                 /* Filesystem UNLINK operations */                 \
	X(COUNT_FS_RENAME)                 /* Filesystem RENAME operations */                 \
	X(COUNT_FS_LINK)                   /* Filesystem LINK operations */                   \
	X(COUNT_FS_READDIR)                /* Filesystem READDIR operations */                \
	X(COUNT_FS_OPEN)                   /* Filesystem OPEN operations */                   \
	X(COUNT_FS_READ)                   /* Filesystem READ operations */                   \
	X(COUNT_FS_WRITE)                  /* Filesystem WRITE operations */                  \
	X(COUNT_NETWORK_CLIENT_RX_PACKETS) /* Packets (i.e messages) received from  client */ \
	X(COUNT_NETWORK_CLIENT_TX_PACKETS) /* Packets (i.e messages) sent to client */        \
	X(COUNT_NETWORK_CLIENT_RX_BYTES)   /* Bytes received from client */                   \
	X(COUNT_NETWORK_CLIENT_TX_BYTES)   /* Bytes sent to client */

DEFINE_ENUM_WITH_TRAITS(MasterMetrics, MASTER_METRICS);

static Metrics<MasterMetrics> gMasterMetrics = {};

} // metrics
