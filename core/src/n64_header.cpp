#include "plm/n64_header.h"

namespace plm {

namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

std::string printable(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (std::uint8_t b : bytes) {
        out.push_back(b >= 0x20 && b < 0x7F ? static_cast<char>(b) : ' ');
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

} // namespace

std::optional<N64Header> parse_n64_header(std::span<const std::uint8_t> header) {
    if (header.size() < 0x40 || be32(header.data()) != 0x80371240u) {
        return std::nullopt;
    }
    N64Header h;
    h.crc1 = be32(&header[0x10]);
    h.crc2 = be32(&header[0x14]);
    h.title = printable(header.subspan(0x20, 20));
    h.game_code = printable(header.subspan(0x3B, 4));
    h.version = header[0x3F];
    return h;
}

} // namespace plm
