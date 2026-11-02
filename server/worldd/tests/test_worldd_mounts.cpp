// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using aoc::protocol::emit::FIntrepidNetworkGUID;

static void test_donors(const spawns::MountBook& book) {
    std::printf("the donors\n");
    const spawns::MountDonors& d = book.donors();
    CHECK(d.opens().size() == 3, "three donor opens");
    const spawns::OpenTemplate* ground = d.donor("ground");
    const spawns::OpenTemplate* aquatic = d.donor("aquatic");
    const spawns::OpenTemplate* flying = d.donor("flying");
    CHECK(ground && aquatic && flying && !d.donor("vehicle"), "a donor of each kind of mount, and none of another");
    CHECK(ground && ground->capture == 911 && aquatic && aquatic->capture == 898 && flying && flying->capture == 871, "the donors are the opens of 911, 898 and 871");
    bool layout = true;
    const char* expected[] = {
        "ANPCCharacter", "UAlignmentComponent", "UNPCSecondaryInfo", "UNPCInfo",
        "UNPCCombatInfo", "UAoCAbilityComponent", "UAoCStatsComponent", "UMountComponent", "UInteractableComponent"
    };
    for (const spawns::OpenTemplate* t : {ground, aquatic, flying}) {
        if (!t || t->blocks.size() != std::size(expected)) {
            layout = false;
            continue;
        }
        for (size_t i = 0; i < t->blocks.size(); ++i) layout = layout && t->blocks[i].cls == expected[i];
    }
    CHECK(layout, "each is the NPC layout (actor, alignment, creature info, base info, combat info, ability, stats) with the mount and the interactable component");
    CHECK(ground && ground->archetype_path.find("Mount_Grounded_TerrorBird_0") != std::string::npos && ground->archetype.size() == 2,
          "the ground donor's archetype is the terror bird's, with its package");
    CHECK(book.summon_ability() == 108436548376185ll && book.dash_ability() == 6064631555335526877ll, "Summon_Mount and MountAbility_Sprint are the data's");
}

struct GoldenCase {
    const char* name;
    int         capture;
    const char* kind;
    const char* blueprint;
    int         level;
    double      health;
};

static int match_owner_level(const spawns::MountBook& book, const content::MountRow& row, double health) {
    for (int l = 1; l <= 50; ++l) {
        if (std::fabs(book.max_health(row, l) - health) < 0.5) return l;
    }
    return 0;
}

static const std::map<std::string, std::set<std::string>>& golden_allowed_diffs() {
    static const std::map<std::string, std::set<std::string>> allowed = {
        {"ANPCCharacter", {"ReplicatedBasedMovement.BaseID", "ReplicatedBasedMovement.MovementBase"}},
        {"UAlignmentComponent", {"InheritedAlignments", "Alignments"}},
        {"UInteractableComponent", {"InteractionInputData"}},
        {"UNPCInfo", {"CharacterName"}},
        {"UAoCStatsComponent", {"[StatRepInt32EveryoneProxy]", "[CurrentEffects]"}},
    };
    return allowed;
}

static std::set<std::string> flatten_diff_map(const std::map<std::string, std::set<std::string>>& found) {
    std::set<std::string> all;
    for (const auto& [cls, diff] : found) {
        for (const std::string& x : diff) all.insert(cls + "." + x);
    }
    return all;
}

