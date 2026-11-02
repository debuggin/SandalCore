// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_repl_rep_block_common.h"

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using capture_blocks::Block;

static void test_captured_blocks() {
    std::printf("captured blocks: read to the last bit, written back bit for bit\n");
    round_trip_block(capture_blocks::kPcOpenProps);
    round_trip_block(capture_blocks::kPcLoadTracker46);
    round_trip_block(capture_blocks::kPcLoadTracker72);
    round_trip_block(capture_blocks::kPcSettings620);
    round_trip_block(capture_blocks::kPcAttachRotation7706);
    round_trip_block(capture_blocks::kPawnOpenActor);
    round_trip_block(capture_blocks::kPawnPossessed107);
    round_trip_block(capture_blocks::kPawnUnhide2523);
    round_trip_block(capture_blocks::kPawnViewPitch7706);
    round_trip_block(capture_blocks::kPlayerStateOpen434);
    round_trip_block(capture_blocks::kPlayerStateAttach491);
    round_trip_block(capture_blocks::kPlayerInfoOpen);
    round_trip_block(capture_blocks::kPlayerCombatInfoOpen);
    round_trip_block(capture_blocks::kPlayerInfoPossessed);
    round_trip_block(capture_blocks::kOwnerInfo107);
    round_trip_block(capture_blocks::kAbilityPawn107);
    round_trip_block(capture_blocks::kAbilityNpc);
}

static void test_decoded_values_actors() {
    {
        const Block& b = capture_blocks::kPcOpenProps;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const RepState& st = *d.state;
        CHECK(st.values().size() == 7, "PC open: 7 handles (1, 8, 16, 19, 22, 24, 52)");
        CHECK(st.get(s, "RemoteRole") && *st.get(s, "RemoteRole") == Value::u64(2) && st.get(s, "Role") && *st.get(s, "Role") == Value::u64(4),
              "RemoteRole 2 (AutonomousProxy on the wire), Role 4");
        const Value* ps = st.get(s, "PlayerState");
        const auto* g = ps ? ps->get_if<emit::FIntrepidNetworkGUID>() : nullptr;
        CHECK(g && g->ObjectId == 10341528 && g->ServerId == 60 && g->Randomizer == 0x6ee87ee4, "PlayerState is object 10341528 (server 60)");
        const Value* loc = st.get(s, "SpawnLocation");
        CHECK(loc && loc->get_if<Vec3d>() && loc->get_if<Vec3d>()->x == -777664.5, "SpawnLocation is an FVector");
        CHECK(st.get(s, "SummonCooldownTimer") && st.get(s, "SummonCooldownTimer")->get_if<float>(), "SummonCooldownTimer is a float");
    }
    {
        const Block& b = capture_blocks::kPawnOpenActor;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const Value* m = d.state->get(s, "ReplicatedMovement");
        const RepMovement* rm = m ? m->get_if<RepMovement>() : nullptr;
        CHECK(rm && rm->location.x == -77766450 && rm->rotation.yaw == 65, "pawn open: ReplicatedMovement decoded (byte rotation)");
        const Value* o = d.state->get(s, "Owner");
        CHECK(o && o->get_if<emit::FIntrepidNetworkGUID>() && o->get_if<emit::FIntrepidNetworkGUID>()->ObjectId == 10341530, "Owner is the PlayerController 10341530");
    }
    {
        const Block& b = capture_blocks::kPlayerInfoOpen;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const Value* n = d.state->get(s, "CharacterName");
        CHECK(n && n->get_if<std::string>() && *n->get_if<std::string>() == "RandomChar", "UPlayerInfo: CharacterName RandomChar");
        const Value* a = d.state->get(s, "CharacterGuid.A");
        CHECK(a && a->get_if<int64_t>() && static_cast<uint32_t>(*a->get_if<int64_t>()) == 3635609113u, "CharacterGuid.A is an Int32 (sign extended)");
        const Value* t = d.state->get(s, "CurrentBreathingTime");
        CHECK(t && t->get_if<float>() && *t->get_if<float>() == 30.0f, "CurrentBreathingTime 30");
    }
}

