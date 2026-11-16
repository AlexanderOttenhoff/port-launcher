#include "plm/sha1.h"

#include <bit>

namespace plm {

Sha1::Sha1() : state_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u} {}

void Sha1::update(std::span<const std::uint8_t> data) {
    total_bytes_ += data.size();
    std::size_t i = 0;
    if (buffered_ > 0) {
        while (buffered_ < buffer_.size() && i < data.size()) {
            buffer_[buffered_++] = data[i++];
        }
        if (buffered_ < buffer_.size()) {
            return;
        }
        process_block(buffer_.data());
        buffered_ = 0;
    }
    for (; i + 64 <= data.size(); i += 64) {
        process_block(data.data() + i);
    }
    while (i < data.size()) {
        buffer_[buffered_++] = data[i++];
    }
}

Sha1::Digest Sha1::finish() {
    const std::uint64_t bit_length = total_bytes_ * 8;
    const std::uint8_t pad_start = 0x80;
    update({&pad_start, 1});
    const std::uint8_t zero = 0;
    while (buffered_ != 56) {
        update({&zero, 1});
    }
    std::array<std::uint8_t, 8> length_bytes{};
    for (int i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (56 - 8 * i));
    }
    update(length_bytes);

    Digest digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        for (int b = 0; b < 4; ++b) {
            digest[i * 4 + b] = static_cast<std::uint8_t>(state_[i] >> (24 - 8 * b));
        }
    }
    return digest;
}

void Sha1::process_block(const std::uint8_t* block) {
    std::array<std::uint32_t, 80> w{};
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16) |
               (std::uint32_t{block[i * 4 + 2]} << 8) | std::uint32_t{block[i * 4 + 3]};
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    auto [a, b, c, d, e] = state_;
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f;
        std::uint32_t k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const std::uint32_t temp = std::rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = std::rotl(b, 30);
        b = a;
        a = temp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

std::string sha1_hex(std::span<const std::uint8_t> data) {
    Sha1 sha;
    sha.update(data);
    const auto digest = sha.finish();
    return to_hex(digest);
}

} // namespace plm
