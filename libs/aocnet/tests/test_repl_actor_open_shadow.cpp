// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_repl_actor_open_common.h"

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using capture_blocks::Bits;
using capture_blocks::Block;

static void test_shadow_single_and_failed_write(ExportAcks& acks) {
    {
        Actor pc = make_pc();
        ActorShadow shadow;
        open_actor(pc, shadow, acks);
        set(pc.obj, "Role", Value::u64(3));
        bool failed = true;
        const auto e = update_actor(pc, shadow, acks, &failed);
        CHECK(e && !failed && e->exports.empty(), "one changed value: an update without exports");
        if (e) {
            const auto views = read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0);
            size_t pos = views ? (*views)[0].payload_pos : 0;
            const auto st = views ? read_rep_block(*pc.obj.schema, e->payload.data(), e->payload.bit_pos(), pos) : std::nullopt;
            CHECK(views && views->size() == 1 && (*views)[0].header.is_actor && (*views)[0].header.has_rep_layout && st &&
                      st->values().size() == 1 && st->get(16) && *st->get(16) == Value::u64(3),
                  "the block is the actor's, RepLayout, handle 16 (Role) alone");
        }
        CHECK(!update_actor(pc, shadow, acks, &failed) && !failed, "and the next update has nothing");
    }
    {
        Actor pc = make_pc();
        ActorShadow shadow;
        open_actor(pc, shadow, acks);
        set(pc.obj, "bHidden", Value::boolean(true));
        RepMovement wide;
        wide.rotation = Rot{0, 300, 0};
        set(pc.obj, "ReplicatedMovement", Value(wide));
        const ActorShadow before = shadow;
        bool failed = false;
        CHECK(!update_actor(pc, shadow, acks, &failed) && failed, "a value that cannot be written fails the update");
        CHECK(shadow.actor.rep.sent == before.actor.rep.sent && shadow.subobjects.size() == before.subobjects.size(),
              "and the shadow is as it was");
        RepMovement fine;
        fine.rotation = Rot{0, 30, 0};
        set(pc.obj, "ReplicatedMovement", Value(fine));
        const auto e = update_actor(pc, shadow, acks, &failed);
        size_t pos = 0;
        const auto views = e ? read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0) : std::nullopt;
        pos = views ? (*views)[0].payload_pos : 0;
        const auto st = views ? read_rep_block(*pc.obj.schema, e->payload.data(), e->payload.bit_pos(), pos) : std::nullopt;
        CHECK(e && !failed && st && st->get(3) && *st->get(3) == Value::boolean(true) && st->get(7),
              "the corrected update still carries the handle that came before the bad one");
    }
}

static void test_shadow_open_failures_and_subobjects(ExportAcks& acks) {
    {
        Actor pc = make_pc();
        pc.open.archetype[0].name = std::string(1200, 'x');
        ActorShadow shadow;
        bool failed = false;
        CHECK(!open_actor(pc, shadow, acks, &failed) && failed && shadow.actor.rep.sent.empty() && shadow.subobjects.empty(),
              "an export too big for a bunch fails the open, the shadow stays empty");
        Actor odd = make_pc();
        odd.guid = guid(10341531, 60, 1);
        failed = false;
        CHECK(!open_actor(odd, shadow, acks, &failed) && failed, "an odd (static) actor GUID is refused");
    }
    {
        Actor pawn = make_pawn();
        ActorShadow shadow;
        open_actor(pawn, shadow, acks);
        Subobject stats = component(10341566, "StatsComponent", 0xf04938d2);
        pawn.subobjects.push_back(std::move(stats));
        ExportAcks acked;
        acked.mark(session_guid(10341566));
        bool failed = false;
        const auto quiet = update_actor(pawn, shadow, acked, &failed);
        CHECK(quiet && quiet->exports.empty() && quiet->payload.bit_pos() == 139,
              "a new empty subobject: its 139-bit block, no export once acknowledged");
        pawn.subobjects.push_back(component(10341564, "AbilityComponent", 0x9e63ae5c));
        const auto loud = update_actor(pawn, shadow, acks, &failed);
        CHECK(loud && loud->exports.size() == 1 && export_count(loud->exports[0]) == 1, "an unacknowledged new subobject brings its export");
    }
}

