/*
   Copyright 2023      Leil Storage OÜ

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

#include "common/platform.h"

#include "mount/option_casing_normalization.h"

#include <gtest/gtest.h>

TEST(ArgumentCasingTest, LowercaseOptionNamesOnly) {
	char input[] =
	    "sfSWriteCACHESize=4096,sfscacHeperinodePERcentAGE=100,"
	    "SFSCHunKSeRVERwavereadto=2000,sfschunkservertotalreadto=8000,"
	    "MAXreadaheadrequests=1,cacheexpirationtime=10000,READWORKERS=42,"
	    "sfswriteWORKERS=42";
	char expected[] =
	    "sfswritecachesize=4096,sfscacheperinodepercentage=100,"
	    "sfschunkserverwavereadto=2000,sfschunkservertotalreadto=8000,"
	    "maxreadaheadrequests=1,cacheexpirationtime=10000,readworkers=42,"
	    "sfswriteworkers=42";

	normalize_argument_casing(input);

	EXPECT_EQ(strcmp(input, expected), 0);
}
