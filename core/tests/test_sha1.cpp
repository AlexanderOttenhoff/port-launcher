#include "test.h"

#include "plm/sha1.h"
#include "plm/version.h"

#include <string_view>
#include <vector>

namespace {

std::vector<std::uint8_t> bytes(std::string_view s) {
    return {s.begin(), s.end()};
}

} // namespace

TEST_CASE("sha1 of empty input") {
    CHECK_EQ(plm::sha1_hex({}), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
}

TEST_CASE("sha1 of abc") {
    CHECK_EQ(plm::sha1_hex(bytes("abc")), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
}

TEST_CASE("sha1 of two-block message") {
    CHECK_EQ(plm::sha1_hex(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

TEST_CASE("sha1 of a million a's fed in uneven chunks") {
    const std::vector<std::uint8_t> chunk(997, 'a');
    plm::Sha1 sha;
    std::size_t remaining = 1'000'000;
    while (remaining > 0) {
        const std::size_t n = remaining < chunk.size() ? remaining : chunk.size();
        sha.update({chunk.data(), n});
        remaining -= n;
    }
    const auto digest = sha.finish();
    CHECK_EQ(plm::to_hex(digest), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

TEST_CASE("core version is set") {
    CHECK(!plm::core_version().empty());
}
