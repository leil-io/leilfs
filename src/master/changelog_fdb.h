// src/master/changelog_fdb.cc

#pragma once

#include <cstdint>

int changelog_fdb_init();
void changelog_fdb_append(uint64_t version, const char *entry);
