#pragma once
#include <cstdint>

namespace fdb::schema {

// clang-format off
constexpr uint8_t CHANGELOG            = 0x01;
constexpr uint8_t CHANGELOG_DATA_ENTRY = 0x00;
constexpr uint8_t CHANGELOG_META_INDEX = 0xFF;

constexpr uint8_t METADATA_PREFIX  = 0x02;
constexpr uint8_t USERS_PREFIX     = 0x03;
// clang-format on

}  // namespace fdb::schema
