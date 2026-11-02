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

static bool alignment_holds_mount_and_owner(
    const repl::AlignmentContainer* c,
    const repl::GuidValue& mount_guid,
    const repl::GuidValue& owner_guid) {
    if (!c) return false;
    bool both = false;
    for (const auto& inst : c->instances) {
        bool m = false;
        bool o = false;
        for (const repl::GuidValue& g : inst.guids) {
            m = m || g == mount_guid;
            o = o || g == owner_guid;
        }
        both = both || (m && o);
    }
    return both;
}

static void test_owner(const spawns::MountBook& book) {
    std::printf("the owner is where the capture's was\n");
    const spawns::OpenTemplate* donor = book.donors().donor("ground");
    const content::MountRow* row = first_of_kind(book, "ground");
    if (!donor || !row) return;
    const auto v = values_of(*donor);
    spawns::MountParams p = plain_params();
    std::string error;
    auto made = book.make(*row, p, mint, error);
    if (!v || !made) {
        CHECK(false, "a mount is made");
        return;
    }
    const repl::GuidValue capture_owner = v->p.owner_guid;
    const repl::GuidValue capture_mount = *v->p.mount_guid;
    size_t left_owner = 0;
    size_t left_mount = 0;
    size_t ours = 0;
    for (const repl::Subobject& s : made->actor.actor.subobjects) {
        repl::ObjectState copy = s.obj;
        left_owner += spawns::replace_guid(copy, capture_owner, repl::GuidValue{9, 9, 9, 9});
        left_mount += spawns::replace_guid(copy, capture_mount, repl::GuidValue{9, 9, 9, 9});
        repl::ObjectState again = s.obj;
        ours += spawns::replace_guid(again, p.owner_guid, repl::GuidValue{9, 9, 9, 9});
    }
    CHECK(left_owner == 0 && left_mount == 0, "the capture's owner and mount guids stand nowhere (alignments, interaction states)");
    CHECK(ours >= 5, "the owner's guid stands in the alignments and the interaction state (two maps and the input state)");
    const repl::Subobject* al = sub_named(made->actor.actor, "UAlignmentComponent");
    const repl::Value* av = al ? al->obj.state.get(*al->obj.schema, "Alignments") : nullptr;
    const auto* c = av ? av->get_if<repl::AlignmentContainer>() : nullptr;
    CHECK(alignment_holds_mount_and_owner(c, made->mount_guid, p.owner_guid), "the Alignments hold the mount and its owner in one instance");
    repl::ObjectState o = made->actor.actor.subobjects[0].obj;
    CHECK(spawns::replace_guid(o, p.owner_guid, p.owner_guid) == 0 &&
              spawns::replace_guid(o, repl::GuidValue{5, 6, 7, 8}, repl::GuidValue{1, 2, 3, 4}) == 0,
          "replacing a guid by itself, or one that is not there, changes nothing");
}

static void test_book(const spawns::MountBook& book) {
    std::printf("every mount item makes an actor that opens\n");
    size_t made_n = 0;
    size_t opens = 0;
    size_t wrong_kind = 0;
    std::set<uint64_t> archetype_ids;
    std::map<std::string, size_t> by_kind;
    for (const content::MountRow& row : book.data().mounts) {
        spawns::MountParams p = plain_params();
        p.owner_level = 25;
        std::string error;
        auto made = book.make(row, p, mint, error);
        if (!made) {
            std::printf("    %s: %s\n", row.item_name.c_str(), error.c_str());
            continue;
        }
        ++made_n;
        ++by_kind[row.kind];
        if (writes_open(made->actor.actor)) ++opens;
        const repl::Subobject* mc = sub_named(made->actor.actor, "UMountComponent");
        const std::string want_name =
            row.kind == "ground" ? "GroundMountComponent" : row.kind == "flying" ? "FlyingMountComponent" : "AquaticMountComponent";
        if (!mc || mc->name != want_name) ++wrong_kind;
        archetype_ids.insert(made->actor.actor.open.archetype[0].guid.ObjectId);
        if (made->max_health != book.max_health(row, 25)) ++wrong_kind;
    }
    CHECK(made_n == book.data().mounts.size() && made_n == 101, "all 101 mount items make an actor");
    CHECK(opens == made_n, "every actor's open writes");
    CHECK(wrong_kind == 0, "each has the mount component of its kind (Ground, Flying, Aquatic) and its MaxHealth");
    CHECK(by_kind["ground"] == 51 && by_kind["flying"] == 46 && by_kind["aquatic"] == 4, "51 ground, 46 flying and 4 aquatic mounts");
    std::set<std::string> blueprints;
    for (const content::MountRow& row : book.data().mounts) blueprints.insert(row.blueprint);
    CHECK(archetype_ids.size() == blueprints.size(), "one archetype export per Blueprint (items that share a Blueprint share it)");
}