static void test_decoded_values_components() {
    {
        const Block& b = capture_blocks::kPlayerInfoPossessed;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const Value* w = d.state->get(s, "WorldAttunements.AttunedBlocks");
        const ArrayValue* a = w ? w->get_if<ArrayValue>() : nullptr;
        const Value* inner = a && a->elements.size() == 1 && !a->elements[0].values.empty() ? &a->elements[0].values.begin()->second : nullptr;
        const ArrayValue* ia = inner ? inner->get_if<ArrayValue>() : nullptr;
        CHECK(ia && ia->elements.size() == 1 && ia->elements[0].values.size() == 1 && *ia->elements[0].values.at(1).get_if<uint64_t>() == 1,
              "UPlayerInfo after possession: WorldAttunements is an array of one element holding an array of one value (1)");
        CHECK(!d.state->get(s, "HeldItems.Items"), "the HeldItems fast array is not in the RepLayout block");
    }
    {
        const Block& b = capture_blocks::kOwnerInfo107;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const Value* r = d.state->get(s, "ResourceContainers");
        const ArrayValue* a = r ? r->get_if<ArrayValue>() : nullptr;
        CHECK(a && a->elements.size() == 3 && a->elements[0].values.size() == 1 && a->elements[1].values.empty() && a->elements[2].values.empty(),
              "OwnerInfo: ResourceContainers has 3 elements and the server sent element 0 only");
        const Value* n = d.state->get(s, "WeaponSkillLineId.Name");
        CHECK(n && n->get_if<NameValue>() && n->get_if<NameValue>()->text == "Weapon_1H_Scepter", "WeaponSkillLineId.Name is an FName");
    }
    {
        const Block& b = capture_blocks::kAbilityPawn107;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const ArrayValue* known = d.state->get(s, "KnownAbilities") ? d.state->get(s, "KnownAbilities")->get_if<ArrayValue>() : nullptr;
        const ArrayValue* recs = d.state->get(s, "ReplicatedRecordsToLoad") ? d.state->get(s, "ReplicatedRecordsToLoad")->get_if<ArrayValue>() : nullptr;
        CHECK(known && known->elements.size() == 57 && recs && recs->elements.size() == 58, "AbilityComponent: arrays of 57 and 58 elements");
        const Value* nm = recs && recs->elements[0].values.count(3) ? &recs->elements[0].values.at(3) : nullptr;
        CHECK(nm && nm->get_if<NameValue>() && nm->get_if<NameValue>()->hardcoded && nm->get_if<NameValue>()->index == 0,
              "a record's Name is the hardcoded FName None (EName 0)");
    }
}

static void test_decoded_values() {
    std::printf("what the captured blocks decode to\n");
    test_decoded_values_actors();
    test_decoded_values_components();
}