static void check_golden_block_diffs(
    const spawns::OpenTemplate& donor,
    const content::MountRow& row,
    const repl::Actor& a,
    const std::string& title) {
    const bool same_blocks = a.subobjects.size() + 1 == donor.blocks.size();
    CHECK(same_blocks, (title + ": the actor has the donor's subobjects").c_str());
    std::map<std::string, std::set<std::string>> found;
    found["ANPCCharacter"] = differing(donor.actor_block().state, a.obj);
    for (size_t i = 0; same_blocks && i < a.subobjects.size(); ++i) {
        found[donor.blocks[i + 1].cls] = differing(donor.blocks[i + 1].state, a.subobjects[i].obj);
    }
    if (!donor.actor_block().state.state.get(*donor.actor_block().state.schema, "ReplicatedServerLastTransformUpdateTimeStamp")) {
        found["ANPCCharacter"].erase("ReplicatedServerLastTransformUpdateTimeStamp");
    }
    if (const repl::Subobject* info = sub_named(a, "UNPCInfo"); same_blocks && info) {
        const repl::Value* shown = info->obj.state.get(*info->obj.schema, "CharacterName");
        const std::string* name = shown ? shown->get_if<std::string>() : nullptr;
        CHECK(!row.display_name.empty() && name && *name == row.display_name,
              (title + ": the nameplate shows the mount's own name \"" + row.display_name + "\"").c_str());
    }
    const auto& allowed = golden_allowed_diffs();
    bool exact = true;
    std::string report;
    for (const auto& [cls, diff] : found) {
        const auto it = allowed.find(cls);
        for (const std::string& path : diff) {
            if (it == allowed.end() || !it->second.count(path)) {
                exact = false;
                report += cls + "." + path + " ";
            }
        }
    }
    CHECK(exact, (title + ": every value is the capture's but for the road effect, the based movement and the guild (differing: " +
                  join(flatten_diff_map(found)) + ")").c_str());
    if (!exact) std::printf("      unexpected: %s\n", report.c_str());
}

static void check_golden_stats_and_open(
    const spawns::MountBook& book,
    const spawns::OpenTemplate& donor,
    const spawns::MountParams& p,
    const repl::Actor& a,
    const GoldenCase& c) {
    const std::string title = c.name;
    const spawns::ParsedBlock* donor_stats = nullptr;
    for (const spawns::ParsedBlock& b : donor.blocks) {
        if (b.cls == "UAoCStatsComponent") donor_stats = &b;
    }
    const repl::Subobject* our_stats = sub_named(a, "UAoCStatsComponent");
    if (donor_stats && our_stats) {
        const std::set<int64_t> stat_diff = stat_records_differing(donor_stats->state, our_stats->obj);
        const bool on_road = c.capture == 911;
        CHECK(on_road ? (stat_diff == std::set<int64_t>{spawns::kStatMoveSpeedMult}) : stat_diff.empty(),
              (title + ": its stats are the capture's" + (on_road ? " but for MoveSpeedMult (1.25 on a road, 1 here)" : "")).c_str());
        size_t all = 0;
        const size_t roads = road_effects_in(donor_stats->state, book.data(), &all);
        size_t ours = 0;
        road_effects_in(our_stats->obj, book.data(), &ours);
        CHECK(all >= roads && all >= 1 && ours == 0,
              (title + ": the " + std::to_string(all) + " effect(s) the capture holds (" + std::to_string(roads) +
               " a road's, the rest the owner's place's) are none of ours: a new mount has no effect").c_str());
    }
    const repl::Subobject* mc = sub_named(a, "UMountComponent");
    CHECK(mc && guid_of(mc->obj, "ItemInstanceGuid").a == p.item_guid.a, (title + ": the mount component names the item").c_str());
    CHECK(writes_open(const_cast<repl::Actor&>(a)), (title + ": the open writes").c_str());
}

static void run_open_golden_case(const spawns::MountBook& book, const GoldenCase& c) {
    const spawns::OpenTemplate* donor = book.donors().donor(c.kind);
    const content::MountRow* row = row_of_blueprint(book, c.blueprint);
    const std::string title = c.name;
    if (!donor || !row || donor->capture != c.capture) {
        CHECK(false, (title + ": the donor and the item are there").c_str());
        return;
    }
    auto v = values_of(*donor);
    if (!v) {
        CHECK(false, (title + ": the donor's values read").c_str());
        return;
    }
    spawns::MountParams p = v->p;
    const int level = match_owner_level(book, *row, v->health);
    if (c.level != 0) {
        CHECK(level == c.level,
              (title + ": MaxHealth " + std::to_string(static_cast<int>(v->health)) + " is Mount_Base_HP at owner level " +
               std::to_string(c.level) + " times the factor " + std::to_string(row->health_pct) + " %").c_str());
    } else {
        CHECK(level != 0, (title + ": the capture's MaxHealth is Mount_Base_HP at some owner level times the item's factor").c_str());
    }
    p.owner_level = level ? level : 1;
    p.player_id = 7;
    uint64_t next = 0x3000000;
    const auto minter = [&]() {
        FIntrepidNetworkGUID g;
        g.ObjectId = next;
        next += 2;
        g.ServerId = 23;
        g.Randomizer = 0x11223344u;
        return g;
    };
    std::string error;
    auto made = book.make(*row, p, minter, error);
    if (!made) {
        CHECK(false, (title + ": the mount is made: " + error).c_str());
        return;
    }
    check_golden_block_diffs(*donor, *row, made->actor.actor, title);
    check_golden_stats_and_open(book, *donor, p, made->actor.actor, c);
}

