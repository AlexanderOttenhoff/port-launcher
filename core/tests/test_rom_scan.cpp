#include "test.h"
#include "test_util.h"

#include "plm/byte_source.h"
#include "plm/rom_scan.h"
#include "plm/sha1.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

using namespace plm_test;

TEST_CASE("plain ROMs in all byte orders scan to the same z64 SHA-1") {
    const auto z64 = make_rom(0x3000, "JET FORCE GEMINI", "NJFE");
    const std::string expected = plm::sha1_hex(z64);
    for (const auto& [name, bytes, order] : {std::tuple{"a.z64", z64, plm::RomByteOrder::BigEndian},
                                             std::tuple{"a.v64", to_v64(z64), plm::RomByteOrder::ByteSwapped},
                                             std::tuple{"a.n64", to_n64(z64), plm::RomByteOrder::LittleEndian}}) {
        MemorySource source(bytes);
        const auto result = plm::scan_file(source, name);
        CHECK_EQ(result.roms.size(), std::size_t{1});
        CHECK_EQ(result.roms[0].sha1, expected);
        CHECK(result.roms[0].original_order == order);
        CHECK_EQ(result.roms[0].header.title, std::string("JET FORCE GEMINI"));
        CHECK_EQ(result.roms[0].header.game_code, std::string("NJFE"));
        CHECK(result.roms[0].zip_entry.empty());
    }
}

TEST_CASE("ROMs inside zips are found, other entries ignored") {
    const auto rom = make_rom(0x4000);
    MemorySource source(make_zip({{"readme.txt", {'x', 'y', 'z', 'w'}, true}, {"Game (USA).v64", to_v64(rom), true}}));
    const auto result = plm::scan_file(source, "Game (USA).ZIP");
    CHECK(result.error.empty());
    CHECK_EQ(result.roms.size(), std::size_t{1});
    CHECK_EQ(result.roms[0].sha1, plm::sha1_hex(rom));
    CHECK_EQ(result.roms[0].zip_entry, std::string("Game (USA).v64"));
}

TEST_CASE("non-ROM files give no ROMs") {
    std::vector<std::uint8_t> junk(0x2000, 0x41);
    MemorySource source(junk);
    CHECK(plm::scan_file(source, "fake.z64").roms.empty());
    MemorySource bad_zip(junk);
    CHECK(!plm::scan_file(bad_zip, "fake.zip").error.empty());
}

TEST_CASE("candidate file names") {
    CHECK(plm::is_candidate_file("Mario.Z64"));
    CHECK(plm::is_candidate_file("x.v64"));
    CHECK(plm::is_candidate_file("x.n64"));
    CHECK(plm::is_candidate_file("x (USA).zip"));
    CHECK(!plm::is_candidate_file("x.7z"));
    CHECK(!plm::is_candidate_file("notes.txt"));
}

TEST_CASE("export writes a big-endian ROM from a zip") {
    const auto rom = make_rom(0x5000);
    const auto dir = std::filesystem::temp_directory_path() / "plm_core_tests";
    std::filesystem::create_directories(dir);
    const auto zip_path = dir / "game.zip";
    {
        const auto zip = make_zip({{"game.n64", to_n64(rom), true}});
        std::ofstream(zip_path, std::ios::binary).write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
    }
    auto source = plm::FdByteSource::open(zip_path.string());
    CHECK(source != nullptr && source->valid());
    const auto out_path = (dir / "rom.z64").string();
    const auto sha = plm::export_rom(*source, "game.n64", out_path);
    CHECK(sha.has_value() && *sha == plm::sha1_hex(rom));
    std::ifstream in(out_path, std::ios::binary);
    const std::vector<std::uint8_t> written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(written == rom);
    CHECK(!plm::export_rom(*source, "missing.z64", out_path + "2").has_value());
    std::filesystem::remove_all(dir);
}

// Optional integration test over a real ROM folder: PLM_TEST_ROM_DIR=roms/n64.
TEST_CASE("scan a real ROM folder (PLM_TEST_ROM_DIR, optional)") {
    const char* dir = std::getenv("PLM_TEST_ROM_DIR");
    if (dir == nullptr) {
        std::cout << "  (skipped: PLM_TEST_ROM_DIR not set)\n";
        return;
    }
    int files = 0, roms = 0, empty = 0, errors = 0;
    std::map<std::string, std::string> by_sha;
    for (const auto& item : std::filesystem::directory_iterator(dir)) {
        const std::string name = item.path().filename().string();
        if (!item.is_regular_file() || !plm::is_candidate_file(name)) continue;
        ++files;
        auto source = plm::FdByteSource::open(item.path().string());
        CHECK(source != nullptr && source->valid());
        const auto result = plm::scan_file(*source, name);
        if (!result.error.empty()) { ++errors; std::cout << "  error: " << name << ": " << result.error << "\n"; }
        if (result.roms.empty()) ++empty;
        for (const auto& rom : result.roms) {
            ++roms;
            by_sha[rom.sha1] = name;
        }
    }
    std::cout << "  " << files << " files, " << roms << " ROMs, " << empty << " without a ROM, " << errors << " errors\n";
    // Jet Force Gemini (USA), the reference module's ROM.
    CHECK(by_sha.count("493ced9008dbe932d6e91179b68e8630cf23a023") == 1);
}