static void test_built_from_scratch_pawn_pc() {
    {
        const Block& b = capture_blocks::kPawnPossessed107;
        const auto& s = schema_of(b);
        RepState st;
        bool ok = st.set(s, "AuthServerIDReplicated", Value::u64(0));
        ok = st.set(s, "bHidden", Value::boolean(true)) && ok;
        ok = st.set(s, "RemoteRole", Value::u64(2)) && ok;
        ok = st.set(s, "Instigator", Value(guid(10341538))) && ok;
        ok = st.set(s, "PlayerState", Value(guid(10341528))) && ok;
        ok = st.set(s, "Controller", Value(guid(10341530))) && ok;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 477), "pawn pkt#107 (possession): 477 bits");
    }
    {
        const Block& b = capture_blocks::kPcOpenProps;
        const auto& s = schema_of(b);
        RepState st;
        bool ok = st.set(s, "AuthServerIDReplicated", Value::u64(0));
        ok = st.set(s, "RemoteRole", Value::u64(2)) && ok;
        ok = st.set(s, "Role", Value::u64(4)) && ok;
        ok = st.set(s, "PlayerState", Value(guid(10341528))) && ok;
        ok = st.set(s, "SpawnLocation", Value(Vec3d{-777664.5, 616578.6875, 15952.0498046875})) && ok;
        ok = st.set(s, "CaravanLaunchNode", Value::i64(0)) && ok;
        ok = st.set(s, "SummonCooldownTimer", Value::f32(1593462.375f)) && ok;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 519), "PC open pkt#22: 519 bits");
    }
    for (const Block* b : {&capture_blocks::kPcLoadTracker46, &capture_blocks::kPcLoadTracker72}) {
        const auto& s = schema_of(*b);
        LoadTracker lt;
        lt.flags.assign(25, b == &capture_blocks::kPcLoadTracker72);
        lt.flags[0] = true;
        if (b == &capture_blocks::kPcLoadTracker72) lt.flags[1] = false;
        RepState st;
        const bool ok = st.set(s, "CharacterLoadTracker", Value(lt));
        const auto bytes = capture_blocks::bytes_of(b->hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 823), b == &capture_blocks::kPcLoadTracker72 ? "PC pkt#72 CharacterLoadTracker: 823 bits" : "PC pkt#46 CharacterLoadTracker: 823 bits");
    }
}

static void test_built_from_scratch_movement_ps() {
    {
        const Block& b = capture_blocks::kPawnOpenActor;
        const auto& s = schema_of(b);
        RepMovement m;
        m.location = QuantVec{QuantVec::Mode::Scaled, -77766450, 61657869, 1595205};
        m.rotation = Rot{0, 65, 0};
        m.linear_velocity = quantize_vector(1, 0, 0, 0);
        RepState st;
        bool ok = st.set(s, "AuthServerIDReplicated", Value::u64(0));
        ok = st.set(s, "ReplicatedMovement", Value(m)) && ok;
        ok = st.set(s, "RemoteRole", Value::u64(1)) && ok;
        ok = st.set(s, "Owner", Value(guid(10341530))) && ok;
        ok = st.set(s, "Role", Value::u64(4)) && ok;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 332), "pawn open pkt#78 with FRepMovement: 332 bits");
    }
    {
        const Block& b = capture_blocks::kPlayerStateOpen434;
        const auto& s = schema_of(b);
        UniqueNetId id;
        id.flags = 8;
        id.id = "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492";
        RepState st;
        bool ok = st.set(s, "AuthServerIDReplicated", Value::u64(0));
        ok = st.set(s, "RemoteRole", Value::u64(1)) && ok;
        ok = st.set(s, "Owner", Value(guid(10341530))) && ok;
        ok = st.set(s, "Role", Value::u64(4)) && ok;
        ok = st.set(s, "PlayerId", Value::i64(1874)) && ok;
        ok = st.set(s, "StartTime", Value::i64(1593462)) && ok;
        ok = st.set(s, "UniqueId", Value(id)) && ok;
        ok = st.set(s, "PlayerNamePrivate", Value::str("DESKTOP-EXAMPLE1-US-")) && ok;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 967), "PlayerState open pkt#434 with FUniqueNetIdRepl and strings: 967 bits");
    }
}

static void test_built_from_scratch() {
    std::printf("blocks built from scratch with RepState::set\n");
    test_built_from_scratch_pawn_pc();
    test_built_from_scratch_movement_ps();
}

