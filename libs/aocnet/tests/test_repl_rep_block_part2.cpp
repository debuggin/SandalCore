// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_repl_rep_block_common.h"

#include <climits>
#include <new>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
namespace schema = aoc::protocol::schema;

static size_t g_live = 0, g_peak = 0;

void* operator new(size_t n) {
    void* p = std::malloc(n + 16);
    if (!p) throw std::bad_alloc();
    *static_cast<size_t*>(p) = n;
    g_live += n;
    if (g_live > g_peak) g_peak = g_live;
    return static_cast<char*>(p) + 16;
}
void operator delete(void* q) noexcept {
    if (!q) return;
    char* p = static_cast<char*>(q) - 16;
    g_live -= *reinterpret_cast<size_t*>(p);
    std::free(p);
}
void operator delete(void* q, size_t) noexcept { operator delete(q); }

struct HostileResult { bool ok; size_t peak; };

static HostileResult attempt_hostile(const schema::ClassSchema& cls, const BunchWriter& w) {
    size_t pos = 0;
    const size_t base = g_live;
    g_peak = base;
    const bool ok = read_rep_block(cls, w.data(), w.bit_pos(), pos).has_value();
    return HostileResult{ok, g_peak - base};
}

static void test_hostile_counts_nested() {
    const schema::ClassSchema& info = schema_named("UPlayerInfo");
    const schema::RepCommand* attuned = info.command("WorldAttunements.AttunedBlocks");
    CHECK(attuned && attuned->type == schema::PropType::DynamicArray && info.elements(*attuned).size() == 1 &&
              info.elements(*attuned)[0].type == schema::PropType::DynamicArray,
          "WorldAttunements.AttunedBlocks: an array whose element is an array");
    if (!attuned) return;
    auto block = [&](uint64_t outer, uint64_t inner, size_t nested) {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(attuned->handle);
        w.write_bits(outer, 16);
        for (size_t k = 0; k < nested; ++k) {
            w.write_sip(k + 1);
            w.write_bits(inner, 16);
            w.write_sip(0);
        }
        w.write_sip(0);
        w.write_sip(0);
        return w;
    };
    {
        const BunchWriter w = block(65535, 65535, 20);
        const HostileResult r = attempt_hostile(info, w);
        std::printf("    %zu bytes of input: peak allocation %zu bytes\n", w.byte_size(), r.peak);
        CHECK(!r.ok && r.peak < (1u << 20), "86 bytes of 65535-element arrays within 65535-element arrays: refused without the allocation");
    }
    {
        const BunchWriter w = block(65535, 65535, 4000);
        const HostileResult r = attempt_hostile(info, w);
        CHECK(!r.ok && r.peak < (1u << 20), "16 KB of the same: refused, quickly");
    }
    {
        const BunchWriter w = block(kMaxBlockElements, kMaxBlockElements, 500);
        const HostileResult r = attempt_hostile(info, w);
        std::printf("    %zu bytes of input: peak allocation %zu bytes\n", w.byte_size(), r.peak);
        CHECK(!r.ok && r.peak < (8u << 20), "an outer array that uses the whole budget leaves none for the inner ones: refused, at most one array built");
    }
}

static void test_hostile_counts_budget() {
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    const schema::RepCommand* containers = owner.command("ResourceContainers");
    const schema::RepCommand* attackers = owner.command("NpcAttackers");
    CHECK(containers && attackers && containers->type == schema::PropType::DynamicArray && attackers->type == schema::PropType::DynamicArray,
          "UOwnerInfoComponent has the arrays ResourceContainers and NpcAttackers");
    if (!containers || !attackers) return;
    auto two_arrays = [&](uint64_t first, uint64_t second) {
        BunchWriter w;
        w.write_bit(0);
        const schema::RepCommand* lo = containers->handle < attackers->handle ? containers : attackers;
        const schema::RepCommand* hi = lo == containers ? attackers : containers;
        w.write_sip(lo->handle);
        w.write_bits(lo == containers ? first : second, 16);
        w.write_sip(0);
        w.write_sip(hi->handle);
        w.write_bits(hi == containers ? first : second, 16);
        w.write_sip(0);
        w.write_sip(0);
        return w;
    };
    CHECK(attempt_hostile(owner, two_arrays(kMaxBlockElements / 2, kMaxBlockElements / 2)).ok, "two arrays that together fit the budget read");
    CHECK(!attempt_hostile(owner, two_arrays(kMaxBlockElements / 2, kMaxBlockElements / 2 + 1)).ok, "one element more in all: refused");
    CHECK(attempt_hostile(owner, two_arrays(kMaxBlockElements, 0)).ok && !attempt_hostile(owner, two_arrays(kMaxBlockElements + 1, 0)).ok,
          "an array of exactly kMaxBlockElements reads, one more does not");
    {
        ArrayValue three;
        three.elements.resize(3);
        three.elements[1].values[1] = Value(guid(10341599));
        BunchWriter w;
        const bool wrote = write_value(*containers, owner.element_commands, Value(three), w);
        wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
        const auto v = wrote ? read_value(*containers, owner.element_commands, r) : std::nullopt;
        CHECK(v && *v == Value(three) && r.pos() == w.bit_pos(), "read_value reads an array on its own");
    }
}

