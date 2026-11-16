#include "test.h"

#include "plm/game_db.h"

namespace {

const char* jfg_manifest = R"({
  "spec": 0, "id": "jfg-recomp",
  "games": [ { "id": "n64.jet-force-gemini.us-1.0", "title": "Jet Force Gemini", "region": "US",
               "revision": "1.0", "sha1": ["493CED9008DBE932D6E91179B68E8630CF23A023"] } ]
})";

} // namespace

TEST_CASE("game db indexes manifest games by id and case-insensitive sha1") {
    plm::GameDb db;
    std::string error;
    CHECK(db.add_manifest(jfg_manifest, error));
    const auto* game = db.find_by_sha1("493ced9008dbe932d6e91179b68e8630cf23a023");
    CHECK(game != nullptr);
    CHECK_EQ(game->title, std::string("Jet Force Gemini"));
    CHECK(game->modules == std::vector<std::string>{"jfg-recomp"});
    CHECK(db.find_by_id("n64.jet-force-gemini.us-1.0") == game);
    CHECK(db.find_by_sha1("0000") == nullptr);
}

TEST_CASE("game db merges modules for the same game") {
    plm::GameDb db;
    std::string error;
    CHECK(db.add_manifest(jfg_manifest, error));
    CHECK(db.add_manifest(R"({"spec":0,"id":"other-port","games":[{"id":"n64.jet-force-gemini.us-1.0","sha1":[]}]})", error));
    CHECK_EQ(db.games().size(), std::size_t{1});
    CHECK((db.games()[0].modules == std::vector<std::string>{"jfg-recomp", "other-port"}));
}

TEST_CASE("game db rejects broken or newer manifests") {
    plm::GameDb db;
    std::string error;
    CHECK(!db.add_manifest("{nope", error));
    CHECK(!db.add_manifest(R"({"spec":99,"id":"x","games":[]})", error));
    CHECK(!db.add_manifest(R"({"spec":0,"games":[]})", error));
    CHECK(db.games().empty());
}