static void test_shadow() {
    std::printf("the shadow: what the client already has is not sent again\n");
    const auto& s = schema_of(capture_blocks::kPawnPossessed107);
    RepState st;
    st.set(s, "AuthServerIDReplicated", Value::u64(0));
    st.set(s, "bHidden", Value::boolean(true));
    st.set(s, "RemoteRole", Value::u64(2));
    st.set(s, "Instigator", Value(guid(10341538)));
    st.set(s, "PlayerState", Value(guid(10341528)));
    st.set(s, "Controller", Value(guid(10341530)));

    BunchWriter w;
    RepShadow shadow;
    const size_t first = write_rep_block(s, st, shadow, w);
    CHECK(first == 477 && shadow.sent.size() == 6, "the first send writes every set handle (477 bits) and records them in the shadow");
    CHECK(write_rep_block(s, st, shadow, w) == 0 && w.bit_pos() == 477, "a second write with no change writes nothing");

    st.set(s, "bHidden", Value::boolean(false));
    BunchWriter unhide;
    const size_t n = write_rep_block(s, st, shadow, unhide);
    const auto pkt2523 = capture_blocks::bytes_of(capture_blocks::kPawnUnhide2523.hex);
    CHECK(n == 18 && capture_blocks::same_bit_range(unhide.data(), pkt2523.data(), 18, nullptr),
          "changing bHidden writes that handle alone: the 18 bits of pkt#2523");
    CHECK(write_rep_block(s, st, shadow, unhide) == 0, "and then nothing more");

    st.set(s, "RemoteViewPitch16", Value::u64(61115));
    BunchWriter pitch;
    const size_t m = write_rep_block(s, st, shadow, pitch);
    const auto pkt7706 = capture_blocks::bytes_of(capture_blocks::kPawnViewPitch7706.hex);
    CHECK(m == 33 && capture_blocks::same_bit_range(pitch.data(), pkt7706.data(), 33, nullptr),
          "a value set later is written alone: the 33 bits of pkt#7706");

    st.set(s, "RemoteViewPitch16", Value::u64(61115));
    BunchWriter again;
    CHECK(write_rep_block(s, st, shadow, again) == 0 && again.bit_pos() == 0, "setting the same value again writes nothing");
    RepShadow other;
    BunchWriter fresh;
    CHECK(write_rep_block(s, st, other, fresh) > 33 && other.sent.size() == 7, "another connection's empty shadow gets every value");
    RepState none;
    RepShadow empty;
    BunchWriter nothing;
    CHECK(write_rep_block(s, none, empty, nothing) == 0 && nothing.bit_pos() == 0, "an empty state writes nothing");

    const auto& pc = schema_of(capture_blocks::kPcOpenProps);
    RepState f;
    f.set(pc, "SummonCooldownTimer", Value::f32(0.0f));
    RepShadow fs;
    BunchWriter fw;
    write_rep_block(pc, f, fs, fw);
    f.set(pc, "SummonCooldownTimer", Value::f32(-0.0f));
    CHECK(write_rep_block(pc, f, fs, fw) > 0, "-0.0 differs from 0.0: other bits go on the wire");
}