static void test_open_golden(const spawns::MountBook& book) {
    std::printf("the open of a mount made from a donor with the capture's own values is the capture's open\n");
    const GoldenCase cases[] = {
        {"the terror bird of 911", 911, "ground", "Mount_Grounded_TerrorBird_0", 20, 3572},
        {"the shoalskipper of 898", 898, "aquatic", "Mount_Aquatic_Shoalskipper_1_ALT", 0, 0},
        {"the Dawnbreaker of 871", 871, "flying", "Mount_Flying_Dawnbreaker_1", 25, 24375},
    };
    for (const GoldenCase& c : cases) run_open_golden_case(book, c);
}

static void check_made_mount_state(
    const spawns::MountBook& book,
    const content::MountRow& row,
    const spawns::MountParams& p,
    const spawns::MountMade& made) {
    const world::WorldActor& w = made.actor;
    const repl::Value* inst = w.actor.obj.state.get(*w.actor.obj.schema, "Instigator");
    CHECK(inst && inst->get_if<FIntrepidNetworkGUID>() && *inst->get_if<FIntrepidNetworkGUID>() == p.owner_pawn, "the Instigator is the owner's pawn");
    CHECK(!w.actor.obj.state.get(*w.actor.obj.schema, "Owner") && !w.actor.obj.state.get(*w.actor.obj.schema, "Controller"),
          "no Owner and no Controller (they are set when someone rides it)");
    const repl::Subobject* mc = sub_named(w.actor, "UMountComponent");
    const repl::Value* summoning = mc ? mc->obj.state.get(*mc->obj.schema, "bIsSummoning") : nullptr;
    CHECK(summoning && summoning->get_if<bool>() && *summoning->get_if<bool>(), "bIsSummoning is 1 at the open");
    const repl::Subobject* info = sub_named(w.actor, "UNPCInfo");
    CHECK(info && guid_of(info->obj, "CharacterGuid") == made.mount_guid && !(made.mount_guid == repl::GuidValue{}), "the mount has a CharacterGuid of its own");
    const repl::Value* name = info ? info->obj.state.get(*info->obj.schema, "CreatureOwnerName") : nullptr;
    CHECK(name && name->get_if<std::string>() && *name->get_if<std::string>() == "Mounty", "its creature owner name is the owner's");
    const double want_hp = book.max_health(row, 12);
    CHECK(made.max_health == want_hp && want_hp > 1000, "MaxHealth is Mount_Base_HP at the owner's level times the item's factor");
    const auto stats = stat_values(sub_named(w.actor, "UAoCStatsComponent"));
    CHECK(stats.count(content::kGuidHealth) && stats.at(content::kGuidHealth) == want_hp && stats.at(content::kGuidMaxHealth) == want_hp &&
              stats.at(spawns::kStatStealthLevel) == 12 && stats.at(spawns::kStatMoveSpeedMult) == 1.0,
          "Health and MaxHealth are full, StealthLevel is the owner's level, MoveSpeedMult is 1");
}

