#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace plm {

// Fields from the first 0x40 bytes of a big-endian (.z64) N64 ROM.
struct N64Header {
    std::string title;     // internal name, trailing spaces trimmed (e.g. "JET FORCE GEMINI")
    std::string game_code; // 4 characters, e.g. "NJFE" (media, id, region)
    std::uint8_t version = 0;
    std::uint32_t crc1 = 0;
    std::uint32_t crc2 = 0;
};

std::optional<N64Header> parse_n64_header(std::span<const std::uint8_t> header);

} // namespace plm
