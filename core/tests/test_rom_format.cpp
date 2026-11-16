#include "test.h"

#include "plm/rom_format.h"

#include <array>

namespace {

// First eight bytes of an N64 header in each byte order.
constexpr std::array<std::uint8_t, 8> z64{0x80, 0x37, 0x12, 0x40, 0x00, 0x00, 0x00, 0x0F};
constexpr std::array<std::uint8_t, 8> v64{0x37, 0x80, 0x40, 0x12, 0x00, 0x00, 0x0F, 0x00};
constexpr std::array<std::uint8_t, 8> n64{0x40, 0x12, 0x37, 0x80, 0x0F, 0x00, 0x00, 0x00};

} // namespace

TEST_CASE("detects all three byte orders") {
    CHECK(plm::detect_rom_byte_order(z64) == plm::RomByteOrder::BigEndian);
    CHECK(plm::detect_rom_byte_order(v64) == plm::RomByteOrder::ByteSwapped);
    CHECK(plm::detect_rom_byte_order(n64) == plm::RomByteOrder::LittleEndian);
}

TEST_CASE("rejects unknown or short headers") {
    constexpr std::array<std::uint8_t, 4> junk{'P', 'K', 3, 4};
    CHECK(plm::detect_rom_byte_order(junk) == plm::RomByteOrder::Unknown);
    CHECK(plm::detect_rom_byte_order(std::span(z64).first(3)) == plm::RomByteOrder::Unknown);
}

TEST_CASE("normalizes v64 and n64 to z64") {
    auto a = v64;
    CHECK(plm::normalize_rom(a, plm::RomByteOrder::ByteSwapped));
    CHECK(a == z64);
    auto b = n64;
    CHECK(plm::normalize_rom(b, plm::RomByteOrder::LittleEndian));
    CHECK(b == z64);
    auto c = z64;
    CHECK(plm::normalize_rom(c, plm::RomByteOrder::BigEndian));
    CHECK(c == z64);
}

TEST_CASE("refuses unknown order and unaligned sizes") {
    auto a = z64;
    CHECK(!plm::normalize_rom(a, plm::RomByteOrder::Unknown));
    CHECK(!plm::normalize_rom(std::span(a).first(6), plm::RomByteOrder::ByteSwapped));
}
