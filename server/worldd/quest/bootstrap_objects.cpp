// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "bootstrap/bootstrap.h"

#include <utility>

namespace aoc::worldd::bootstrap {

using repl::Value;
namespace schema = ::aoc::protocol::schema;

std::optional<Send> PlayerBootstrap::interact_done(const emit::FIntrepidNetworkGUID& component, const emit::FIntrepidNetworkGUID& actor) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pawn_.obj.schema->rpc("ClientOnDoInteract");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "Source", Value(component)) || !repl::set_rpc_param(*rpc, params, "InInteractable", Value(actor))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientOnDoInteract";
    call.params = std::move(params);
    return rpcs(Role::Pawn, "ClientOnDoInteract", std::span<const repl::RpcRequest>(&call, 1));
}

}  // namespace aoc::worldd::bootstrap