static void test_hostile_counts() {
    std::printf("a hostile array count builds a bounded number of elements\n");
    test_hostile_counts_nested();
    test_hostile_counts_budget();
}

static void t5_array_element_handles() {
    std::printf("array element handles that do not fit the count or do not ascend\n");
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    const schema::RepCommand* arr = owner.command("ResourceContainers");
    auto block = [&](uint64_t count, std::initializer_list<uint64_t> handles) {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(arr->handle);
        w.write_bits(count, 16);
        for (uint64_t h : handles) {
            w.write_sip(h);
            emit::write_intrepid_guid(w, guid(10341599));
        }
        w.write_sip(0);
        w.write_sip(0);
        return w;
    };
    auto reads = [&](const BunchWriter& w) {
        size_t pos = 0;
        return read_rep_block(owner, w.data(), w.bit_pos(), pos).has_value();
    };
    CHECK(arr && arr->type == schema::PropType::DynamicArray && arr->elem_end - arr->elem_begin == 1, "ResourceContainers: an array of one command per element");
    CHECK(reads(block(3, {1, 3})), "control: elements 0 and 2 of 3");
    CHECK(!reads(block(2, {3})), "element 2 of 2 (handle 3): refused");
    CHECK(!reads(block(0, {1})), "element 0 of an empty array: refused");
    CHECK(!reads(block(3, {2, 1})), "element handles 2 then 1 (descending): refused");
    CHECK(!reads(block(3, {2, 2})), "element handle 2 twice: refused");
}

static void t7_array_bound() {
    std::printf("an array of 65536 elements\n");
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    ArrayValue big;
    big.elements.resize(65536);
    RepState st;
    CHECK(!st.set(owner, "ResourceContainers", Value(big)), "65536 elements do not fit the 16-bit count");
    big.elements.resize(65535);
    CHECK(st.set(owner, "ResourceContainers", Value(big)), "65535 do");
}

static bool set_trips(const schema::ClassSchema& cls, const char* path, const Value& v) {
    RepState st;
    if (!st.set(cls, path, v)) return false;
    BunchWriter w;
    RepShadow sh;
    bool failed = true;
    const size_t n = write_rep_block(cls, st, sh, w, {}, &failed);
    size_t pos = 0;
    const auto back = failed || !n ? std::nullopt : read_rep_block(cls, w.data(), w.bit_pos(), pos);
    return back && *back == st;
}

static bool set_refused(const schema::ClassSchema& cls, const char* path, const Value& v) {
    return !RepState().set(cls, path, v);
}

