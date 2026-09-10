/*

   Copyright 2024 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
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
