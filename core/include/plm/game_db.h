#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace plm {

// A game the launcher knows how to run, from the module manifests (MODULE_SPEC §3).
struct GameRecord {
    std::string id;       // e.g. "n64.jet-force-gemini.us-1.0"
    std::string title;
    std::string region;
    std::string revision;
    std::vector<std::string> sha1;
    std::vector<std::string> modules; // candidate module ids, in the order they were added
};

// The union of the games in all bundled (later: installed) module manifests.
class GameDb {
public:
    // Adds the games of one manifest.json. Returns false and sets error if it can't be used.
    bool add_manifest(std::string_view manifest_json, std::string& error);

    const GameRecord* find_by_sha1(std::string_view sha1) const;
    const GameRecord* find_by_id(std::string_view id) const;
    const std::vector<GameRecord>& games() const { return games_; }

private:
    std::vector<GameRecord> games_;
    std::unordered_map<std::string, std::size_t> by_sha1_;
    std::unordered_map<std::string, std::size_t> by_id_;
};

} // namespace plm