constexpr uint64_t kCombatant = 6064629443707731976ull;
constexpr uint64_t kNodeCitizen = 6064631051897143296ull;
constexpr uint64_t kGuildMember = 6064630560782155776ull;

static const repl::AlignmentContainer* donor_inherited_alignments(const spawns::MountBook& book, const char* kind) {
    const spawns::OpenTemplate* d = book.donors().donor(kind);
    const repl::ObjectState* al = nullptr;
    if (d) {
        for (const spawns::ParsedBlock& b : d->blocks) {
            if (b.cls == "UAlignmentComponent") al = &b.state;
        }
    }
    return al ? alignments_in(*al, "InheritedAlignments") : nullptr;
}

static bool has_flagged_current_effect(const repl::Subobject* stats) {
    if (!stats) return false;
    const schema::NetField* nf = stats->obj.schema->field("CurrentEffects");
    const repl::FastArrayState* arr = nf ? stats->obj.arrays.find(nf->index) : nullptr;
    if (!arr) return false;
    bool pvp_effect = false;
    for (const repl::FastArrayItem& it : arr->items()) {
        for (const auto& [h, v] : it.values.values) {
            if (const auto* b = v.get_if<bool>()) pvp_effect = pvp_effect || *b;
        }
    }
    return pvp_effect;
}

static void check_unflagged_owner_alignments(const spawns::MountBook& book, const char* name) {
    const content::MountRow* row = row_of_blueprint(book, name);
    if (!row) {
        CHECK(false, (std::string(name) + ": the item is in the data").c_str());
        return;
    }
    const std::string title = name;
    spawns::MountParams p = plain_params();
    std::string error;
    auto made = make_plain(book, *row, p, error);
    if (!made) {
        CHECK(false, (title + ": the mount is made: " + error).c_str());
        return;
    }
    const repl::Subobject* al = sub_named(made->actor.actor, "UAlignmentComponent");
    const repl::AlignmentContainer* inherited = al ? alignments_in(al->obj, "InheritedAlignments") : nullptr;
    const repl::AlignmentContainer* own = al ? alignments_in(al->obj, "Alignments") : nullptr;
    const std::vector<uint64_t> want = {
        bootstrap::kAlignmentKeyA, bootstrap::kAlignmentKeyB, bootstrap::kAlignmentKeyCharacter, bootstrap::kAlignmentKeyC
    };
    CHECK(inherited && keys_of(*inherited) == want,
          (title + ": it inherits Player, NonCombatant, the owner's Instigator and PVPSuppressed_Perceiver, and nothing else").c_str());
    CHECK(inherited && !instance_of_key(*inherited, kCombatant) &&
              !instance_of_key(*inherited, kNodeCitizen) && !instance_of_key(*inherited, kGuildMember),
          (title + ": no Combatant (the PvP flag), no node and no guild").c_str());
    const repl::AlignmentContainer::Instance* self = inherited ? instance_of_key(*inherited, bootstrap::kAlignmentKeyCharacter) : nullptr;
    CHECK(self && self->guids.size() == 1 && self->guids[0] == p.owner_guid, (title + ": its Instigator group is the owner").c_str());
    bool zeros = inherited != nullptr;
    for (const uint64_t k : {bootstrap::kAlignmentKeyA, bootstrap::kAlignmentKeyB, bootstrap::kAlignmentKeyC}) {
        const auto* i = inherited ? instance_of_key(*inherited, k) : nullptr;
        zeros = zeros && i && i->guids.size() == 1 && i->guids[0] == repl::GuidValue{};
    }
    CHECK(zeros, (title + ": the groups of Player, NonCombatant and PVPSuppressed_Perceiver are the zero group, as the owner's").c_str());
    CHECK(own && own->instances.size() == 1 && own->instances[0].key == bootstrap::kAlignmentKeyCharacter &&
              own->instances[0].guids.size() == 2 && own->instances[0].guids[0] == made->mount_guid &&
              own->instances[0].guids[1] == p.owner_guid && own->timers.empty() && inherited && inherited->timers.empty(),
          (title + ": its own Alignments are one Instigator group, the mount and its owner [R: 911, 898, 871, 872]").c_str());
    const repl::Subobject* ci = sub_named(made->actor.actor, "UNPCCombatInfo");
    CHECK(ci && flag_off(ci->obj, "bPvPForceFlag") && flag_off(ci->obj, "bInPVPCombat") &&
              flag_off(ci->obj, "InPvpSuppressionArea") && flag_off(ci->obj, "bInCombat"),
          (title + ": its combat info holds no PvP flag, no PvP combat and no suppression area").c_str());
    const repl::Subobject* stats = sub_named(made->actor.actor, "UAoCStatsComponent");
    CHECK(stats && !has_flagged_current_effect(stats), (title + ": it holds no effect of a flagged source (a new mount has no effect)").c_str());
}