static void test_array_diff() {
    std::printf("dynamic arrays in the shadow\n");
    const Block& b = capture_blocks::kOwnerInfo107;
    const auto& s = schema_of(b);
    Decoded d = decode(b);
    RepShadow shadow;
    BunchWriter w;
    write_rep_block(s, *d.state, shadow, w);

    RepState st = *d.state;
    ArrayValue a = *st.get(s, "ResourceContainers")->get_if<ArrayValue>();
    a.elements[1].values[1] = Value(guid(10341599));
    st.set(s, "ResourceContainers", Value(a));
    BunchWriter delta;
    const size_t bits = write_rep_block(s, st, shadow, delta);
    CHECK(bits == 1 + 8 + 16 + 8 + 128 + 8 + 8, "one changed element: its handle and value, after the array count");
    size_t pos = 0;
    const auto back = read_rep_block(s, delta.data(), delta.bit_pos(), pos);
    const ArrayValue* got = back && back->get(s, "ResourceContainers") ? back->get(s, "ResourceContainers")->get_if<ArrayValue>() : nullptr;
    CHECK(back && back->values().size() == 1 && got && got->elements.size() == 3 && got->elements[0].values.empty() &&
              got->elements[1].values.size() == 1 && got->elements[2].values.empty(),
          "it reads back as count 3 with element 1 alone (the other properties and elements are not repeated)");

    ArrayValue grown = a;
    grown.elements.resize(4);
    grown.elements[3].values[1] = Value(guid(10341600));
    st.set(s, "ResourceContainers", Value(grown));
    BunchWriter grow;
    const size_t g = write_rep_block(s, st, shadow, grow);
    CHECK(g == 1 + 8 + 16 + 8 + 128 + 8 + 8, "a fourth element: the count and that element's handle 4");
    grown.elements.resize(1);
    st.set(s, "ResourceContainers", Value(grown));
    BunchWriter shrink;
    CHECK(write_rep_block(s, st, shadow, shrink) == 1 + 8 + 16 + 8 + 8, "shrinking to one element: the count and no element");
    CHECK(write_rep_block(s, st, shadow, shrink) == 0, "and the shadow then holds the one-element array");

    const Block& pb = capture_blocks::kPlayerInfoPossessed;
    const auto& ps = schema_of(pb);
    Decoded pd = decode(pb);
    RepShadow pshadow;
    BunchWriter pw;
    write_rep_block(ps, *pd.state, pshadow, pw);
    RepState pst = *pd.state;
    ArrayValue outer = *pst.get(ps, "WorldAttunements.AttunedBlocks")->get_if<ArrayValue>();
    ArrayValue inner = *outer.elements[0].values.at(1).get_if<ArrayValue>();
    inner.elements[0].values[1] = Value::u64(3);
    outer.elements[0].values[1] = Value(inner);
    pst.set(ps, "WorldAttunements.AttunedBlocks", Value(outer));
    BunchWriter pd2;
    const size_t nb = write_rep_block(ps, pst, pshadow, pd2);
    CHECK(nb == 1 + 8 + 16 + 8 + 16 + 8 + 8 + 8 + 8 + 8, "a changed value inside a nested array writes the path of handles down to it");
}

static void test_set_refusals() {
    std::printf("RepState::set refuses what the schema does not carry\n");
    const auto& pawn = schema_of(capture_blocks::kPawnPossessed107);
    RepState st;
    CHECK(!st.set(pawn, "NoSuchProperty", Value::u64(0)), "an unknown path");
    CHECK(!st.set(pawn, 0u, Value::u64(0)) && !st.set(pawn, 9999u, Value::u64(0)), "handle 0 and a handle beyond the commands");
    CHECK(!st.set(pawn, "bHidden", Value::u64(1)), "a bool command with an unsigned value");
    CHECK(!st.set(pawn, "RemoteRole", Value::boolean(true)), "an enum command with a bool");
    CHECK(!st.set(pawn, "RemoteRole", Value::u64(8)), "an enum of 3 bits with 8");
    CHECK(st.set(pawn, "RemoteRole", Value::u64(7)), "and with 7 is fine");
    const uint32_t role = pawn.command("RemoteRole")->handle;
    CHECK(!st.get(role + 65536) && !st.erase(role + 65536) && st.get(role),
          "an out-of-range handle cannot alias or erase a valid 16-bit handle");
    CHECK(!st.set(pawn, "Owner", Value::u64(5)), "an object command with an integer");
    CHECK(!st.set(pawn, "Owner", Value(guid(1))), "an object command with ObjectId 1 (the export form)");
    CHECK(!st.set(pawn, "ReplicatedMovement", Value(Vec3d{})), "an FRepMovement command with an FVector");
    CHECK(!st.set(pawn, "ReplicatedBasedMovement.BaseID", Value::u64(65536)), "a 16-bit command with 65536");
    CHECK(!st.set(pawn, "ReplicatedMovementMode", Value::i64(1)), "an unsigned command with a signed value");
    CHECK(!st.set(pawn, "JumpMaxCount", Value::i64(int64_t(1) << 31)), "a signed 32-bit command with 2^31");
    CHECK(st.set(pawn, "JumpMaxCount", Value::i64(-5)), "and with -5 is fine");
    CHECK(st.get(pawn, "RemoteRole") && st.get(pawn, "JumpMaxCount") && !st.get(pawn, "bHidden") && st.values().size() == 2,
          "refused values leave no trace");
    CHECK(!st.set(pawn, "RepRootMotion.AuthoritativeRootMotion", Value(Vec3d{})),
          "a struct without a codec (FRootMotionSourceGroup)");

    const auto& info = schema_of(capture_blocks::kPlayerInfoPossessed);
    CHECK(!st.set(info, "HeldItems.Items", Value(ArrayValue{})), "a custom-delta (fast array) property: it goes in the field loop");
    CHECK(!st.set(info, "CharacterName", Value::str("bad \xff utf8")), "a string that is not UTF-8");
    ArrayValue bad;
    bad.elements.resize(1);
    bad.elements[0].values[1] = Value::str("wrong kind");
    CHECK(!st.set(info, "WorldAttunements.AttunedBlocks", Value(bad)), "an array whose element has the wrong kind");
    ArrayValue bad_handle;
    bad_handle.elements.resize(1);
    bad_handle.elements[0].values[5] = Value::u64(1);
    CHECK(!st.set(info, "WorldAttunements.AttunedBlocks", Value(bad_handle)), "an array element with a local handle beyond the element commands");

    const auto* align = gen::find_class_schema("UAlignmentComponent");
    CHECK(align && !RepState().set(*align, "Alignments", Value(Vec3d{})), "AoC's FAlignmentContainer needs its own value type");
}

