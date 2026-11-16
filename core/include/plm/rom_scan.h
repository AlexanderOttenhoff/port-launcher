#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plm/byte_source.h"
#include "plm/n64_header.h"
#include "plm/rom_format.h"

namespace plm {

// One N64 ROM found in a file: the file itself, or one entry of a zip.
struct RomInfo {
    std::string zip_entry;          // empty for a plain ROM file
    RomByteOrder original_order = RomByteOrder::Unknown;
    std::uint64_t size = 0;
    std::string sha1;               // of the big-endian (.z64) image
    N64Header header;
};

struct ScanResult {
    std::vector<RomInfo> roms;      // empty if the file holds no N64 ROM
    std::string error;              // set when the file couldn't be read
};

// True for file names worth opening: .z64 .v64 .n64 .zip (case-insensitive).
bool is_candidate_file(std::string_view file_name);

// Identifies the N64 ROM(s) in a file by reading all of it. Plain ROMs are recognised by their header
// in any byte order; in zips, every entry with an N64 header counts.
ScanResult scan_file(ByteSource& source, std::string_view file_name);

// Writes the ROM as big-endian .z64 to out_path (a zip entry when zip_entry isn't empty) and returns its
// SHA-1, or nullopt on failure. The launcher imports ROMs this way before a game's first launch.
std::optional<std::string> export_rom(ByteSource& source, const std::string& zip_entry, const std::string& out_path);

} // namespace plm
