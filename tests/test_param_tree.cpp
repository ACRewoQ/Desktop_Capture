#include "dc/protocol/param_tree.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace dc::proto;

TEST_CASE("removeSubtree removes only the exact subtree", "[tree]") {
    ParamTree t;
    t.set("/track/1/volume_db", 0.0);
    t.set("/track/1/out/1/target", std::string("master"));
    t.set("/track/10/volume_db", 0.0);
    t.set("/track/11/volume_db", 0.0);
    t.set("/track/1x", true); // not a real path, but must survive
    CHECK(t.removeSubtree("/track/1") == 2);
    CHECK(t.contains("/track/10/volume_db"));
    CHECK(t.contains("/track/11/volume_db"));
    CHECK(t.contains("/track/1x"));
    CHECK_FALSE(t.contains("/track/1/volume_db"));
}

TEST_CASE("deltas apply in order, stale ones are ignored and gaps detected", "[tree]") {
    ParamTree t;
    t.load(Snapshot{5, {{"/a", std::int64_t{1}}, {"/b/1", true}, {"/b/2", true}}});
    CHECK(t.rev() == 5);

    CHECK(t.apply(Delta{6, 0, "", {{"/a", std::int64_t{2}}}, {}}) == ParamTree::ApplyResult::Applied);
    CHECK(t.getAs<std::int64_t>("/a") == 2);

    CHECK(t.apply(Delta{6, 0, "", {{"/a", std::int64_t{3}}}, {}}) == ParamTree::ApplyResult::Stale);
    CHECK(t.getAs<std::int64_t>("/a") == 2);

    CHECK(t.apply(Delta{8, 0, "", {{"/a", std::int64_t{4}}}, {}}) == ParamTree::ApplyResult::Gap);
    CHECK(t.rev() == 6);

    // removals happen before changes within one delta
    CHECK(t.apply(Delta{7, 0, "", {{"/b/1", false}}, {"/b"}}) == ParamTree::ApplyResult::Applied);
    CHECK(t.getAs<bool>("/b/1") == false);
    CHECK_FALSE(t.contains("/b/2"));
}

TEST_CASE("snapshot round trip keeps everything", "[tree]") {
    ParamTree t;
    t.set("/z", std::string("last"));
    t.set("/a", 1.0);
    t.setRev(9);
    ParamTree u;
    u.load(t.toSnapshot());
    CHECK(u == t);
    CHECK(t.toSnapshot().params.front().path == "/a"); // sorted
}

TEST_CASE("pathsUnder lists a subtree", "[tree]") {
    ParamTree t;
    t.set("/track/2/a", true);
    t.set("/track/2/b/c", true);
    t.set("/track/20/a", true);
    CHECK(t.pathsUnder("/track/2") == std::vector<std::string>{"/track/2/a", "/track/2/b/c"});
}
