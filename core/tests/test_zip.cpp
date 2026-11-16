#include "test.h"
#include "test_util.h"

#include "plm/zip.h"

using namespace plm_test;

namespace {

std::vector<std::uint8_t> extract_all(plm::ByteSource& source, const plm::ZipEntry& entry, bool* ok) {
    std::vector<std::uint8_t> out;
    *ok = plm::zip_extract(source, entry, [&](std::span<const std::uint8_t> d) {
        out.insert(out.end(), d.begin(), d.end());
        return true;
    });
    return out;
}

} // namespace

TEST_CASE("zip lists and extracts stored and deflated entries") {
    const auto big = make_rom(600 * 1024); // larger than one 256 KB chunk
    MemorySource source(make_zip({{"a.txt", {'h', 'i'}, false}, {"dir/rom.z64", big, true}}));
    const auto entries = plm::zip_list(source);
    CHECK(entries.has_value());
    CHECK_EQ(entries->size(), std::size_t{2});
    bool ok = false;
    CHECK(extract_all(source, (*entries)[0], &ok) == std::vector<std::uint8_t>({'h', 'i'}));
    CHECK(ok);
    CHECK(extract_all(source, (*entries)[1], &ok) == big);
    CHECK(ok);
    CHECK_EQ((*entries)[1].name, std::string("dir/rom.z64"));
}

TEST_CASE("zip rejects non-zips and detects corruption") {
    MemorySource not_zip(make_rom());
    CHECK(!plm::zip_list(not_zip).has_value());

    auto bytes = make_zip({{"rom.z64", make_rom(), false}});
    bytes[40] ^= 0xFF; // flip a payload byte of the stored entry
    MemorySource corrupt(bytes);
    const auto entries = plm::zip_list(corrupt);
    CHECK(entries.has_value());
    bool ok = true;
    extract_all(corrupt, (*entries)[0], &ok);
    CHECK(!ok); // CRC mismatch
}

TEST_CASE("zip extraction stops when the sink declines") {
    MemorySource source(make_zip({{"rom.z64", make_rom(600 * 1024), true}}));
    const auto entries = plm::zip_list(source);
    int calls = 0;
    const bool ok = plm::zip_extract(source, (*entries)[0], [&](std::span<const std::uint8_t>) { return ++calls < 2; });
    CHECK(!ok);
    CHECK_EQ(calls, 2);
}
