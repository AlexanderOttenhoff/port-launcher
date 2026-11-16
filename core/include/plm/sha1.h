#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace plm {

// Incremental SHA-1, used to identify ROMs (not for security).
class Sha1 {
public:
    using Digest = std::array<std::uint8_t, 20>;

    Sha1();
    void update(std::span<const std::uint8_t> data);
    // Finalises and returns the digest. The object must not be updated afterwards.
    Digest finish();

private:
    void process_block(const std::uint8_t* block);

    std::array<std::uint32_t, 5> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

std::string to_hex(std::span<const std::uint8_t> bytes);
std::string sha1_hex(std::span<const std::uint8_t> data);

} // namespace plm
