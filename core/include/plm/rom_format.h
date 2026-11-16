#pragma once

#include <cstdint>
#include <span>

namespace plm {

// Byte order of an N64 ROM image, detected from its first four bytes.
enum class RomByteOrder {
    BigEndian,   // .z64, native order (80 37 12 40)
    ByteSwapped, // .v64, 16-bit swapped (37 80 40 12)
    LittleEndian,// .n64, 32-bit swapped (40 12 37 80)
    Unknown,
};

RomByteOrder detect_rom_byte_order(std::span<const std::uint8_t> header);

// Converts a ROM image (or any chunk starting at a 4-byte aligned offset) to big-endian in place.
// Returns false for Unknown order or when the size isn't a multiple of 4.
bool normalize_rom(std::span<std::uint8_t> data, RomByteOrder order);

} // namespace plm
