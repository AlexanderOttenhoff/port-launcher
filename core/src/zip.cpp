#include "plm/zip.h"

#include <algorithm>
#include <array>

#include <zlib.h>

namespace plm {

namespace {

constexpr std::uint32_t eocd_signature = 0x06054b50;
constexpr std::uint32_t central_signature = 0x02014b50;
constexpr std::uint32_t local_signature = 0x04034b50;
constexpr std::size_t chunk_size = 256 * 1024;

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
           (std::uint32_t{p[3]} << 24);
}

} // namespace

std::optional<std::vector<ZipEntry>> zip_list(ByteSource& source) {
    const std::uint64_t size = source.size();
    if (size < 22) {
        return std::nullopt;
    }
    // The end-of-central-directory record is in the last 22 + 65535 (comment) bytes.
    const std::uint64_t tail_size = std::min<std::uint64_t>(size, 22 + 65535);
    std::vector<std::uint8_t> tail(tail_size);
    if (!source.read_at(size - tail_size, tail)) {
        return std::nullopt;
    }
    std::optional<std::size_t> eocd;
    for (std::size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (le32(&tail[i]) == eocd_signature) {
            eocd = i;
            break;
        }
    }
    if (!eocd) {
        return std::nullopt;
    }
    const std::uint8_t* e = &tail[*eocd];
    const std::uint16_t entry_count = le16(e + 10);
    const std::uint32_t cd_size = le32(e + 12);
    const std::uint32_t cd_offset = le32(e + 16);
    if (entry_count == 0xFFFF || cd_offset == 0xFFFFFFFFu || std::uint64_t{cd_offset} + cd_size > size) {
        return std::nullopt; // ZIP64 or corrupt
    }

    std::vector<std::uint8_t> cd(cd_size);
    if (!source.read_at(cd_offset, cd)) {
        return std::nullopt;
    }
    std::vector<ZipEntry> entries;
    std::size_t pos = 0;
    for (std::uint16_t i = 0; i < entry_count; ++i) {
        if (pos + 46 > cd.size() || le32(&cd[pos]) != central_signature) {
            return std::nullopt;
        }
        const std::uint8_t* c = &cd[pos];
        const std::uint16_t flags = le16(c + 8);
        const std::uint16_t name_len = le16(c + 28);
        const std::uint16_t extra_len = le16(c + 30);
        const std::uint16_t comment_len = le16(c + 32);
        if (pos + 46 + name_len > cd.size()) {
            return std::nullopt;
        }
        ZipEntry entry;
        entry.method = le16(c + 10);
        entry.crc32 = le32(c + 16);
        entry.compressed_size = le32(c + 20);
        entry.uncompressed_size = le32(c + 24);
        entry.local_header_offset = le32(c + 42);
        entry.name.assign(reinterpret_cast<const char*>(c + 46), name_len);
        const bool encrypted = (flags & 1) != 0;
        const bool is_directory = !entry.name.empty() && entry.name.back() == '/';
        if (!encrypted && !is_directory) {
            entries.push_back(std::move(entry));
        }
        pos += 46 + name_len + extra_len + comment_len;
    }
    return entries;
}

bool zip_extract(ByteSource& source, const ZipEntry& entry,
                 const std::function<bool(std::span<const std::uint8_t>)>& sink) {
    std::array<std::uint8_t, 30> local{};
    if (!source.read_at(entry.local_header_offset, local) || le32(local.data()) != local_signature) {
        return false;
    }
    const std::uint64_t data_offset = entry.local_header_offset + 30 + le16(&local[26]) + le16(&local[28]);
    if (data_offset > source.size() || entry.compressed_size > source.size() - data_offset) {
        return false;
    }

    std::vector<std::uint8_t> in(chunk_size);
    std::vector<std::uint8_t> out(chunk_size);
    uLong crc = crc32(0, nullptr, 0);
    std::uint64_t produced = 0;

    if (entry.method == 0) {
        for (std::uint64_t done = 0; done < entry.compressed_size;) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, entry.compressed_size - done));
            if (!source.read_at(data_offset + done, std::span(in).first(n))) {
                return false;
            }
            crc = crc32(crc, in.data(), static_cast<uInt>(n));
            if (!sink(std::span<const std::uint8_t>(in).first(n))) {
                return false;
            }
            done += n;
        }
        produced = entry.compressed_size;
    } else if (entry.method == 8) {
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
            return false;
        }
        std::uint64_t consumed = 0;
        int status = Z_OK;
        bool ok = true;
        while (ok && status != Z_STREAM_END) {
            if (z.avail_in == 0 && consumed < entry.compressed_size) {
                const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, entry.compressed_size - consumed));
                if (!source.read_at(data_offset + consumed, std::span(in).first(n))) {
                    ok = false;
                    break;
                }
                consumed += n;
                z.next_in = in.data();
                z.avail_in = static_cast<uInt>(n);
            }
            z.next_out = out.data();
            z.avail_out = static_cast<uInt>(out.size());
            status = inflate(&z, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) {
                ok = false;
                break;
            }
            const std::size_t n = out.size() - z.avail_out;
            if (n > 0) {
                crc = crc32(crc, out.data(), static_cast<uInt>(n));
                produced += n;
                if (!sink(std::span<const std::uint8_t>(out).first(n))) {
                    ok = false;
                }
            } else if (z.avail_in == 0 && consumed >= entry.compressed_size && status != Z_STREAM_END) {
                ok = false; // truncated stream
            }
        }
        inflateEnd(&z);
        if (!ok) {
            return false;
        }
    } else {
        return false;
    }
    return produced == entry.uncompressed_size && static_cast<std::uint32_t>(crc) == entry.crc32;
}

} // namespace plm