static void test_alignments_are_the_owners(const spawns::MountBook& book) {
    std::printf("a mount's alignments are its owner's, never a donor's (stage 3 fix 2: the PvP flag)\n");
    const repl::AlignmentContainer* aq = donor_inherited_alignments(book, "aquatic");
    const repl::AlignmentContainer* gr = donor_inherited_alignments(book, "ground");
    const repl::AlignmentContainer* fl = donor_inherited_alignments(book, "flying");
    CHECK(aq && instance_of_key(*aq, kCombatant), "(the aquatic donor's owner, the retail capture, was a Combatant: flagged for PvP)");
    CHECK(gr && fl && instance_of_key(*gr, kNodeCitizen) && instance_of_key(*gr, kGuildMember) &&
              instance_of_key(*fl, kNodeCitizen) && instance_of_key(*fl, kGuildMember) &&
              !instance_of_key(*gr, kCombatant) && !instance_of_key(*fl, kCombatant),
          "(the ground and flying donors' owners, 911 and 871, had a node and a guild)");
    const char* blueprints[] = {
        "Mount_Grounded_TerrorBird_0", "Mount_Aquatic_Shoalskipper_1_ALT", "Mount_Flying_Dawnbreaker_1",
        "Mount_Grounded_Scorpix_1", "Mount_Grounded_VerdantVoyager_0", "Mount_Flying_Gryphon_5_Armored"
    };
    for (const char* name : blueprints) check_unflagged_owner_alignments(book, name);
    const content::MountRow* row = row_of_blueprint(book, "Mount_Grounded_TerrorBird_0");
    spawns::MountParams p = plain_params();
    repl::AlignmentContainer flagged;
    const repl::GuidValue none;
    flagged.instances = {
        {bootstrap::kAlignmentKeyA, {none}},
        {kCombatant, {repl::GuidValue{9, 8, 7, 6}}},
        {bootstrap::kAlignmentKeyCharacter, {p.owner_guid}}
    };
    p.owner_alignments = flagged;
    std::string error;
    auto made = row ? make_plain(book, *row, p, error) : std::nullopt;
    const repl::Subobject* al = made ? sub_named(made->actor.actor, "UAlignmentComponent") : nullptr;
    const repl::AlignmentContainer* inherited = al ? alignments_in(al->obj, "InheritedAlignments") : nullptr;
    CHECK(inherited && *inherited == flagged,
          "a flagged owner's alignments (a Combatant) are inherited exactly: the owner's state, not the donor's");
}