static void test_shadow() {
    std::printf("the shadow\n");
    ExportAcks acks;
    test_shadow_single_and_failed_write(acks);
    test_shadow_open_failures_and_subobjects(acks);
}

static void t1_rollback_across_objects() {
    std::printf("a failure in a subobject rolls back the whole update\n");
    Actor pawn = make_pawn();
    ActorShadow shadow;
    ExportAcks acks;
    open_actor(pawn, shadow, acks);
    set(pawn.obj, "bHidden", Value::boolean(true));
    Subobject* base = pawn.find(session_guid(10341540));
    FastArrayState* held = base->obj.arrays.get(*base->obj.schema, "HeldItems");
    bool built = held != nullptr;
    std::vector<int32_t> ids;
    for (int i = 0; built && i < 2049; ++i) {
        ElementState item;
        item.values[1] = Value::u64(i & 0x7F);
        item.values[2] = Value::i64(i);
        item.values[3] = Value::u64(0);
        const auto id = held->add(item);
        built = id.has_value();
        if (id) ids.push_back(*id);
    }
    CHECK(built, "a HeldItems array of 2049 items is built");
    const ActorShadow before = shadow;
    bool failed = false;
    CHECK(!update_actor(pawn, shadow, acks, &failed) && failed, "the update fails: the writer refuses 2049 changes");
    CHECK(shadow.actor.rep.sent == before.actor.rep.sent && shadow.subobjects.size() == before.subobjects.size(),
          "and the actor's shadow is as it was (its bHidden write was rolled back)");
    CHECK(shadow.subobjects.at(10341540).rep.sent == before.subobjects.at(10341540).rep.sent &&
              shadow.subobjects.at(10341540).fast.by_field.size() == before.subobjects.at(10341540).fast.by_field.size(),
          "and so is the failing subobject's");
    for (size_t i = 0; i < 100; ++i) held->remove(ids[i]);
    const auto e = update_actor(pawn, shadow, acks, &failed);
    size_t pos = 0;
    const auto views = e ? read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0) : std::nullopt;
    pos = views ? (*views)[0].payload_pos : 0;
    const auto st = views ? read_rep_block(*pawn.obj.schema, e->payload.data(), e->payload.bit_pos(), pos) : std::nullopt;
    CHECK(e && !failed && st && st->get(3) && *st->get(3) == Value::boolean(true),
          "once fixed, the update still carries the actor's bHidden (it was not lost with the failed attempt)");
}

static void t2_assembly_failure_rolls_back() {
    std::printf("an update whose assembly fails leaves the shadow alone\n");
    Actor pawn = make_pawn();
    ActorShadow shadow;
    ExportAcks acks;
    open_actor(pawn, shadow, acks);
    set(pawn.obj, "bHidden", Value::boolean(true));
    Subobject nameless = component(10341560, "AlignmentComponent", 0x3fbd829c);
    nameless.name.clear();
    pawn.subobjects.push_back(std::move(nameless));
    const ActorShadow before = shadow;
    bool failed = false;
    CHECK(!update_actor(pawn, shadow, acks, &failed) && failed, "a subobject that cannot be exported fails the update");
    CHECK(shadow.actor.rep.sent == before.actor.rep.sent && shadow.subobjects.size() == before.subobjects.size(),
          "and the shadow is as it was: no bHidden committed, the new subobject not recorded");
    pawn.subobjects.back().name = "AlignmentComponent";
    const auto e = update_actor(pawn, shadow, acks, &failed);
    const auto views = e ? read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0) : std::nullopt;
    CHECK(e && !failed && views && views->size() == 2 && (*views)[0].header.is_actor && !(*views)[1].header.is_actor,
          "named, the retry carries the actor's change and the new subobject's block");

    Actor again = make_pawn();
    ActorShadow s2;
    open_actor(again, s2, acks);
    set(again.obj, "bHidden", Value::boolean(true));
    again.subobjects.push_back(component(10341560, "AlignmentComponent", 0x3fbd829c));
    const ActorShadow b2 = s2;
    std::vector<ContentBlock> blocks;
    ShadowUndo undo;
    CHECK(collect_update_blocks(again, s2, blocks, &undo) && blocks.size() == 2 && s2.actor.rep.sent.count(3) == 1 &&
              s2.subobjects.count(10341560) == 1 && s2.subobjects.at(10341560).opened,
          "collect_update_blocks commits the actor's bHidden and records the new subobject as opened");
    undo.rollback(s2);
    CHECK(s2.actor.rep.sent == b2.actor.rep.sent && s2.subobjects.size() == b2.subobjects.size(),
          "ShadowUndo::rollback puts both back");
    ActorShadow s3 = b2;
    ShadowUndo open_undo;
    CHECK(collect_open_blocks(again, s3, blocks, &open_undo) && open_undo.whole.has_value(), "an open keeps the shadow it replaced");
    open_undo.rollback(s3);
    CHECK(s3.actor.rep.sent == b2.actor.rep.sent && s3.subobjects.size() == b2.subobjects.size(), "and the rollback restores it");
}

