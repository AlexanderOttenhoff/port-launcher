#include "plm/rom_format.h"

#include <utility>

namespace plm {

RomByteOrder detect_rom_byte_order(std::span<const std::uint8_t> header) {
    if (header.size() < 4) {
        return RomByteOrder::Unknown;
    }
    const std::uint32_t magic = (std::uint32_t{header[0]} << 24) | (std::uint32_t{header[1]} << 16) |
                                (std::uint32_t{header[2]} << 8) | std::uint32_t{header[3]};
    switch (magic) {
    case 0x80371240u:
        return RomByteOrder::BigEndian;
    case 0x37804012u:
        return RomByteOrder::ByteSwapped;
    case 0x40123780u:
        return RomByteOrder::LittleEndian;
    default:
        return RomByteOrder::Unknown;
    }
}

bool normalize_rom(std::span<std::uint8_t> data, RomByteOrder order) {
    if (order == RomByteOrder::Unknown || data.size() % 4 != 0) {
        return false;
    }
    switch (order) {
    case RomByteOrder::BigEndian:
        break;
    case RomByteOrder::ByteSwapped:
        for (std::size_t i = 0; i < data.size(); i += 2) {
            std::swap(data[i], data[i + 1]);
        }
        break;
    case RomByteOrder::LittleEndian:
        for (std::size_t i = 0; i < data.size(); i += 4) {
            std::swap(data[i], data[i + 3]);
            std::swap(data[i + 1], data[i + 2]);
        }
        break;
    case RomByteOrder::Unknown:
        return false;
    }
    return true;
}

} // namespace plm