static void test_set_matches_writer_quant_and_move() {
    const auto& loc = schema_named("PlayerPawn_C");
    const char* offset = "AttachmentReplication.LocationOffset";
    const int64_t big63 = (int64_t(1) << 62) - 1;
    CHECK(set_trips(loc, offset, Value(QuantVec{QuantVec::Mode::Scaled, big63, -(int64_t(1) << 62), 5})), "a quantized component of 63 bits is written and read back");
    CHECK(set_refused(loc, offset, Value(QuantVec{QuantVec::Mode::Scaled, INT64_MAX, 0, 0})), "INT64_MAX (64 bits) is refused");
    CHECK(set_refused(loc, offset, Value(QuantVec{QuantVec::Mode::Scaled, INT64_MIN, 0, 0})), "INT64_MIN is refused");
    CHECK(set_refused(loc, offset, Value(QuantVec{QuantVec::Mode::Unscaled, int64_t(1) << 62, 0, 0})), "2^62 (64 bits) is refused");
    CHECK(set_refused(loc, offset, Value(QuantVec{QuantVec::Mode::Scaled, 0, -(int64_t(1) << 62) - 1, 0})), "-2^62 - 1 is refused");
    BunchWriter scratch;
    CHECK(!write_quantized_vector(scratch, QuantVec{QuantVec::Mode::Scaled, INT64_MAX, 0, 0}) && scratch.bit_pos() == 0 &&
              !quant_vec_fits(QuantVec{QuantVec::Mode::Scaled, INT64_MAX, 0, 0}) && quant_vec_fits(QuantVec{QuantVec::Mode::Scaled, big63, 0, 0}),
          "the writer refuses the same values and writes nothing");
    CHECK(set_trips(loc, offset, Value(QuantVec{QuantVec::Mode::RawDouble, INT64_MIN, 0, INT64_MAX})), "full precision doubles carry any bits");
    CHECK(set_trips(loc, offset, Value(QuantVec{QuantVec::Mode::RawFloat, 0x3f800000, 0, UINT32_MAX})), "a full precision float vector carries 32 bits");
    CHECK(set_refused(loc, offset, Value(QuantVec{QuantVec::Mode::RawFloat, int64_t(UINT32_MAX) + 1, 0, 0})), "a float component beyond 32 bits is refused");
    CHECK(set_refused(loc, offset, Value(QuantVec{QuantVec::Mode::RawFloat, -1, 0, 0})), "and a negative one");

    RepMovement m;
    m.location = QuantVec{QuantVec::Mode::Scaled, 100, 200, 300};
    CHECK(set_trips(loc, "ReplicatedMovement", Value(m)), "an FRepMovement with small vectors is written and read back");
    RepMovement wide = m;
    wide.location.x = INT64_MAX;
    CHECK(set_refused(loc, "ReplicatedMovement", Value(wide)), "a 64-bit location component is refused");
    wide = m;
    wide.linear_velocity.z = INT64_MIN;
    CHECK(set_refused(loc, "ReplicatedMovement", Value(wide)), "a 64-bit velocity component is refused");
    wide = m;
    wide.rep_physics = true;
    wide.angular_velocity.y = INT64_MAX;
    CHECK(set_refused(loc, "ReplicatedMovement", Value(wide)), "a 64-bit angular velocity is refused when it goes out (bRepPhysics)");
    wide = m;
    wide.acceleration = QuantVec{QuantVec::Mode::Scaled, 0, 0, INT64_MAX};
    CHECK(set_refused(loc, "ReplicatedMovement", Value(wide)), "a 64-bit acceleration is refused");
    wide = m;
    wide.rotation = Rot{0, 300, 0};
    RepState bytes_rotation;
    BunchWriter rw;
    RepShadow rs;
    bool failed = false;
    CHECK(bytes_rotation.set(loc, "ReplicatedMovement", Value(wide)) && write_rep_block(loc, bytes_rotation, rs, rw, {}, &failed) == 0 && failed,
          "a rotation beyond a byte stays a write-time failure: set cannot know the object's rotation quantization");
}