struct RiderAnimCase {
    const char* blueprint;
    const char* klass;
    const char* package;
};

static void count_interaction_refs(
    const repl::Subobject* inter,
    const repl::ExportChain& chain,
    size_t& named,
    size_t& ours) {
    const repl::Value* data = inter ? inter->obj.state.get(*inter->obj.schema, "InteractionInputData") : nullptr;
    const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
    if (!arr) return;
    for (const repl::ElementState& e : arr->elements) {
        for (const auto& [h, v] : e.values) {
            if (const auto* g = v.get_if<FIntrepidNetworkGUID>()) {
                ++named;
                ours += !chain.empty() && g->ObjectId == chain[0].guid.ObjectId;
            }
        }
    }
}

static void check_rider_anim_case(const spawns::MountBook& book, const RiderAnimCase& c) {
    const std::string title = c.blueprint;
    const content::MountRow* row = row_of_blueprint(book, c.blueprint);
    if (!row) {
        CHECK(false, (title + ": the item is in the data").c_str());
        return;
    }
    CHECK(book.rider_anim_of(*row) == std::string(c.package) + "." + c.klass, (title + ": its rider AnimBP is " + c.klass).c_str());
    std::string merror;
    auto made = make_plain(book, *row, plain_params(), merror);
    if (!made) {
        CHECK(false, (title + ": the mount is made: " + merror).c_str());
        return;
    }
    const world::WorldActor& w = made->actor;
    CHECK(w.open_exports.size() == 1 && w.open_exports[0].size() == 2,
          (title + ": the open carries one export: the AnimBP class and its package").c_str());
    const repl::ExportChain chain = w.open_exports.empty() ? repl::ExportChain{} : w.open_exports[0];
    CHECK(chain.size() == 2 && chain[0].name == c.klass && chain[1].name == c.package &&
              chain[0].has_path() && chain[1].has_path() && chain[0].has_checksum && chain[0].checksum == 0 &&
              chain[1].has_checksum && chain[1].checksum == 0 && chain[0].guid.is_valid() && chain[1].guid.is_valid() &&
              chain[0].guid.ObjectId != chain[1].guid.ObjectId,
          (title + ": the class is exported by its name in its package, flags 5, checksum 0 [R: the retail capture's export of ABP_PC_MNT_Swiftclaw_C]").c_str());
    size_t named = 0;
    size_t ours = 0;
    count_interaction_refs(sub_named(w.actor, "UInteractableComponent"), chain, named, ours);
    CHECK(named == 1 && ours == 1, (title + ": the interaction data's one object reference, AttachmentAnimation, is the exported class").c_str());
    auto second = make_plain(book, *row, plain_params(), merror);
    CHECK(second && second->actor.open_exports.size() == 1 && same_chain(second->actor.open_exports[0], chain),
          (title + ": a second mount exports the same objects (stable between calls)").c_str());
    CHECK(writes_open(const_cast<repl::Actor&>(w.actor)), (title + ": the open writes").c_str());
}

