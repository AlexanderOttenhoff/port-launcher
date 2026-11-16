#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "plm/byte_source.h"

namespace plm {

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0; // 0 = stored, 8 = deflate
    std::uint32_t crc32 = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t local_header_offset = 0;
};

// Reads a zip's central directory. Returns nullopt if the source isn't a (supported) zip.
// ZIP64 and encrypted entries aren't supported; ROM zips don't need them.
std::optional<std::vector<ZipEntry>> zip_list(ByteSource& source);

// Streams an entry's uncompressed bytes to sink in chunks and checks the CRC-32. Stops early, returning
// false, if sink returns false. Supports stored and deflate entries.
bool zip_extract(ByteSource& source, const ZipEntry& entry,
                 const std::function<bool(std::span<const std::uint8_t>)>& sink);

} // namespace plm