static void t3_block_past_the_bunch() {
    std::printf("a content block whose payload runs past the bunch\n");
    BunchWriter w;
    w.write_bit(0);
    w.write_bit(1);
    w.write_sip(1000);
    size_t pos = 0;
    CHECK(!read_content_block(w.data(), w.bit_pos(), pos, false).has_value() && pos == 0,
          "read_content_block (client form) refuses it and leaves pos");
    CHECK(!read_content_blocks(w.data(), w.bit_pos(), 0, true).has_value(), "read_content_blocks (server form) refuses it");
    const schema::ClassSchema& pawn = schema_named("PlayerPawn_C");
    CHECK(!read_client_bunch(pawn, nullptr, w.data(), w.bit_pos()).has_value(), "and so does read_client_bunch");
    BunchWriter v;
    v.write_bit(0);
    v.write_bit(1);
    v.write_sip(9);
    for (int i = 0; i < 8; ++i) v.write_bit(0);
    size_t p2 = 0;
    CHECK(!read_content_block(v.data(), v.bit_pos(), p2, false).has_value(), "a payload one bit past the end is refused");
}

static void t9_rpc_on_schemaless_target() {
    std::printf("an RPC on a subobject that has no schema\n");
    Actor pc = make_pc();
    ExportAcks acks;
    CHECK(!call_rpc(pc, &pc.subobjects[0], "ClientSetHUD", RpcParams{}, {}, acks).has_value(), "refused, no crash");
}

static void test_rpc_calls_checked() {
    std::printf("call_rpcs: reliability, targets\n");
    Actor pc = make_pc();
    ExportAcks acks;
    RpcRequest reliable;
    reliable.rpc = "ClientFlushLevelStreaming";
    RpcRequest unreliable;
    unreliable.rpc = "ClientHitConfirmations";
    const auto r = call_rpcs(pc, std::span<const RpcRequest>(&reliable, 1), acks);
    const auto u = call_rpcs(pc, std::span<const RpcRequest>(&unreliable, 1), acks);
    CHECK(r && r->reliable && u && !u->reliable, "a reliable call gives a reliable emission, an unreliable one an unreliable emission");
    const RpcRequest both[2] = {reliable, unreliable};
    CHECK(!call_rpcs(pc, both, acks), "a bunch that mixes the two is refused (one bunch has one reliability)");
    const RpcRequest two[2] = {reliable, reliable};
    const auto rr = call_rpcs(pc, two, acks);
    CHECK(rr && rr->reliable, "two reliable calls batch");
    ActorShadow pc_shadow;
    const auto pc_open = open_actor(pc, pc_shadow, acks);
    CHECK(pc_open && pc_open->reliable, "an open is reliable");

    Actor other = make_pc();
    RpcRequest foreign;
    foreign.rpc = "ClientFlushLevelStreaming";
    foreign.target = &other.subobjects[0];
    CHECK(!call_rpcs(pc, std::span<const RpcRequest>(&foreign, 1), acks), "a target that is not one of the actor's subobjects is refused");
    Subobject stray = component(10341564, "AbilityComponent", 0x9e63ae5c);
    foreign.target = &stray;
    CHECK(!call_rpcs(pc, std::span<const RpcRequest>(&foreign, 1), acks), "nor one that belongs to no actor");
    Actor pawn = make_pawn();
    pawn.subobjects.push_back(component(10341564, "AbilityComponent", 0x9e63ae5c));
    RpcRequest own;
    own.target = &pawn.subobjects.back();
    own.rpc = "ClientEndAbility";
    const schema::RpcSchema* end = own.target->obj.schema->rpc("ClientEndAbility");
    set_rpc_param(*end, own.params, end->params[0].name, Value::i64(77));
    CHECK(call_rpcs(pawn, std::span<const RpcRequest>(&own, 1), acks).has_value(), "the actor's own subobject is fine");
}

