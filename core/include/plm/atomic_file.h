// Atomic file writes for module shims (SLOP_AUDIT §1.4). A shim that writes a port's config while the player can
// quit at any moment must never leave a half-written file: the port would then fail to load its settings, or reset
// them. Write to a unique temporary file next to the target, flush it to disk, then rename it over the target; the
// rename is atomic on the same filesystem, so the target is always either the old file or the new one.
#pragma once

#include <cstdio>
#include <string>
#include <unistd.h>

namespace plm {

// Returns false (leaving the old file in place) if anything fails; callers log it.
inline bool write_file_atomic(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".plm-tmp." + std::to_string(::getpid());
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const bool written = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0 &&
                         ::fsync(::fileno(f)) == 0;
    if (std::fclose(f) != 0 || !written) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace plm
