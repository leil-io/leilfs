/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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
#include <type_traits>

#include <common/type_defs.h>

/* SFS data pack */

static inline void put64bit(uint8_t **ptr,uint64_t val) {
	(*ptr)[0]=((val)>>56)&0xFF;
	(*ptr)[1]=((val)>>48)&0xFF;
	(*ptr)[2]=((val)>>40)&0xFF;
	(*ptr)[3]=((val)>>32)&0xFF;
	(*ptr)[4]=((val)>>24)&0xFF;
	(*ptr)[5]=((val)>>16)&0xFF;
	(*ptr)[6]=((val)>>8)&0xFF;
	(*ptr)[7]=(val)&0xFF;
	(*ptr)+=8;
}

template <typename T>
static inline void put32bit(uint8_t **ptr, T val) {
	static_assert(sizeof(T) <= 4, "put32bit only accepts types up to 4 bytes size");
	(*ptr)[0]=((val)>>24)&0xFF;
	(*ptr)[1]=((val)>>16)&0xFF;
	(*ptr)[2]=((val)>>8)&0xFF;
	(*ptr)[3]=(val)&0xFF;
	(*ptr)+=4;
}

template <typename T>
static inline void putINode(uint8_t **ptr, T val) {
	static_assert(sizeof(T) == kinode_t_size, "putINode only accepts types with sizeof(inode_t)");

#ifdef SAUNAFS_USE_INODE64
	put64bit(ptr, val);
#else
	put32bit(ptr, val);
#endif  // SAUNAFS_USE_INODE64
}

static inline void put16bit(uint8_t **ptr,uint16_t val) {
	(*ptr)[0]=((val)>>8)&0xFF;
	(*ptr)[1]=(val)&0xFF;
	(*ptr)+=2;
}

static inline void put8bit(uint8_t **ptr,uint8_t val) {
	(*ptr)[0]=(val)&0xFF;
	(*ptr)++;
}

// Overload for enum classes with underlying uint8_t
template <typename E>
    requires std::is_enum_v<E> && std::is_same_v<std::underlying_type_t<E>, uint8_t>
static inline void put8bit(uint8_t **ptr, E val) {
	put8bit(ptr, static_cast<uint8_t>(val));
}

static inline uint64_t get64bit(const uint8_t **ptr) {
	uint64_t t64;
	t64=((*ptr)[3]+256U*((*ptr)[2]+256U*((*ptr)[1]+256U*(*ptr)[0])));
	t64<<=32;
	t64|=(uint32_t)(((*ptr)[7]+256U*((*ptr)[6]+256U*((*ptr)[5]+256U*(*ptr)[4]))));
	(*ptr)+=8;
	return t64;
}

template <typename T>
static inline void get32bit(const uint8_t **ptr, T &output) {
	static_assert(sizeof(T) == 4, "get32bit only accepts types of 4 bytes size");
	output = ((*ptr)[3] + 256U * ((*ptr)[2] + 256U * ((*ptr)[1] + 256U * (*ptr)[0])));
	(*ptr) += sizeof(uint32_t);
}

template <typename T>
static inline void getINode(const uint8_t **ptr, T &output) {
	static_assert(sizeof(T) == kinode_t_size, "getINode only accepts types with sizeof(inode_t)");

#ifdef SAUNAFS_USE_INODE64
	output = get64bit(ptr);
#else
	get32bit(ptr, output);
#endif  // SAUNAFS_USE_INODE64
}

static inline uint16_t get16bit(const uint8_t **ptr) {
	uint32_t t16;
	t16=(*ptr)[1]+256U*(*ptr)[0];
	(*ptr)+=2;
	return t16;
}

static inline uint8_t get8bit(const uint8_t **ptr) {
	uint32_t t8;
	t8=(*ptr)[0];
	(*ptr)++;
	return t8;
}