static void test_export_packing_first_guid(const FIntrepidNetworkGUID& npc, const ExportChain& level) {
    ExportAcks none;
    std::vector<ExportChain> chains = {level, level};
    const PackedExports twice = pack_exports(chains, none);
    const PackedExports once = pack_exports(std::span<const ExportChain>(&chains[0], 1), none);
    CHECK(twice.ok && once.ok && twice.bunches.size() == 1 && export_count(twice.bunches[0]) == 1 &&
              twice.bunches[0].bit_pos() == once.bunches[0].bit_pos() &&
              capture_blocks::same_bit_range(twice.bunches[0].data(), once.bunches[0].data(), once.bunches[0].bit_pos(), nullptr) &&
              twice.exported == once.exported && twice.exported.size() == 3,
          "two chains with the same first GUID give one entry, count 1, exactly the bits of the single chain");
    const ExportChain alignment = component_export(npc, session_guid(2932), "AlignmentComponent", 0x3fbd829c);
    const ExportChain actor_alone = {ExportLink{npc, "", true, true, 0}};
    std::vector<ExportChain> with_actor = {alignment, actor_alone};
    const PackedExports q = pack_exports(with_actor, none);
    CHECK(q.ok && q.bunches.size() == 1 && export_count(q.bunches[0]) == 1,
          "a chain whose first GUID the bunch holds as an outer (the actor of a component) is not exported again");
    const PackedExports both = pack_exports(std::vector<ExportChain>{level, alignment}, none);
    const size_t just_under = both.bunches[0].bit_pos() - 1;
    const PackedExports split = pack_exports(std::vector<ExportChain>{level, alignment, level}, none, just_under);
    CHECK(split.ok && split.bunches.size() == 3 && export_count(split.bunches[0]) == 1 && export_count(split.bunches[1]) == 1 &&
              export_count(split.bunches[2]) == 1,
          "after a flush the first GUID is exported again: the record is per bunch");
}

static void test_export_packing_bunch_acks(const FIntrepidNetworkGUID& npc, const ExportChain& level) {
    ExportAcks none;
    {
        std::vector<ExportChain> chains;
        chains.push_back({link(15775025627955649865ull, "Default__CSQ0006_EmissaryVaeluneEmpire_C", false, 0x2cdc5b6),
                          link(17259758083969458627ull,
                               "/Game/DesignAssets/Quests/StoryArcAssets/Side_Quests_Alpha01/CSQ_FTU/CSQ0006_EmissaryVaeluneEmpire", false, 0)});
        chains.push_back(level);
        chains.push_back(component_export(npc, session_guid(2932), "AlignmentComponent", 0x3fbd829c));
        chains.push_back(gameplay_class(4937921067877173103ull, "NPCAppearanceComponent", 0x8523f385));
        const PackedExports one = pack_exports(chains, none);
        CHECK(one.ok && one.bunches.size() == 1 && one.exported_by_bunch.size() == 1 && one.exported_by_bunch[0] == one.exported,
              "one bunch: its GUIDs are all the exported ones");
        size_t fits = 0;
        for (size_t bits = 200; bits < 7621; bits += 8) {
            const PackedExports t = pack_exports(chains, none, bits);
            if (t.ok && t.bunches.size() == 2) { fits = bits; break; }
        }
        const PackedExports two = pack_exports(chains, none, fits);
        std::vector<FIntrepidNetworkGUID> all;
        for (const auto& b : two.exported_by_bunch) all.insert(all.end(), b.begin(), b.end());
        CHECK(fits != 0 && two.bunches.size() == 2 && two.exported_by_bunch.size() == 2 && all == two.exported &&
                  !two.exported_by_bunch[0].empty() && !two.exported_by_bunch[1].empty(),
              "two bunches: the GUIDs per bunch, parallel to the bunches, in the order they were written");
    }
    {
        Actor pawn = make_pawn();
        ActorShadow shadow;
        ExportAcks acks;
        const auto open = open_actor(pawn, shadow, acks);
        CHECK(open && open->exported_by_bunch.size() == open->exports.size() && open->exports.size() == 1 &&
                  open->exported_by_bunch[0] == open->exported,
              "an open's Emission has the GUIDs per export bunch");
        ActorShadow other;
        const auto again = open_actor(pawn, other, acks);
        CHECK(again && again->exports.size() == 1 && again->exported == open->exported, "unacknowledged: the same exports again");
        acks.mark(open->exported_by_bunch[0]);
        ActorShadow third;
        const auto quiet = open_actor(pawn, third, acks);
        CHECK(quiet && quiet->exports.empty() && quiet->exported.empty(), "acknowledged: no export bunch");
    }
}