static void check_rider_anim_fallbacks() {
    struct K {
        const char* blueprint;
        const char* want;
    } fallbacks[] = {
        {"Mount_Grounded_Scorpix_1", "/Game/Characters/Animations/Mounts_Rider/Swiftclaw/ABP_PC_MNT_Swiftclaw.ABP_PC_MNT_Swiftclaw_C"},
        {"Mount_Grounded_VerdantVoyager_0", "/Game/Characters/Animations/Mounts_Rider/Tidesnapper/ABP_PC_MNT_Tidesnapper.ABP_PC_MNT_Tidesnapper_C"},
        {"Mount_Flying_Gryphon_5_Armored", "/Game/Characters/Animations/Mounts_Rider/Dawnbreaker/ABP_PC_MNT_Dawnbreaker.ABP_PC_MNT_Dawnbreaker_C"}
    };
    std::string error;
    auto opened = content::ContentDb::open(db_path(), error);
    auto fresh = opened ? spawns::MountBook::load(*opened, error) : std::nullopt;
    bool ok = fresh.has_value() && fresh->riders_known() == 0;
    for (const K& k : fallbacks) {
        const content::MountRow* row = fresh ? row_of_blueprint(*fresh, k.blueprint) : nullptr;
        ok = ok && row && fresh->rider_anim_of(*row) == k.want;
    }
    CHECK(ok, "with no riders file a mount names its kind's donor AnimBP (ground Swiftclaw, aquatic Tidesnapper, flying Dawnbreaker)");
    std::string bad;
    CHECK(fresh && !fresh->load_riders("no_such_file.tsv", bad) && !bad.empty(), "a missing riders file is an error, not a crash");
}

static void test_rider_anim(spawns::MountBook& book) {
    std::printf("the rider's AnimBP: named in the mount's interaction data and exported with its open (stage 3 fix 2: the rider sat nowhere)\n");
    check_rider_anim_fallbacks();
    std::string error;
    CHECK(book.load_riders(MOUNT_RIDERS_PATH, error), ("dist/content/mount_riders.tsv loads (tools/mapc mount-speeds): " + error).c_str());
    CHECK(book.riders_known() > 100, "it names the AnimBP of over a hundred Blueprints");
    const RiderAnimCase cases[] = {
        {"Mount_Grounded_TerrorBird_0", "ABP_PC_MNT_Swiftclaw_C", "/Game/Characters/Animations/Mounts_Rider/Swiftclaw/ABP_PC_MNT_Swiftclaw"},
        {"Mount_Aquatic_Shoalskipper_1_ALT", "ABP_PC_MNT_Tidesnapper_C", "/Game/Characters/Animations/Mounts_Rider/Tidesnapper/ABP_PC_MNT_Tidesnapper"},
        {"Mount_Flying_Dawnbreaker_1", "ABP_PC_MNT_Dawnbreaker_C", "/Game/Characters/Animations/Mounts_Rider/Dawnbreaker/ABP_PC_MNT_Dawnbreaker"},
        {"Mount_Grounded_Scorpix_1", "ABP_PC_MNT_VerdantVoyager_C", "/Game/Characters/Animations/Mounts_Rider/VerdantVoyager/ABP_PC_MNT_VerdantVoyager"},
        {"Mount_Grounded_Horse_0", "ABP_PC_MNT_Horse_C", "/Game/Characters/Animations/Mounts_Rider/Horse/ABP_PC_MNT_Horse"},
        {"Mount_Flying_Gryphon_5_Armored", "ABP_PC_MNT_Gryphon_C", "/Game/Characters/Animations/Mounts_Rider/Gryphon/ABP_PC_MNT_Gryphon"},
    };
    for (const RiderAnimCase& c : cases) check_rider_anim_case(book, c);
    size_t named = 0;
    size_t exported = 0;
    std::set<std::string> anims;
    for (const content::MountRow& row : book.data().mounts) {
        const std::string anim = book.rider_anim_of(row);
        named += anim.rfind("/Game/", 0) == 0 && anim.size() > 2 && anim.compare(anim.size() - 2, 2, "_C") == 0 && anim.find('.') != std::string::npos;
        anims.insert(anim);
        std::string e;
        auto made = make_plain(book, row, plain_params(), e);
        exported += made && made->actor.open_exports.size() == 1;
    }
    CHECK(named == book.data().mounts.size() && exported == book.data().mounts.size(), "all 101 mount items name a rider AnimBP and export it");
    CHECK(anims.size() > 20, "(and they are not all one AnimBP: the Blueprints name their own)");
}

void run_mounts_part2() {
    test_owner(*g_book);
    test_book(*g_book);
    test_alignments_are_the_owners(*g_book);
    test_rider_anim(*g_book);
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
