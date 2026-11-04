// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "bootstrap/bootstrap.h"

#include "bootstrap/constants.h"
#include "protocol/repl/move_packed.h"
#include "protocol/schema/generated/index.h"

#include <initializer_list>
#include <string_view>
#include <vector>

namespace aoc::worldd::bootstrap {

using protocol::emit::FIntrepidNetworkGUID;
using repl::Value;
namespace schema = ::aoc::protocol::schema;

namespace {

Value object(const FIntrepidNetworkGUID& g) { return Value(g); }

bool observer_component(const repl::Subobject& from, const std::vector<std::string_view>& paths, const std::vector<std::string_view>& arrays, repl::Subobject& out) {
    out = repl::Subobject{};
    out.guid = from.guid;
    out.name = from.name;
    out.checksum = from.checksum;
    out.dynamic_class = from.dynamic_class;
    out.outer = from.outer;
    out.obj.schema = from.obj.schema;
    out.obj.opts = from.obj.opts;
    if (!from.obj.schema) return false;
    const schema::ClassSchema& cls = *from.obj.schema;
    for (const std::string_view path : paths) {
        const Value* v = from.obj.state.get(cls, path);
        if (v && !out.obj.state.set(cls, path, *v)) return false;
    }
    repl::FastArrays source = from.obj.arrays;
    for (const std::string_view name : arrays) {
        const repl::FastArrayState* src = source.get(cls, name);
        repl::FastArrayState* dst = out.obj.arrays.get(cls, name);
        if (!src || !dst) return false;
        for (const repl::FastArrayItem& it : src->items()) {
            if (!dst->insert(it.id, it.values)) return false;
        }
        dst->set_array_key(src->array_key());
    }
    return true;
}

}  // namespace

std::optional<repl::Actor> PlayerBootstrap::observer_pawn() const {
    if (!ok_ || !second_state_added_ || !pawn_.obj.schema) return std::nullopt;
    const SessionGuids& g = setup_.guids;
    repl::Actor a;
    a.guid = pawn_.guid;
    a.obj.schema = pawn_.obj.schema;
    a.obj.opts = pawn_.obj.opts;
    a.open = pawn_.open;
    bool ok = true;
    const auto set = [&](std::string_view path, Value v) { ok = a.obj.state.set(*a.obj.schema, path, std::move(v)) && ok; };
    set("AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated));
    set("RemoteRole", Value::u64(1));
    set("Role", Value::u64(4));
    set("Instigator", object(g.pawn));
    // Simulated proxies omit PlayerState so login ?Name= text (which names the player's PC) never reaches other clients.
    set("ReplicatedMovementMode", Value::u64(repl::kMoveWalking));
    struct Want {
        const FIntrepidNetworkGUID*            guid;
        std::initializer_list<std::string_view> paths, arrays;
    };
    const Want wants[] = {
        {&g.alignment, {"Alignments"}, {}},
        {&g.base_character_info,
         {"CharacterGuid.A", "CharacterGuid.B", "CharacterGuid.C", "CharacterGuid.D", "CharacterName", "Race", "Gender", "Level", "ServerId", "bHasBeenPossessed"},
         {"HeldItems"}},
        {&g.combat_info, {"bIsDead", "PrimaryClassId.Guid"}, {}},
        {&g.stats, {}, {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}},
        {&g.appearance, {"SharedAppearanceInfoId.Guid", "CharacterCustomization", "AppearanceIDs"}, {}},
    };
    for (const Want& w : wants) {
        const repl::Subobject* from = pawn_.find(*w.guid);
        if (!from) return std::nullopt;
        repl::Subobject sub;
        if (!observer_component(*from, w.paths, w.arrays, sub)) return std::nullopt;
        a.subobjects.push_back(std::move(sub));
    }
    if (!ok) return std::nullopt;
    return a;
}

}  // namespace aoc::worldd::bootstrap