static void test_export_packing_rules() {
    std::printf("exports: a first GUID the bunch already holds, per-bunch GUIDs, marking on acknowledgement\n");
    const FIntrepidNetworkGUID npc = session_guid(2926);
    const ExportChain level = level_chain();
    test_export_packing_first_guid(npc, level);
    test_export_packing_bunch_acks(npc, level);
}

static RepState actor_block_of(const Actor& a, const Emission& e, size_t from) {
    const auto views = read_content_blocks(e.payload.data(), e.payload.bit_pos(), from);
    size_t pos = views && !views->empty() ? (*views)[0].payload_pos : 0;
    const auto st = views && !views->empty() && (*views)[0].header.is_actor && (*views)[0].header.has_rep_layout
                        ? read_rep_block(*a.obj.schema, e.payload.data(), e.payload.bit_pos(), pos)
                        : std::nullopt;
    return st ? *st : RepState{};
}

static void test_active_parents_subobject(Actor& pawn, ExportAcks& acks) {
    ActorShadow s2;
    Subobject* combat = pawn.find(session_guid(10341542));
    s2.subobjects[10341542].active.set_active(*combat->obj.schema, "bIsDead", false);
    const auto open2 = open_actor(pawn, s2, acks);
    const auto views = open2 ? read_content_blocks(open2->payload.data(), open2->payload.bit_pos(), 487) : std::nullopt;
    bool without = false;
    bool found = false;
    if (views) {
        for (const BlockView& v : *views) {
            if (v.header.is_actor || !(v.header.object == session_guid(10341542))) continue;
            found = true;
            size_t pos = v.payload_pos;
            const auto cs = read_rep_block(*combat->obj.schema, open2->payload.data(), open2->payload.bit_pos(), pos);
            without = cs && !cs->get(*combat->obj.schema, "bIsDead") && cs->get(*combat->obj.schema, "PrimaryClassId.Guid");
        }
    }
    CHECK(found && without, "a subobject with a mask made before the open gets its block, with the inactive property left out");
    CHECK(s2.subobjects.at(10341542).opened && s2.subobjects.at(10341542).rep.sent.count(combat->obj.schema->command("bIsDead")->handle) == 0,
          "and is opened in the shadow");
    s2.forget_sent();
    CHECK(s2.subobjects.size() == 1 && !s2.subobjects.at(10341542).opened && !s2.subobjects.at(10341542).active.all_active() &&
              s2.subobjects.at(10341542).rep.sent.empty() && s2.actor.rep.sent.empty(),
          "forget_sent keeps a subobject that has a mask (forgotten) and drops the rest");
}