static void test_set_matches_writer_ids_and_strings() {
    const auto& info = schema_named("UPlayerInfo");
    const auto& ps = schema_named("AoCPlayerStateBP_C");
    const schema::RepCommand* unique_id = ps.command("UniqueId");
    CHECK(unique_id != nullptr, "AoCPlayerStateBP_C.UniqueId");
    if (unique_id) {
        UniqueNetId id;
        id.flags = 1 | (5 << 3);
        id.bytes.assign(255, 7);
        CHECK(set_trips(ps, "UniqueId", Value(id)), "an encoded id of 255 bytes is written and read back");
        id.bytes.assign(256, 7);
        CHECK(set_refused(ps, "UniqueId", Value(id)), "256 bytes are refused (the length is a byte)");
        id.flags = 1 | (30 << 3);
        id.services = 3;
        id.bytes.assign(300, 7);
        CHECK(set_trips(ps, "UniqueId", Value(id)), "a V2 id of 300 bytes is fine");
        UniqueNetId named;
        named.flags = 1 | (31 << 3);
        named.type = std::string("a\0b", 3);
        CHECK(set_refused(ps, "UniqueId", Value(named)), "a type name with a NUL is refused");
        UniqueNetId text;
        text.flags = 8;
        text.id = std::string("x\0y", 3);
        CHECK(set_refused(ps, "UniqueId", Value(text)), "an unencoded id string with a NUL is refused");
        text.id = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
        CHECK(set_trips(ps, "UniqueId", Value(text)), "and a plain one is fine");
        UniqueNetId empty;
        empty.flags = 3;
        empty.bytes.assign(1000, 1);
        CHECK(struct_accepts("FUniqueNetIdRepl", Value(empty)), "an empty id has no bytes on the wire: its leftover bytes do not matter");
    }

    CHECK(set_trips(info, "CharacterName", Value::str(std::string(65535, 'x'))), "a string of 65,535 characters is written and read back");
    CHECK(set_refused(info, "CharacterName", Value::str(std::string(65536, 'x'))), "65,536 characters are refused");
    CHECK(set_refused(info, "CharacterName", Value::str(std::string("ab\0cd", 5))), "a string with an embedded NUL is refused");
    std::string accents;
    for (int i = 0; i < 65535; ++i) accents += "\xC3\xA9";
    CHECK(set_trips(info, "CharacterName", Value::str(accents)), "65,535 two-byte characters (131,070 bytes: the limit counts characters) are fine");
    accents += "\xC3\xA9";
    CHECK(set_refused(info, "CharacterName", Value::str(accents)), "and 65,536 are not");
    std::string faces;
    for (int i = 0; i < 32768; ++i) faces += "\xF0\x9F\x98\x80";
    CHECK(set_refused(info, "CharacterName", Value::str(faces)), "32,768 surrogate pairs (65,536 UCS-2 units) are refused");
    CHECK(set_refused(info, "CharacterName", Value::str(std::string("\xC3\xA9\0", 3))), "a NUL after a non-ASCII character is refused too");
    BunchWriter sw;
    CHECK(!write_fstring(sw, std::string(65536, 'x')) && !write_fstring(sw, std::string("a\0b", 3)) && sw.bit_pos() == 0 &&
              write_fstring(sw, std::string(65535, 'x')),
          "write_fstring refuses the same and writes nothing");
}

static void test_set_matches_writer_names_texts_tags() {
    schema::RepCommand cmd{};
    cmd.type = schema::PropType::Name;
    const std::string nul("n\0m", 3);
    CHECK(!value_matches(cmd, {}, Value(NameValue::plain(nul))) && value_matches(cmd, {}, Value(NameValue::plain("ok"))) &&
              !value_matches(cmd, {}, Value(NameValue::plain(std::string(65536, 'n')))),
          "an FName with a NUL or a string of 65,536 characters is refused");
    NameValue hard = NameValue::hardcoded_name(5);
    hard.text = nul;
    CHECK(value_matches(cmd, {}, Value(hard)), "a hardcoded name has no text on the wire: its leftover text does not matter");
    cmd.type = schema::PropType::Text;
    TextValue t;
    t.history = 0;
    t.ns = "UI";
    t.key = "Key";
    t.source = "text";
    CHECK(value_matches(cmd, {}, Value(t)), "an FText of the Base history");
    t.key = nul;
    CHECK(!value_matches(cmd, {}, Value(t)), "with a NUL in its key is refused");
    t.key = "Key";
    t.history = 3;
    CHECK(!value_matches(cmd, {}, Value(t)), "a history that is not decoded is refused");
    TextValue none;
    none.history = 0xFF;
    none.has_string = true;
    none.source = std::string(65536, 's');
    CHECK(!value_matches(cmd, {}, Value(none)), "an FText string of 65,536 characters is refused");
    none.has_string = false;
    CHECK(value_matches(cmd, {}, Value(none)), "and is not checked when it does not go out");
    cmd.type = schema::PropType::SoftObject;
    SoftObjectValue so;
    so.is_path = true;
    so.path = nul;
    CHECK(!value_matches(cmd, {}, Value(so)), "a soft object path with a NUL is refused");
    so.path = "/Game/A";
    CHECK(value_matches(cmd, {}, Value(so)), "and a plain one is fine");
    CHECK(!struct_accepts("FGameplayTag", Value(GameplayTag{NameValue::plain(nul)})) &&
              !struct_accepts("FGameplayTagContainer", Value(GameplayTagContainer{{NameValue::plain("ok"), NameValue::plain(nul)}})) &&
              struct_accepts("FGameplayTagContainer", Value(GameplayTagContainer{{NameValue::plain("ok")}})),
          "a gameplay tag or a tag of a container with a NUL is refused");
}

