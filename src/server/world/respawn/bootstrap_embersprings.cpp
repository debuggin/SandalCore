// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "bootstrap/bootstrap.h"

#include <utility>
#include <vector>

namespace aoc::worldd::bootstrap {

using protocol::emit::FIntrepidNetworkGUID;
using repl::Value;
namespace schema = ::aoc::protocol::schema;

namespace {

repl::ArrayValue pack_attuned_blocks(const std::vector<std::vector<uint8_t>>& blocks) {
    repl::ArrayValue list;
    for (const std::vector<uint8_t>& block : blocks) {
        repl::ArrayValue inner;
        for (const uint8_t b : block) {
            repl::ElementState e;
            e.values[1] = Value::u64(b);
            inner.elements.push_back(std::move(e));
        }
        repl::ElementState outer;
        outer.values[1] = Value(std::move(inner));
        list.elements.push_back(std::move(outer));
    }
    return list;
}

}  // namespace

bool PlayerBootstrap::set_attunements(const std::vector<std::vector<uint8_t>>& blocks, int home, double home_last_used) {
    if (!ok_ || home < 0 || home > 255) return false;
    repl::Subobject* base = component(setup_.guids.base_character_info);
    if (!base) return false;
    const schema::ClassSchema& s = *base->obj.schema;
    if (!s.command("WorldAttunements.AttunedBlocks") || !s.command("HomeAttunement.AttunedIndex") || !s.command("HomeAttunementLastUsed.TimeStamp")) return false;
    if (!base->obj.state.set(s, "WorldAttunements.AttunedBlocks", Value(pack_attuned_blocks(blocks)))) return false;
    if (home != 0 || home_in_state_) {
        if (!base->obj.state.set(s, "HomeAttunement.AttunedIndex", Value::i64(home))) return false;
        home_in_state_ = true;
    }
    if (home_last_used != 0.0 || home_used_in_state_) {
        if (!base->obj.state.set(s, "HomeAttunementLastUsed.TimeStamp", Value::f64(home_last_used))) return false;
        home_used_in_state_ = true;
    }
    return true;
}

std::optional<Send> PlayerBootstrap::attune_prompt(int64_t place) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pawn_.obj.schema->rpc("SendClientPromptForHomeAttunement");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "AttuneRecord", Value::i64(place))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "SendClientPromptForHomeAttunement";
    call.params = std::move(params);
    return rpcs(Role::Pawn, "SendClientPromptForHomeAttunement", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::home_attuned(const emit::FIntrepidNetworkGUID& shrine, int64_t place, bool result) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* notify = pawn_.obj.schema->rpc("ClientNotifyHomeAttune");
    const schema::RpcSchema* done = pawn_.obj.schema->rpc("SendClientHomeAttuneResult");
    if (!notify || !done) return std::nullopt;
    std::vector<repl::RpcRequest> calls;
    if (result) {
        repl::RpcRequest n;
        n.rpc = "ClientNotifyHomeAttune";
        if (!repl::set_rpc_param(*notify, n.params, "Spawner", Value(shrine))) return std::nullopt;
        calls.push_back(std::move(n));
    }
    repl::RpcRequest r;
    r.rpc = "SendClientHomeAttuneResult";
    if (!repl::set_rpc_param(*done, r.params, "AttuneRecord", Value::i64(place)) || !repl::set_rpc_param(*done, r.params, "bResult", Value::boolean(result))) return std::nullopt;
    calls.push_back(std::move(r));
    return rpcs(Role::Pawn, "ClientNotifyHomeAttune and SendClientHomeAttuneResult", std::span<const repl::RpcRequest>(calls.data(), calls.size()));
}

std::optional<Send> PlayerBootstrap::chat_line(const std::string& text, int category) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveChatMessages");
    if (!rpc) return std::nullopt;
    repl::ArrayValue list;
    repl::ElementState e;
    e.values[1] = Value::str(text);
    e.values[2] = Value::str(std::string());
    e.values[3] = Value::u64(static_cast<uint64_t>(category));
    e.values[4] = Value::str(std::string());
    e.values[5] = Value::boolean(false);
    e.values[6] = Value(repl::ItemInstance{});
    list.elements.push_back(std::move(e));
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "Messages", Value(std::move(list)))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveChatMessages";
    call.params = std::move(params);
    return rpcs(Role::PlayerController, "ClientReceiveChatMessages", std::span<const repl::RpcRequest>(&call, 1));
}

}  // namespace aoc::worldd::bootstrap
