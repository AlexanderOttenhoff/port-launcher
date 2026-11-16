#include "plm/rom_scan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>

#include "plm/sha1.h"
#include "plm/zip.h"

namespace plm {

namespace {

constexpr std::size_t chunk_size = 256 * 1024;
constexpr std::uint64_t max_rom_size = 128ull * 1024 * 1024; // largest commercial N64 ROM is 64 MB

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Receives a ROM image in arbitrary chunks, converts it to big-endian and hands aligned chunks on.
// The byte order comes from the first four bytes.
class RomStream {
public:
    explicit RomStream(std::function<bool(std::span<const std::uint8_t>)> on_normalized = nullptr)
        : on_normalized_(std::move(on_normalized)) {}

    bool feed(std::span<const std::uint8_t> data) {
        pending_.insert(pending_.end(), data.begin(), data.end());
        if (order_ == RomByteOrder::Unknown && !decided_) {
            if (pending_.size() < 4) {
                return true;
            }
            order_ = detect_rom_byte_order(pending_);
            decided_ = true;
            if (order_ == RomByteOrder::Unknown) {
                return false; // not an N64 ROM: stop reading
            }
        }
        return flush(false);
    }

    bool finish() { return decided_ && order_ != RomByteOrder::Unknown && flush(true) && pending_.empty(); }

    RomByteOrder order() const { return order_; }
    std::uint64_t size() const { return total_; }
    const std::array<std::uint8_t, 0x40>& header() const { return header_; }
    std::string sha1() { return to_hex(sha_.finish()); }

private:
    bool flush(bool final_chunk) {
        const std::size_t aligned = pending_.size() / 4 * 4;
        if (aligned == 0 && !final_chunk) {
            return true;
        }
        std::span<std::uint8_t> block(pending_.data(), aligned);
        normalize_rom(block, order_);
        if (total_ < header_.size()) {
            const std::size_t n = std::min<std::size_t>(header_.size() - total_, block.size());
            std::copy_n(block.begin(), n, header_.begin() + total_);
        }
        sha_.update(block);
        total_ += block.size();
        if (total_ > max_rom_size) {
            return false;
        }
        if (on_normalized_ && !block.empty() && !on_normalized_(block)) {
            return false;
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(aligned));
        return true;
    }

    std::function<bool(std::span<const std::uint8_t>)> on_normalized_;
    std::vector<std::uint8_t> pending_;
    RomByteOrder order_ = RomByteOrder::Unknown;
    bool decided_ = false;
    std::uint64_t total_ = 0;
    std::array<std::uint8_t, 0x40> header_{};
    Sha1 sha_;
};

// Streams a plain file or a zip entry into a RomStream; returns false if reading failed or it isn't a ROM.
bool stream_rom(ByteSource& source, const ZipEntry* entry, RomStream& stream) {
    if (entry != nullptr) {
        if (entry->uncompressed_size > max_rom_size || entry->uncompressed_size < 0x1000) {
            return false;
        }
        return zip_extract(source, *entry, [&](std::span<const std::uint8_t> data) { return stream.feed(data); }) &&
               stream.finish();
    }
    if (source.size() > max_rom_size || source.size() < 0x1000) {
        return false;
    }
    std::vector<std::uint8_t> buffer(chunk_size);
    for (std::uint64_t offset = 0; offset < source.size();) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, source.size() - offset));
        if (!source.read_at(offset, std::span(buffer).first(n)) || !stream.feed(std::span(buffer).first(n))) {
            return false;
        }
        offset += n;
    }
    return stream.finish();
}

std::optional<RomInfo> identify(ByteSource& source, const ZipEntry* entry) {
    RomStream stream;
    if (!stream_rom(source, entry, stream)) {
        return std::nullopt;
    }
    const auto header = parse_n64_header(stream.header());
    if (!header) {
        return std::nullopt;
    }
    RomInfo info;
    info.zip_entry = entry != nullptr ? entry->name : std::string();
    info.original_order = stream.order();
    info.size = stream.size();
    info.sha1 = stream.sha1();
    info.header = *header;
    return info;
}

} // namespace

bool is_candidate_file(std::string_view file_name) {
    const std::string name = lower(file_name);
    return ends_with(name, ".z64") || ends_with(name, ".v64") || ends_with(name, ".n64") || ends_with(name, ".zip");
}

ScanResult scan_file(ByteSource& source, std::string_view file_name) {
    ScanResult result;
    if (ends_with(lower(file_name), ".zip")) {
        const auto entries = zip_list(source);
        if (!entries) {
            result.error = "not a readable zip";
            return result;
        }
        for (const ZipEntry& entry : *entries) {
            if (auto info = identify(source, &entry)) {
                result.roms.push_back(std::move(*info));
            }
        }
        return result;
    }
    if (auto info = identify(source, nullptr)) {
        result.roms.push_back(std::move(*info));
    }
    return result;
}

std::optional<std::string> export_rom(ByteSource& source, const std::string& zip_entry, const std::string& out_path) {
    const std::string temp_path = out_path + ".part";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::nullopt;
    }
    RomStream stream([&](std::span<const std::uint8_t> data) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(out);
    });

    bool ok;
    if (zip_entry.empty()) {
        ok = stream_rom(source, nullptr, stream);
    } else {
        const auto entries = zip_list(source);
        const auto it = entries ? std::find_if(entries->begin(), entries->end(),
                                               [&](const ZipEntry& e) { return e.name == zip_entry; })
                                : std::vector<ZipEntry>::const_iterator{};
        ok = entries && it != entries->end() && stream_rom(source, &*it, stream);
    }
    out.close();
    if (!ok || !out || std::rename(temp_path.c_str(), out_path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        return std::nullopt;
    }
    return stream.sha1();
}

} // namespace plm