static void test_set_matches_writer() {
    std::printf("RepState::set accepts exactly what the writer can write\n");
    test_set_matches_writer_quant_and_move();
    test_set_matches_writer_ids_and_strings();
    test_set_matches_writer_names_texts_tags();
}

static void test_removed_element_values_clear() {
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    ArrayValue a;
    a.elements.resize(3);
    a.elements[1].values[1] = Value(guid(10341599));
    RepState st;
    RepShadow shadow;
    BunchWriter w;
    st.set(owner, "ResourceContainers", Value(a));
    CHECK(write_rep_block(owner, st, shadow, w) == 1 + 8 + 16 + 8 + 128 + 8 + 8, "the first send: element 1 alone");
    a.elements[1].values.clear();
    st.set(owner, "ResourceContainers", Value(a));
    BunchWriter cleared;
    bool failed = true;
    const size_t bits = write_rep_block(owner, st, shadow, cleared, {}, &failed);
    CHECK(bits == 1 + 8 + 16 + 8 + 128 + 8 + 8 && !failed, "clearing it writes the element again, as the null object");
    size_t pos = 0;
    const auto back = read_rep_block(owner, cleared.data(), cleared.bit_pos(), pos);
    const Value* arr = back ? back->get(owner, "ResourceContainers") : nullptr;
    const ArrayValue* got = arr ? arr->get_if<ArrayValue>() : nullptr;
    CHECK(got && got->elements.size() == 3 && got->elements[1].values.size() == 1 &&
              got->elements[1].values.at(1) == Value(emit::FIntrepidNetworkGUID{}),
          "it reads back as element 1 = the null object (the client no longer keeps the stale GUID)");
    CHECK(write_rep_block(owner, st, shadow, cleared) == 0, "and then nothing more: the client has the zero");

    ArrayValue z;
    z.elements.resize(2);
    z.elements[0].values[1] = Value(emit::FIntrepidNetworkGUID{});
    RepState zs;
    RepShadow zsh;
    BunchWriter zw;
    zs.set(owner, "ResourceContainers", Value(z));
    write_rep_block(owner, zs, zsh, zw);
    z.elements[0].values.clear();
    zs.set(owner, "ResourceContainers", Value(z));
    CHECK(write_rep_block(owner, zs, zsh, zw) == 0, "a value that was the zero and is gone sends nothing");
}

static void test_removed_element_values_nested_and_look() {
    const schema::ClassSchema& info = schema_named("UPlayerInfo");
    ArrayValue inner;
    inner.elements.resize(1);
    inner.elements[0].values[1] = Value::u64(1);
    ArrayValue outer;
    outer.elements.resize(1);
    outer.elements[0].values[1] = Value(inner);
    RepState is;
    RepShadow ish;
    BunchWriter iw;
    is.set(info, "WorldAttunements.AttunedBlocks", Value(outer));
    write_rep_block(info, is, ish, iw);
    outer.elements[0].values.clear();
    is.set(info, "WorldAttunements.AttunedBlocks", Value(outer));
    BunchWriter ow;
    CHECK(write_rep_block(info, is, ish, ow) == 1 + 8 + 16 + 8 + 16 + 8 + 8 + 8, "a removed inner array goes out as count 0");

    const schema::ClassSchema& look = schema_named("UCharacterAppearanceComponent");
    ArrayValue ids;
    ids.elements.resize(1);
    ids.elements[0].values[1] = Value(AppearanceId{1, 2, 3, 4});
    RepState ls;
    RepShadow lsh;
    BunchWriter lw;
    ls.set(look, "AppearanceIDs", Value(ids));
    CHECK(write_rep_block(look, ls, lsh, lw) > 0, "AppearanceIDs: one element sent");
    ids.elements[0].values.clear();
    ls.set(look, "AppearanceIDs", Value(ids));
    const RepShadow before = lsh;
    BunchWriter lw2;
    bool failed = false;
    CHECK(write_rep_block(look, ls, lsh, lw2, {}, &failed) == 0 && failed && lw2.bit_pos() == 0 && lsh.sent == before.sent,
          "an FAppearanceId has no known zero: removing it fails the write, the shadow untouched");
    CHECK(zero_value(*look.command("AppearanceIDs")).has_value() && !zero_value(look.elements(*look.command("AppearanceIDs"))[0]).has_value(),
          "(the array's zero is empty, the struct's is unknown)");
}