static void test_guids(const spawns::MountBook& book) {
    std::printf("the NetGUIDs follow the retail pattern\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    spawns::MountParams p = plain_params();
    uint64_t next = 0x5000000;
    const auto minter = [&]() {
        FIntrepidNetworkGUID g;
        g.ObjectId = next;
        next += 2;
        g.ServerId = 60;
        g.Randomizer = 1;
        return g;
    };
    std::string error;
    auto made = row ? book.make(*row, p, minter, error) : std::nullopt;
    CHECK(made.has_value(), "a ground mount is made");
    if (!made) return;
    const uint64_t A = 0x5000000;
    const world::WorldActor& w = made->actor;
    CHECK(w.id == A && w.actor.guid.ObjectId == A && w.kind == world::ActorKind::Mount, "the actor's id is A, its kind Mount");
    CHECK(w.npc.collision_cylinder == A + 2, "the CollisionCylinder is A+2");
    const uint64_t want[] = {A + 4, A + 6, A + 8, A + 10, A + 12, A + 14, A + 18, A + 20};
    bool ids = w.actor.subobjects.size() == std::size(want);
    for (size_t i = 0; ids && i < std::size(want); ++i) ids = w.actor.subobjects[i].guid.ObjectId == want[i];
    CHECK(ids, "the subobjects are A+4, +6, +8, +10, +12, +14 (the stats), then A+18 (the mount component) and A+20 (the interactable component): A+16 is never exported");
    CHECK(w.npc.interact_component == A + 20 && next == A + 22, "the interactable component is A+20 and the actor used 11 ids");
    CHECK(w.never_dormant && w.owner == 3 && w.x == p.x && w.y == p.y && w.z == p.z && w.yaw_degrees == 90, "never dormant, owned by its player, at its place");
    check_made_mount_state(book, *row, p, *made);
    p.health = 100;
    next = 0x6000000;
    auto hurt = book.make(*row, p, minter, error);
    const double want_hp = book.max_health(*row, 12);
    const auto hs = hurt ? stat_values(sub_named(hurt->actor.actor, "UAoCStatsComponent")) : std::map<int64_t, double>{};
    CHECK(hs.count(content::kGuidHealth) && hs.at(content::kGuidHealth) == 100 && hs.at(content::kGuidMaxHealth) == want_hp,
          "a saved Health below the maximum is the mount's Health");
}

static bool donor_archetype_clashes(const spawns::MountBook& book, const repl::ExportChain& a1) {
    bool clash = false;
    for (const auto& donor : book.donors().opens()) {
        for (const repl::ExportLink& l : donor.archetype) {
            clash = clash || (l.guid.ObjectId == a1[0].guid.ObjectId || l.guid.ObjectId == a1[1].guid.ObjectId);
        }
    }
    return clash;
}

static void test_archetypes(const spawns::MountBook& book) {
    std::printf("the archetype of a mount: the captured export or one made from the Blueprint's path\n");
    const spawns::OpenTemplate* ground = book.donors().donor("ground");
    const content::MountRow* terror = row_named(book, "Mount_Grounded_TerrorBird_0");
    CHECK(terror && ground && same_chain(book.archetype_of(*terror, *ground), ground->archetype),
          "the terror bird keeps the export retail sent (its id, the checksum 47039926)");
    const content::MountRow* other = nullptr;
    for (const content::MountRow& r : book.data().mounts) {
        if (r.kind == "ground" && r.blueprint != "Mount_Grounded_TerrorBird_0") {
            other = &r;
            break;
        }
    }
    CHECK(other != nullptr, "another ground mount of the data");
    if (!other || !ground) return;
    const repl::ExportChain a1 = book.archetype_of(*other, *ground);
    const repl::ExportChain a2 = book.archetype_of(*other, *ground);
    CHECK(a1.size() == 2 && same_chain(a1, a2), "a made export is an object and its package, the same at every call (the client keeps what it acknowledged)");
    CHECK(a1.size() == 2 && a1[0].name == "Default__" + other->blueprint + "_C" &&
              a1[1].name == "/Game/GameBlueprints/Mounts/" + other->blueprint &&
              a1[0].has_checksum && a1[0].checksum == 0 && a1[1].checksum == 0,
          "the object is the class default object of the Blueprint by its path, in its package, with checksum 0 (not compared by the client)");
    CHECK(!donor_archetype_clashes(book, a1) && a1.size() == 2 && (a1[0].guid.ObjectId & 1) == 1,
          "its NetGUIDs are static ids (odd) that are none of the captured ones");
    const content::MountRow* gryphon = row_named(book, "Mount_Flying_PTR_ONLY");
    const spawns::OpenTemplate* flying = book.donors().donor("flying");
    CHECK(gryphon && flying && gryphon->blueprint == "Mount_Flying_Gryphon_5_Armored", "the GM Eagle Gryphon is the Armored gryphon's Blueprint");
    if (gryphon && flying) {
        const repl::ExportChain g = book.archetype_of(*gryphon, *flying);
        CHECK(g.size() == 2 && g[0].name == "Default__Mount_Flying_Gryphon_5_Armored_C" && !same_chain(g, flying->archetype),
              "it opens with the flying donor's shape and an export made from its path");
        spawns::MountParams p = plain_params();
        std::string error;
        auto made = book.make(*gryphon, p, mint, error);
        CHECK(made && same_chain(made->actor.actor.open.archetype, g) && sub_named(made->actor.actor, "UMountComponent") != nullptr,
              "the Eagle Gryphon makes an actor with that export");
    }
}

static bool try_mount_speeds_file(const content::ContentDb& db, const std::string& text, std::string& why) {
    const std::string path = "mount_speeds_test_" + std::to_string(g_pass + g_fail) + ".tsv";
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    std::string e;
    auto fresh = spawns::MountBook::load(db, e);
    const bool ok = fresh && fresh->load_speeds(path, why);
    std::remove(path.c_str());
    return ok;
}

static void test_world_loads(const content::ContentDb& db) {
    std::printf("the world loads the mounts\n");
    aoc::protocol::ServerGuidAllocator guids(60, 1);
    PlayerWorld world(&db, "", aoc::worldd::bootstrap::GameStateMode::Tier2, 60, guids);
    CHECK(world.ready() && world.mounts() != nullptr && world.mounts()->data().mounts.size() == 101,
          "a PlayerWorld on the real content.db holds the 101 mounts and the donors");
    std::string error;
    const bool speeds = world.load_mount_speeds(MOUNT_SPEEDS_PATH, error);
    CHECK(speeds, ("the Blueprints' speeds load (dist/content/mount_speeds.tsv): " + error).c_str());
    const spawns::MountBook::Speeds* terror = world.mounts() ? world.mounts()->speeds_of("Mount_Grounded_TerrorBird_0") : nullptr;
    const spawns::MountBook::Speeds* shoal = world.mounts() ? world.mounts()->speeds_of("Mount_Aquatic_Shoalskipper_1_ALT") : nullptr;
    const spawns::MountBook::Speeds* dawn = world.mounts() ? world.mounts()->speeds_of("Mount_Flying_Dawnbreaker_1") : nullptr;
    CHECK(terror && terror->run == 990 && shoal && shoal->swim == 920 && dawn && dawn->fly == 1290,
          "the terror bird runs 990, the shoalskipper swims 920, the Dawnbreaker flies 1290");
    CHECK(world.mounts() && world.mounts()->speeds_known() >= 140, "140 Blueprints' speeds");
    std::string bad;
    CHECK(!world.load_mount_speeds("no_such_file.tsv", bad) && !bad.empty(),
          "a missing file is an error, not a crash (the mounts then run at 600 cm/s times the item's ground percentage)");
    const std::string header = "blueprint\tkind\tparents\tmax_walk_speed\tmax_run_speed\tmax_fly_speed\tmax_swim_speed\tdefaults";
    std::string why;
    CHECK(try_mount_speeds_file(db, header + "\nMount_X\t\tMount_Base_C\t220\t600\t600\t400\tfly\n", why),
          "(control: a one-row file with the right header loads)");
    CHECK(!try_mount_speeds_file(db, "blueprint\tkind\tmax_run_speed\tmax_walk_speed\tparents\tmax_fly_speed\tmax_swim_speed\tdefaults\nMount_X\t\t600\t220\tMount_Base_C\t600\t400\tfly\n", why) &&
              why.find("columns") != std::string::npos,
          "a file with its columns reordered is refused (the speeds would be read as other numbers)");
    CHECK(!try_mount_speeds_file(db, header + "\nMount_X\t\tMount_Base_C\tfast\t600\t600\t400\tfly\n", why) && why.find("not a number") != std::string::npos,
          "a row whose speed is not a number is an error naming its line");
    CHECK(!try_mount_speeds_file(db, header + "\nMount_X\t\tMount_Base_C\n", why) && why.find("line 2") != std::string::npos,
          "and so is a row with too few columns");
}

int main() {
    std::string error;
    auto db = content::ContentDb::open(db_path(), error);
    if (!db) {
        std::printf("  [FAIL] the content.db does not open (%s): %s\n", db_path().c_str(), error.c_str());
        return 2;
    }
    auto book = spawns::MountBook::load(*db, error);
    if (!book) {
        std::printf("  [FAIL] the mount tables do not load: %s\n", error.c_str());
        return 2;
    }
    g_book = &*book;
    test_world_loads(*db);
    test_donors(*book);
    test_open_golden(*book);
    test_guids(*book);
    test_archetypes(*book);
    run_mounts_part2();
    run_mounts_part3();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
