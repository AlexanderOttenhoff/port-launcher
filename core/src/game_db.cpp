#include "plm/game_db.h"

#include <algorithm>
#include <cctype>

#include "nlohmann/json.hpp"

namespace plm {

namespace {

constexpr int supported_spec = 0;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

bool GameDb::add_manifest(std::string_view manifest_json, std::string& error) {
    const auto manifest = nlohmann::json::parse(manifest_json, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        error = "manifest is not valid JSON";
        return false;
    }
    if (manifest.value("spec", -1) > supported_spec) {
        error = "manifest needs a newer launcher (spec " + std::to_string(manifest.value("spec", -1)) + ")";
        return false;
    }
    const std::string module_id = manifest.value("id", "");
    if (module_id.empty() || !manifest.contains("games") || !manifest["games"].is_array()) {
        error = "manifest has no id or games";
        return false;
    }
    for (const auto& game : manifest["games"]) {
        const std::string id = game.value("id", "");
        if (id.empty()) {
            continue;
        }
        auto [it, inserted] = by_id_.try_emplace(id, games_.size());
        if (inserted) {
            GameRecord record;
            record.id = id;
            record.title = game.value("title", id);
            record.region = game.value("region", "");
            record.revision = game.value("revision", "");
            games_.push_back(std::move(record));
        }
        GameRecord& record = games_[it->second];
        if (std::find(record.modules.begin(), record.modules.end(), module_id) == record.modules.end()) {
            record.modules.push_back(module_id);
        }
        if (game.contains("sha1") && game["sha1"].is_array()) {
            for (const auto& hash : game["sha1"]) {
                if (!hash.is_string()) {
                    continue;
                }
                const std::string sha = lower(hash.get<std::string>());
                if (std::find(record.sha1.begin(), record.sha1.end(), sha) == record.sha1.end()) {
                    record.sha1.push_back(sha);
                }
                by_sha1_.try_emplace(sha, it->second);
            }
        }
    }
    return true;
}

const GameRecord* GameDb::find_by_sha1(std::string_view sha1) const {
    const auto it = by_sha1_.find(lower(std::string(sha1)));
    return it == by_sha1_.end() ? nullptr : &games_[it->second];
}

const GameRecord* GameDb::find_by_id(std::string_view id) const {
    const auto it = by_id_.find(std::string(id));
    return it == by_id_.end() ? nullptr : &games_[it->second];
}

} // namespace plm