static void test_removed_element_values() {
    std::printf("an array element value that was sent and is gone is sent as its zero\n");
    test_removed_element_values_clear();
    test_removed_element_values_nested_and_look();
}

static void test_parent_mask() {
    std::printf("the parent mask: an inactive parent is left out of the block and the shadow\n");
    const auto& s = schema_named("PlayerPawn_C");
    RepState st;
    st.set(s, "bHidden", Value::boolean(true));
    RepMovement m;
    m.location = QuantVec{QuantVec::Mode::Scaled, 1, 2, 3};
    st.set(s, "ReplicatedMovement", Value(m));
    const schema::RepCommand* movement = s.command("ReplicatedMovement");
    const schema::RepCommand* hidden = s.command("bHidden");

    ParentMask mask;
    CHECK(mask.all_active() && mask.active(movement->parent), "everything is active until the caller says otherwise");
    CHECK(mask.set_active(s, "ReplicatedMovement", false) && !mask.all_active() && !mask.active(movement->parent) && mask.active(hidden->parent),
          "set_active by path turns one parent off");
    RepShadow shadow;
    BunchWriter w;
    RepShadowUndo undo;
    bool failed = true;
    const size_t bits = write_rep_block(s, st, shadow, w, {}, &failed, &mask, &undo);
    size_t pos = 0;
    const auto back = bits ? read_rep_block(s, w.data(), w.bit_pos(), pos) : std::nullopt;
    CHECK(!failed && back && back->values().size() == 1 && back->get(hidden->handle) && shadow.sent.size() == 1 && shadow.sent.count(hidden->handle),
          "only the active parent's handle is written and recorded");
    CHECK(undo.entries.size() == 1 && undo.entries[0].first == hidden->handle && !undo.entries[0].second,
          "the undo record has that handle, which the shadow did not hold");
    undo.apply(shadow);
    CHECK(shadow.empty(), "applying it puts the shadow back");

    mask.set_active(s, "bHidden", false);
    BunchWriter none;
    CHECK(write_rep_block(s, st, shadow, none, {}, &failed, &mask) == 0 && !failed && none.bit_pos() == 0 && shadow.empty(), "every parent inactive: nothing");
    mask.reset();
    BunchWriter all;
    CHECK(write_rep_block(s, st, shadow, all, {}, &failed, &mask) > 0 && shadow.sent.size() == 2, "reset: everything is active again");

    RepState st2 = st;
    st2.set(s, "bHidden", Value::boolean(false));
    RepShadowUndo undo2;
    BunchWriter again;
    CHECK(write_rep_block(s, st2, shadow, again, {}, &failed, nullptr, &undo2) > 0 && *shadow.sent.at(hidden->handle).get_if<bool>() == false,
          "an update that changes bHidden");
    undo2.apply(shadow);
    CHECK(shadow.sent.size() == 2 && *shadow.sent.at(hidden->handle).get_if<bool>() == true, "its undo restores the old value");

    const auto& owner = schema_named("UOwnerInfoComponent");
    RepState es;
    es.set(owner, "ResourceContainers", Value(ArrayValue{}));
    RepShadow esh;
    BunchWriter ew;
    CHECK(write_rep_block(owner, es, esh, ew) == 1 + 8 + 16 + 8 + 8, "an empty array on a first send: count 0");
}

void run_repl_rep_block_part2() {
    test_hostile_counts();
    t5_array_element_handles();
    t7_array_bound();
    test_set_matches_writer();
    test_removed_element_values();
    test_parent_mask();
}