static void test_active_parents() {
    std::printf("per-connection conditions: a parent the connection does not have is left out, sent when it turns active\n");
    Actor pawn = make_pawn();
    const schema::ClassSchema& cls = *pawn.obj.schema;
    ExportAcks acks;
    ActorShadow shadow;
    CHECK(shadow.actor.active.set_active(cls, "ReplicatedMovement", false) && !shadow.actor.active.set_active(cls, "NoSuchProperty", false),
          "a mask is set by property path (an unknown path is refused)");
    const auto open = open_actor(pawn, shadow, acks);
    const RepState opened = open ? actor_block_of(pawn, *open, 487) : RepState{};
    CHECK(open && opened.values().size() == 4 && opened.get(1) && !opened.get(7) && shadow.actor.rep.sent.count(7) == 0,
          "an open leaves the inactive parent's handle out (4 of the actor's 5 values), and out of the shadow");
    CHECK(!shadow.actor.active.active(cls.command("ReplicatedMovement")->parent) && shadow.actor.active.active(0),
          "the mask survives the open (it is the connection's, not something that was sent)");

    RepMovement moved;
    moved.location = QuantVec{QuantVec::Mode::Scaled, -77766000, 61657869, 1595205};
    moved.rotation = Rot{0, 70, 0};
    moved.linear_velocity = QuantVec{QuantVec::Mode::Scaled, 120, 0, 0};
    set(pawn.obj, "ReplicatedMovement", Value(moved));
    bool failed = true;
    CHECK(!update_actor(pawn, shadow, acks, &failed) && !failed, "a change to an inactive parent sends nothing");

    shadow.actor.active.set_active(cls, "ReplicatedMovement", true);
    const auto e = update_actor(pawn, shadow, acks, &failed);
    const RepState st = e ? actor_block_of(pawn, *e, 0) : RepState{};
    CHECK(e && !failed && st.values().size() == 1 && st.get(7) && *st.get(7) == Value(moved),
          "once active, the update carries it: what differs from the shadow (the whole value: it was never sent)");
    CHECK(!update_actor(pawn, shadow, acks, &failed) && !failed, "and then nothing");

    shadow.actor.active.set_active(cls, "ReplicatedMovement", false);
    RepMovement moved2 = moved;
    moved2.location.x += 100;
    set(pawn.obj, "ReplicatedMovement", Value(moved2));
    CHECK(!update_actor(pawn, shadow, acks, &failed), "inactive again: nothing for the new value");
    shadow.actor.active.set_active(cls, "ReplicatedMovement", true);
    const auto e2 = update_actor(pawn, shadow, acks, &failed);
    const RepState st2 = e2 ? actor_block_of(pawn, *e2, 0) : RepState{};
    CHECK(e2 && st2.get(7) && *st2.get(7) == Value(moved2), "reactivated: the changed value goes out");
    shadow.actor.active.set_active(cls, "ReplicatedMovement", false);
    set(pawn.obj, "ReplicatedMovement", Value(moved));
    set(pawn.obj, "ReplicatedMovement", Value(moved2));
    shadow.actor.active.set_active(cls, "ReplicatedMovement", true);
    CHECK(!update_actor(pawn, shadow, acks, &failed) && !failed, "a value that is back at what the connection has sends nothing on activation");

    test_active_parents_subobject(pawn, acks);
}

static size_t g_allocated = 0;

void* operator new(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    g_allocated += n;
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static void test_noop_update_copies_nothing() {
    std::printf("an update that sends nothing does not copy the shadow\n");
    Actor pawn = make_pawn();
    ActorShadow shadow;
    ExportAcks acks;
    open_actor(pawn, shadow, acks);
    g_allocated = 0;
    {
        const ActorShadow copy = shadow;
        (void)copy;
    }
    const size_t copy_cost = g_allocated;
    g_allocated = 0;
    bool failed = true;
    const auto e = update_actor(pawn, shadow, acks, &failed);
    const size_t noop_cost = g_allocated;
    std::printf("    copying the pawn's shadow allocates %zu bytes, an update that sends nothing %zu\n", copy_cost, noop_cost);
    CHECK(!e && !failed, "nothing changed: no emission");
    CHECK(copy_cost > 10000 && noop_cost * 8 < copy_cost,
          "and it allocates a fraction of what copying the shadow once would (the old code copied it twice)");
}

void run_repl_actor_open_shadow() {
    test_shadow();
    t1_rollback_across_objects();
    t2_assembly_failure_rolls_back();
    t3_block_past_the_bunch();
    t9_rpc_on_schemaless_target();
    test_rpc_calls_checked();
    test_export_packing_rules();
    test_active_parents();
    test_noop_update_copies_nothing();
}