static void test_write_failure() {
    std::printf("a value the wire cannot carry\n");
    const auto& s = schema_of(capture_blocks::kPawnOpenActor);
    RepMovement m;
    m.rotation = Rot{0, 300, 0};
    RepState st;
    st.set(s, "AuthServerIDReplicated", Value::u64(0));
    st.set(s, "ReplicatedMovement", Value(m));
    BunchWriter w;
    w.write_bits(5, 3);
    RepShadow shadow;
    bool failed = false;
    CHECK(write_rep_block(s, st, shadow, w, {}, &failed) == 0 && failed && w.bit_pos() == 3 && shadow.empty(),
          "write_rep_block returns 0 with failed set, the writer as it was and the shadow untouched");
    CodecOptions shorts;
    shorts.rep_movement_short_rotation = true;
    failed = true;
    CHECK(write_rep_block(s, st, shadow, w, shorts, &failed) > 0 && !failed, "with short rotation components it is written");
}

static void test_read_refusals_malformed() {
    const auto& s = schema_of(capture_blocks::kPawnPossessed107);
    auto attempt = [&](const BunchWriter& w, size_t cut = 0) {
        size_t pos = 0;
        const auto r = read_rep_block(s, w.data(), w.bit_pos() - cut, pos);
        return std::make_pair(r.has_value(), pos);
    };
    {
        BunchWriter w;
        w.write_bit(1);
        w.write_sip(0);
        CHECK(!attempt(w).first, "a set bDoChecksum bit");
    }
    {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(3); w.write_bit(1);
        w.write_sip(2); w.write_bit(1);
        w.write_sip(0);
        CHECK(!attempt(w).first, "handles that do not ascend");
    }
    {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(4000); w.write_bit(1);
        w.write_sip(0);
        CHECK(!attempt(w).first, "a handle beyond the class's commands");
    }
    {
        const auto& stats = *gen::find_class_schema("UAoCStatsComponent");
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(4);
        w.write_bits(0, 16); w.write_sip(0); w.write_sip(0);
        size_t pos = 0;
        CHECK(!read_rep_block(stats, w.data(), w.bit_pos(), pos).has_value(), "a custom-delta handle (fast array)");
    }
    {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(3); w.write_bit(1);
        CHECK(!attempt(w).first, "a block without its closing handle 0");
    }
}

