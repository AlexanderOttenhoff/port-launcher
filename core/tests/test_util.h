#pragma once

// Helpers shared by the core tests: in-memory byte sources, synthetic ROMs and zips.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "plm/byte_source.h"

namespace plm_test {

class MemorySource final : public plm::ByteSource {
public:
    explicit MemorySource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    std::uint64_t size() const override { return bytes_.size(); }
    bool read_at(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset > bytes_.size() || out.size() > bytes_.size() - offset) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

// A fake big-endian N64 ROM: valid header, deterministic body.
inline std::vector<std::uint8_t> make_rom(std::size_t size = 0x2000, const char* title = "TEST ROM", const char* code = "NTSE") {
    std::vector<std::uint8_t> rom(size);
    for (std::size_t i = 0; i < size; ++i) {
        rom[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    }
    const std::uint8_t magic[4] = {0x80, 0x37, 0x12, 0x40};
    std::memcpy(rom.data(), magic, 4);
    std::memset(rom.data() + 0x20, ' ', 20);
    std::memcpy(rom.data() + 0x20, title, std::strlen(title));
    std::memcpy(rom.data() + 0x3B, code, 4);
    rom[0x3F] = 1;
    return rom;
}

inline std::vector<std::uint8_t> to_v64(std::vector<std::uint8_t> rom) {
    for (std::size_t i = 0; i < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
    return rom;
}

inline std::vector<std::uint8_t> to_n64(std::vector<std::uint8_t> rom) {
    for (std::size_t i = 0; i < rom.size(); i += 4) {
        std::swap(rom[i], rom[i + 3]);
        std::swap(rom[i + 1], rom[i + 2]);
    }
    return rom;
}

struct ZipInput {
    std::string name;
    std::vector<std::uint8_t> data;
    bool deflate = true;
};

inline void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

inline void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

inline std::vector<std::uint8_t> raw_deflate(const std::vector<std::uint8_t>& in) {
    z_stream z{};
    deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::vector<std::uint8_t> out(deflateBound(&z, static_cast<uLong>(in.size())));
    z.next_in = const_cast<Bytef*>(in.data());
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

// Builds a minimal zip (local headers, central directory, end record).
inline std::vector<std::uint8_t> make_zip(const std::vector<ZipInput>& inputs) {
    std::vector<std::uint8_t> zip;
    std::vector<std::uint8_t> central;
    for (const auto& in : inputs) {
        const std::uint32_t crc = crc32(0, in.data.data(), static_cast<uInt>(in.data.size()));
        const std::vector<std::uint8_t> payload = in.deflate ? raw_deflate(in.data) : in.data;
        const std::uint16_t method = in.deflate ? 8 : 0;
        const auto offset = static_cast<std::uint32_t>(zip.size());
        put32(zip, 0x04034b50); put16(zip, 20); put16(zip, 0); put16(zip, method); put16(zip, 0); put16(zip, 0);
        put32(zip, crc); put32(zip, static_cast<std::uint32_t>(payload.size())); put32(zip, static_cast<std::uint32_t>(in.data.size()));
        put16(zip, static_cast<std::uint16_t>(in.name.size())); put16(zip, 0);
        zip.insert(zip.end(), in.name.begin(), in.name.end());
        zip.insert(zip.end(), payload.begin(), payload.end());

        put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0); put16(central, method);
        put16(central, 0); put16(central, 0); put32(central, crc); put32(central, static_cast<std::uint32_t>(payload.size()));
        put32(central, static_cast<std::uint32_t>(in.data.size())); put16(central, static_cast<std::uint16_t>(in.name.size()));
        put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0); put32(central, 0); put32(central, offset);
        central.insert(central.end(), in.name.begin(), in.name.end());
    }
    const auto cd_offset = static_cast<std::uint32_t>(zip.size());
    zip.insert(zip.end(), central.begin(), central.end());
    put32(zip, 0x06054b50); put16(zip, 0); put16(zip, 0);
    put16(zip, static_cast<std::uint16_t>(inputs.size())); put16(zip, static_cast<std::uint16_t>(inputs.size()));
    put32(zip, static_cast<std::uint32_t>(central.size())); put32(zip, cd_offset); put16(zip, 0);
    return zip;
}

} // namespace plm_test
