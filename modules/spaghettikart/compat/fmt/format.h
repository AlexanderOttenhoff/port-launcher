// SpaghettiKart's src/port/pak.cpp formats with <fmt/format.h> when it exists and falls back to std::format, which the
// NDK r26 libc++ doesn't provide. Our libultraship deps have no standalone fmt, only the copy bundled with spdlog
// (build/deps/lus-android-arm64): this shim makes <fmt/format.h> resolve to it (spdlog/fmt/fmt.h picks header-only
// mode for the bundled copy, so nothing extra needs linking). Only on the game target's include path (module.cmake).
#pragma once
#include <spdlog/fmt/fmt.h>