static void test_read_refusals_bounds() {
    const auto& s = schema_of(capture_blocks::kPawnPossessed107);
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kPawnPossessed107.hex);
        size_t pos = 0;
        CHECK(!read_rep_block(s, bytes.data(), 470, pos).has_value() && pos == 0, "a block cut short (pos stays)");
        pos = 0;
        CHECK(read_rep_block(s, bytes.data(), 477, pos).has_value() && pos == 477, "the whole block reads and pos moves to its end");
        pos = 477;
        CHECK(!read_rep_block(s, bytes.data(), 477, pos).has_value(), "no block at the end of the data");
    }
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kPawnUnhide2523.hex);
        BunchWriter w;
        w.write_bits(0x15, 5);
        w.write_bit_range(bytes.data(), 0, 18);
        size_t pos = 5;
        const auto r = read_rep_block(s, w.data(), w.bit_pos(), pos);
        CHECK(r && pos == 23 && r->get(s, "bHidden") && *r->get(s, "bHidden") == Value::boolean(false), "a block at a bit offset reads and leaves pos after it");
    }
}

static void test_read_refusals() {
    std::printf("read_rep_block refuses malformed blocks\n");
    test_read_refusals_malformed();
    test_read_refusals_bounds();
}

static void test_field_loop() {
    std::printf("field loop header after the block: UPlayerInfo HeldItems\n");
    const Block& b = capture_blocks::kPlayerInfoPossessed;
    const auto& s = schema_of(b);
    const auto bytes = capture_blocks::bytes_of(b.hex);
    Decoded d = decode(b);
    CHECK(d.state && d.pos == 165 && b.bits == 2868, "the RepLayout block is 165 of the 2868 bits");
    size_t pos = d.pos;
    const auto h = read_field_header(s, bytes.data(), b.bits, pos);
    CHECK(h && h->field == 28 && h->payload_bits == 2682 && pos + h->payload_bits == b.bits,
          "field 28 with a 2682-bit payload, ending the block");
    CHECK(s.field(28) && std::string(s.field(28)->name) == "HeldItems", "field 28 is HeldItems");

    BunchWriter w;
    CHECK(write_field_header(s, 28, 2682, w), "write_field_header accepts field 28");
    const auto captured_header = slice_bits(bytes, d.pos, w.bit_pos());
    CHECK(pos - d.pos == w.bit_pos() && capture_blocks::same_bit_range(w.data(), captured_header.data(), w.bit_pos(), nullptr),
          "and the header is the captured bits (SerializeInt(28, 198) + SIP(2682))");

    BunchWriter payload;
    payload.write_bits(0x2d, 6);
    BunchWriter header6;
    write_field_header(s, 28, 6, header6);
    BunchWriter framed;
    CHECK(write_field(s, 28, payload, framed) == header6.bit_pos() + 6, "write_field appends the payload's bits after the header");
    size_t p2 = 0;
    const auto back = read_field_header(s, framed.data(), framed.bit_pos(), p2);
    CHECK(back && back->field == 28 && back->payload_bits == 6 && p2 == header6.bit_pos(), "and read_field_header reads it back");

    BunchWriter bad;
    CHECK(!write_field_header(s, s.num_fields(), 0, bad) && bad.bit_pos() == 0, "a field beyond the class is not written");
    BunchWriter over;
    write_field_header(s, 28, 100, over);
    size_t p3 = 0;
    CHECK(!read_field_header(s, over.data(), over.bit_pos(), p3).has_value() && p3 == 0, "a payload that runs past the block is refused");
    BunchWriter beyond;
    beyond.write_serialize_int(s.num_fields(), s.field_max());
    beyond.write_sip(0);
    size_t p4 = 0;
    CHECK(!read_field_header(s, beyond.data(), beyond.bit_pos(), p4).has_value(), "field number NumFields is refused");
}

int main() {
    test_captured_blocks();
    test_decoded_values();
    test_built_from_scratch();
    test_shadow();
    test_array_diff();
    test_set_refusals();
    test_write_failure();
    test_read_refusals();
    test_field_loop();
    run_repl_rep_block_part2();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
