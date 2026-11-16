cmake_minimum_required(VERSION 3.22)
project(plm_core VERSION 0.1.0 LANGUAGES CXX)

# Platform-neutral launcher core: no Android/JNI headers or types in here (see docs/ARCHITECTURE.md).

add_library(plm_core STATIC
    src/version.cpp
    src/sha1.cpp
    src/rom_format.cpp
    src/byte_source.cpp
    src/zip.cpp
    src/n64_header.cpp
    src/rom_scan.cpp
    src/game_db.cpp
)
target_include_directories(plm_core PUBLIC include third_party)

find_package(ZLIB REQUIRED) # the NDK provides it on Android
target_link_libraries(plm_core PRIVATE ZLIB::ZLIB)
target_compile_features(plm_core PUBLIC cxx_std_20)
target_compile_definitions(plm_core PRIVATE PLM_CORE_VERSION="${PROJECT_VERSION}")
set_target_properties(plm_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plm_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Host-run unit tests for platform-neutral code. Not a supported build target.
option(PLM_CORE_TESTS "Build the core unit tests" OFF)
if(PLM_CORE_TESTS)
    enable_testing()
    add_executable(plm_core_tests tests/test_main.cpp tests/test_sha1.cpp tests/test_rom_format.cpp
        tests/test_zip.cpp tests/test_rom_scan.cpp tests/test_game_db.cpp)
    target_link_libraries(plm_core_tests PRIVATE plm_core ZLIB::ZLIB)
    add_test(NAME plm_core_tests COMMAND plm_core_tests)
endif()
