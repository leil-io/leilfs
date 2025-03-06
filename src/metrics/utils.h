/*

   Copyright 2024 Leil Storage OÜ

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
#ifdef HAVE_PROMETHEUS
#include <prometheus/counter.h>
#include <prometheus/family.h>

inline prometheus::Family<prometheus::Counter> &setupFamily(
    const char *name, const char *help,
    std::shared_ptr<prometheus::Registry> &registry) {
	return prometheus::BuildCounter().Name(name).Help(help).Register(*registry);
}
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define START_COUNTER_SWITCH(type) \
	using type::Counters; \
	type::Counters start = Counters::KEY_START; \
	switch (start) { \
		case Counters::KEY_START: \
		[[fallthrough]];

#define DEFINE_COUNTER_CASE(enum_val, family, ...) \
	case Counters::enum_val:                              \
        counters[static_cast<uint8_t>(Counters::enum_val)] = Counter(__VA_ARGS__, family); \
        [[fallthrough]];

#define END_COUNTER_CASE \
	case Counters::KEY_END: \
		break; \
	}

#define START_GAUGE_SWITCH(type) \
	type::Gauges start = GAUGE_KEY_START; \
	switch (start) { \
    case GAUGE_KEY_START: \
		[[fallthrough]];

#define DEFINE_GAUGE_CASE(enum_val, family, ...) \
    case enum_val:                              \
        counters[enum_val] = Counter(__VA_ARGS__, family); \
        [[fallthrough]];

#define END_GAUGE_CASE \
    case GAUGE_KEY_END: \
		break; \
	}


#define INITIALIZE_FAMILY(type, family, description) \
	family = &setupFamily(TOSTRING(type) "_" TOSTRING(family), description, registry)
