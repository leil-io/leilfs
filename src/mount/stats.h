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

#include <inttypes.h>
#include <vector>

struct statsnode {
	uint64_t counter;
	uint8_t active;
	uint8_t absolute;
	char *name;
	char *fullname;
	uint32_t nleng; // : strlen(name)
	uint32_t fnleng; // : strlen(fullname)
	struct statsnode *firstchild;
	struct statsnode *nextsibling;
};

statsnode* stats_get_subnode(statsnode *node, const char *name, uint8_t absolute);
uint64_t* stats_get_counterptr(statsnode *node);
void stats_reset_all(void);
void stats_show_all(char **buff, uint32_t *leng);
void stats_term(void);
void stats_inc(uint8_t id, std::vector<uint64_t *> &statsptr, uint64_t inc = 1);
void stats_dec(uint8_t id, std::vector<uint64_t *> &statsptr, uint64_t dec = 1);
