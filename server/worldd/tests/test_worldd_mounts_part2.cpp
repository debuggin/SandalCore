// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using aoc::protocol::emit::FIntrepidNetworkGUID;

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
