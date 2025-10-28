/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2019 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <vector>

#include "common/goal.h"
#include "protocol/SFSCommunication.h"

struct ExportEntry {
	uint32_t pleng = 0;
	std::vector<uint8_t> path;  // without '/' at the begin and at the end
	uint32_t fromip = 0;
	uint32_t toip = 0;
	uint32_t minversion = 0;
	std::vector<uint8_t> passworddigest;
	unsigned alldirs : 1;
	unsigned needpassword : 1;
	unsigned meta : 1;
	unsigned rootredefined : 1;
	uint8_t sesflags = SESFLAG_READONLY;
	uint8_t mingoal = GoalId::kMin;
	uint8_t maxgoal = GoalId::kMax;
	uint32_t mintrashtime = 0;
	uint32_t maxtrashtime = UINT32_C(0xFFFFFFFF);
	uint32_t rootuid = 999;
	uint32_t rootgid = 999;
	uint32_t mapalluid = 999;
	uint32_t mapallgid = 999;

	ExportEntry() : alldirs(0), needpassword(0), meta(0), rootredefined(0) {}
	uint32_t serialized_size(uint8_t versmode) const {
		constexpr uint32_t kBaseSize = sizeof(fromip) + sizeof(toip) + sizeof(uint32_t) +
		                               sizeof(uint8_t) + sizeof(minversion) + sizeof(uint8_t) +
		                               sizeof(sesflags) + sizeof(rootuid) + sizeof(rootgid) +
		                               sizeof(mapalluid) + sizeof(mapallgid);
		constexpr uint32_t kExtraSizeWithVersMode =
		    sizeof(mingoal) + sizeof(maxgoal) + sizeof(mintrashtime) + sizeof(maxtrashtime);

		if (meta) { return kBaseSize + ((versmode) ? kExtraSizeWithVersMode : 0); }

		return kBaseSize + ((versmode) ? kExtraSizeWithVersMode : 0) + pleng;
	}
};

uint32_t exports_info_size(uint8_t versmode);
void exports_info_data(uint8_t versmode, uint8_t *buff);
uint8_t exports_check(uint32_t ip, uint32_t version, uint8_t meta,
		const uint8_t *path, const uint8_t rndcode[32],
		const uint8_t *passcode, uint8_t *sesflags,
		uint32_t *rootuid, uint32_t *rootgid, uint32_t *mapalluid,
		uint32_t *mapallgid, uint8_t *mingoal, uint8_t *maxgoal,
		uint32_t *mintrashtime, uint32_t
		*maxtrashtime);
int exports_init();
