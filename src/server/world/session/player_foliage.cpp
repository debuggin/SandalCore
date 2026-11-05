// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "bootstrap/constants.h"
#include "content/content_db.h"
#include "foliage/foliage.h"
#include "gather/rules.h"
#include "protocol/repl/fname.h"
#include "session/player_session.h"
#include "world/interaction_state.h"
namespace aoc {
namespace worldd {
namespace repl = ::aoc::protocol::repl;
namespace {
constexpr std::chrono::milliseconds kFoliageDataDelay{116};
constexpr std::chrono::milliseconds kMovingWindow{250};
constexpr double kChannelDriftCm = 60.0;
constexpr double kChannelSlackCm = 100.0;
constexpr int32_t kInterruptCalloutType = 4;
constexpr int32_t kCalloutPointSize = 32;
constexpr uint64_t kInterruptChatCategory = 4;
constexpr std::chrono::seconds kCalloutLife{3};
struct FoliageRpcParams {
    bool decoded = false;
    uint32_t index = 0;
    std::string owner;
    std::string component;
    int state = -1;
    const char *state_name = "?";
    std::string what;
};
const repl::Value *find_foliage_rpc_value(const repl::ClientRpc &rpc, const char *path) {
    if (!rpc.call.rpc || !rpc.call.decoded)
        return nullptr;
    const auto *cmd = repl::rpc_command(*rpc.call.rpc, path);
    if (!cmd)
        return nullptr;
    const auto it = rpc.call.params.values.find(cmd->handle);
    return it == rpc.call.params.values.end() ? nullptr : &it->second;
}
template <typename T> const T *find_foliage_rpc_typed(const repl::ClientRpc &rpc, const char *path) {
    const repl::Value *v = find_foliage_rpc_value(rpc, path);
    return v ? v->get_if<T>() : nullptr;
}
FoliageRpcParams parse_foliage_rpc_params(const repl::ClientRpc &rpc) {
    FoliageRpcParams out;
    out.decoded = rpc.call.rpc && rpc.call.decoded;
    if (const auto *iv = find_foliage_rpc_typed<int64_t>(rpc, "OriginalInstanceIndex"))
        out.index = static_cast<uint32_t>(*iv);
    if (const auto *ov = find_foliage_rpc_typed<repl::NameValue>(rpc, "FoliageName"))
        out.owner = repl::fname_text(*ov);
    if (const auto *cv = find_foliage_rpc_typed<repl::NameValue>(rpc, "FoliageComponentUUID"))
        out.component = repl::fname_text(*cv);
    if (const auto *sv = find_foliage_rpc_typed<uint64_t>(rpc, "FoliageState"))
        out.state = static_cast<int>(*sv);
    if (out.state >= foliage::kInfancy && out.state <= foliage::kEmpty)
        out.state_name = foliage::kStateNames[out.state];
    out.what = out.owner + " / " + out.component + " #" + std::to_string(out.index) + " (" + out.state_name + ")";
    return out;
}
enum class FoliageInterrupt : uint8_t {
    None = 0,
    Movement = 1,
    Death = 2,
    Mounted = 3,
    Generic = 4,
};
struct FoliagePrecheck {
    bool ok = false;
    const char *kind = nullptr;
    std::string why;
    FoliageInterrupt interrupt = FoliageInterrupt::None;
    uint32_t row = 0;
    const foliage::DefInfo *def = nullptr;
    const content::GatherDefRow *interaction = nullptr;
    double distance = 0.0;
    gather::ToolInfo tool;
    const svc::InventoryItem *worn = nullptr;
};
FoliagePrecheck fail_foliage_check(const char *kind, std::string why, FoliageInterrupt interrupt = FoliageInterrupt::None) {
    FoliagePrecheck out;
    out.kind = kind;
    out.why = std::move(why);
    out.interrupt = interrupt;
    return out;
}
FoliagePrecheck resolve_foliage_instance(const foliage::Foliage &foliage_sys, const FoliageRpcParams &p) {
    const auto found = foliage_sys.map().find(p.owner, p.component, p.index);
    if (!found) {
        return fail_foliage_check("unknown instance", "the world has no such instance (not in "
                                                      "dist/content/region_foliage.tsv: a town's runtime tree, or a "
                                                      "name the map does not know)");
    }
    FoliagePrecheck out;
    out.row = *found;
    out.def = &foliage_sys.def_of(out.row);
    const int now_state = foliage_sys.store().state_of(out.row);
    if (p.state != now_state) {
        const char *world_state = (now_state >= 0 && now_state <= 3) ? foliage::kStateNames[now_state] : "?";
        return fail_foliage_check("wrong state", std::string("the client sees it ") + p.state_name + ", the world has it " + world_state);
    }
    out.interaction = (p.state >= foliage::kInfancy && p.state <= foliage::kAdult) ? out.def->interactions[p.state] : nullptr;
    if (!out.interaction)
        return fail_foliage_check("no interaction", out.def->name + " has no harvest in that state");
    out.ok = true;
    return out;
}
struct ActiveFoliageSpec {
    uint32_t row = 0;
    int state = 0;
    uint32_t index = 0;
    std::string_view owner;
    std::string_view component;
    std::string_view tool_guid;
    int64_t tool_record = 0;
    double seconds = 0.0;
    double start_x = 0.0;
    double start_y = 0.0;
};
struct FoliageChannelCheck {
    bool cancel = false;
    std::string why;
    FoliageInterrupt interrupt = FoliageInterrupt::None;
};
FoliageChannelCheck check_active_foliage_channel(const foliage::Foliage &f, const ActiveFoliageSpec &run, const svc::InventoryItem *tool,
                                                 const GoodPosition &at, std::pair<bool, bool> dead_or_mounted) {
    if (dead_or_mounted.first)
        return {true, "the player is dead", FoliageInterrupt::Death};
    if (dead_or_mounted.second)
        return {true, "the player is on a mount", FoliageInterrupt::Mounted};
    if (!tool || tool->instance_guid != run.tool_guid) {
        return {true, tool ? "the tool was swapped for another" : "the tool was taken off"};
    }
    if (!(tool->durability > 0))
        return {true, "the tool broke"};
    const foliage::FoliageMap::Row &r = f.map().rows()[run.row];
    const double distance = std::hypot(static_cast<double>(r.x) - at.x, static_cast<double>(r.y) - at.y);
    const double reach = f.reach_cm(run.row, run.state);
    if (distance > reach + kChannelSlackCm) {
        return {true,
                "the player moved away (" + std::to_string(static_cast<int>(distance)) + " cm from it, the interaction reaches " +
                    std::to_string(static_cast<int>(reach)) + " cm)",
                FoliageInterrupt::Movement};
    }
    const double drift = std::hypot(at.x - run.start_x, at.y - run.start_y);
    if (drift > kChannelDriftCm) {
        return {true,
                "the player moved " + std::to_string(static_cast<int>(std::lround(drift))) +
                    " cm since the channel began (a cutting needs standing still; "
                    "at most " +
                    std::to_string(static_cast<int>(kChannelDriftCm)) + " cm)",
                FoliageInterrupt::Movement};
    }
    return {};
}
bootstrap::PlayerBootstrap::FoliageChannel build_foliage_channel_wire(const ActiveFoliageSpec &run, double world_time, double session_sec) {
    bootstrap::PlayerBootstrap::FoliageChannel d;
    d.duration = static_cast<float>(run.seconds);
    d.start_time = bootstrap::effect_clock(world_time + session_sec);
    d.instance = run.index;
    d.owner = std::string(run.owner);
    d.component = std::string(run.component);
    d.interaction_index = 0;
    d.state = static_cast<uint8_t>(run.state);
    d.tool_record = run.tool_record;
    if (const auto guid = items::guid_of(std::string(run.tool_guid))) {
        d.tool_guid = {static_cast<uint32_t>(guid->a), static_cast<uint32_t>(guid->b), static_cast<uint32_t>(guid->c), static_cast<uint32_t>(guid->d)};
    }
    return d;
}
struct FoliagePrecheckInput {
    foliage::Foliage *f = nullptr;
    gather::Gathering *g = nullptr;
    bool dead = false;
    bool mounted = false;
    bool active_channel = false;
    Clock::time_point moved_at{};
    GoodPosition at{};
    uint64_t player_id = 0;
    const items::Inventory *inventory = nullptr;
    const professions::Professions *professions = nullptr;
    bool in_combat = false;
    std::function<std::unique_ptr<stats::CharacterStats::Evaluation>(std::string &)> eval_fn;
};
FoliagePrecheck check_foliage_range_and_lock(const FoliagePrecheckInput &in, const FoliageRpcParams &p, Clock::time_point now, FoliagePrecheck out) {
    if (in.moved_at != Clock::time_point{} && now - in.moved_at < kMovingWindow) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - in.moved_at).count();
        return fail_foliage_check("moving",
                                  "the player moved " + std::to_string(elapsed_ms) + " ms ago (a cutting needs standing still: at least " +
                                      std::to_string(kMovingWindow.count()) + " ms)",
                                  FoliageInterrupt::Movement);
    }
    const foliage::FoliageMap::Row &r = in.f->map().rows()[out.row];
    out.distance = std::hypot(static_cast<double>(r.x) - in.at.x, static_cast<double>(r.y) - in.at.y);
    const double reach = in.f->reach_cm(out.row, p.state);
    if (out.distance > reach) {
        return fail_foliage_check("out of range", std::to_string(static_cast<int>(out.distance)) + " cm away, the interaction reaches " +
                                                      std::to_string(static_cast<int>(reach)) + " cm");
    }
    if (const uint64_t holder = in.f->store().locked_by(out.row); holder != 0 && holder != in.player_id)
        return fail_foliage_check("instance busy", "another player is cutting it");
    out.worn = in.inventory->at(svc::ItemContainer::Equipment, out.interaction->tool_slot);
    if (out.worn) {
        out.tool.worn = true;
        out.tool.tool = in.g->tool(out.worn->item_record_id);
        out.tool.durability = out.worn->durability;
    }
    const gather::Verdict verdict = gather::check_requirements(*out.interaction, *in.professions, out.tool, in.in_combat);
    return verdict.ok() ? out : fail_foliage_check(gather::refusal_name(verdict.why), verdict.text);
}
FoliagePrecheck check_foliage_preconditions(const FoliagePrecheckInput &in, const FoliageRpcParams &p, Clock::time_point now) {
    if (in.dead)
        return fail_foliage_check("dead", "the player is dead");
    if (in.mounted)
        return fail_foliage_check("mounted", "the player is on a mount");
    if (in.active_channel)
        return fail_foliage_check("already channeling", "this player is harvesting something already");
    FoliagePrecheck out = resolve_foliage_instance(*in.f, p);
    if (!out.ok)
        return out;
    return check_foliage_range_and_lock(in, p, now, std::move(out));
}
struct FoliageStartAttempt {
    FoliagePrecheck pre;
    double seconds = 0.0;
};
FoliageStartAttempt try_start_foliage_channel(const FoliagePrecheckInput &in, const FoliageRpcParams &p, Clock::time_point now) {
    FoliageStartAttempt out;
    out.pre = check_foliage_preconditions(in, p, now);
    if (!out.pre.ok)
        return out;
    std::string why;
    auto run = in.eval_fn(why);
    if (!run) {
        out.pre = fail_foliage_check("no stats", why);
        return out;
    }
    const auto seconds = in.g->channel_seconds(*out.pre.interaction, *run, why);
    if (!seconds) {
        out.pre = fail_foliage_check("no duration", why);
        return out;
    }
    if (!in.f->store().lock(out.pre.row, in.player_id)) {
        out.pre = fail_foliage_check("instance busy", "another player is cutting it");
        return out;
    }
    out.seconds = *seconds;
    return out;
}
std::string format_foliage_start_log(const foliage::Foliage &f, const FoliageRpcParams &p, const FoliagePrecheck &pre, const professions::Professions &profs,
                                     double seconds) {
    const professions::Profession *pf = profs.valid() ? profs.book()->find(pre.interaction->profession_guid) : nullptr;
    const std::string tier_name = profs.valid() ? profs.book()->tier(pre.interaction->cert_tier).name : std::string("?");
    return fmt::format("starts cutting {} ({}): a {} {} {} with {}, a channel of {:.2f} s (base {:.2f} s), "
                       "{:.0f} cm from it; the instance is whole again {:.0f} s after the harvest",
                       pre.def->name, p.what, p.state_name, tier_name, pf ? pf->name : std::string("?"),
                       pre.tool.tool ? pre.tool.tool->item_name : std::string("?"), seconds, pre.interaction->base_duration, pre.distance,
                       f.store().regrow_seconds(pre.row));
}
struct FoliageRollInput {
    foliage::Foliage *f = nullptr;
    gather::Gathering *g = nullptr;
    const PlayerWorld *world = nullptr;
    uint32_t row = 0;
    const content::GatherDefRow *interaction = nullptr;
    int player_level = 1;
    std::function<std::unique_ptr<stats::CharacterStats::Evaluation>(std::string &)> eval_fn;
};
std::optional<gather::Harvest> roll_completed_foliage(const FoliageRollInput &in, std::string &cancel_why) {
    std::string why;
    auto run = in.eval_fn(why);
    if (!run) {
        cancel_why = "no stat run for the rolls: " + why;
        return std::nullopt;
    }
    const foliage::FoliageMap::Row &r = in.f->map().rows()[in.row];
    const world::CityNodes *nodes = in.world->city_nodes();
    const world::NodeSite site = nodes ? nodes->node_at(r.x, r.y) : world::NodeSite{};
    auto harvest = in.g->harvest(*in.interaction, *run, in.player_level, site, nodes, in.g->rng(), why);
    if (!harvest)
        cancel_why = "the rolls do not run: " + why;
    return harvest;
}
} // namespace
const char *PlayerSession::interrupt_key(Interrupt k) {
    switch (k) {
    case Interrupt::Movement:
        return "GatheringInterruption_Movement";
    case Interrupt::Death:
        return "GatheringInterruption_Death";
    case Interrupt::Mounted:
        return "GatheringInterruption_Mounted";
    case Interrupt::Generic:
        return "GatheringInterruption_Generic";
    case Interrupt::None:
        break;
    }
    return "";
}
void PlayerSession::send_foliage_data(Connection &conn, const bootstrap::PlayerBootstrap::FoliageChannel &data) {
    queue(
        "the pawn's foliage data", [data](bootstrap::PlayerBootstrap &pb) { return pb.pawn_foliage_data(data); }, conn,
        /*must_build=*/true);
}
void PlayerSession::send_interrupt_callout(Connection &conn, Interrupt key, Clock::time_point now) {
    if (key == Interrupt::None)
        return;
    send_economy_callout(conn, interrupt_key(key), now);
}
void PlayerSession::send_economy_callout(Connection &conn, const std::string &message_key, Clock::time_point now) {
    if (!room_for(2))
        return;
    const int64_t unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t ticks = items::ticks_of_ms(unix_ms);
    const uint64_t callout_key = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
    const std::string text = message_key;
    queue(
        "a callout",
        [ticks, callout_key, text](bootstrap::PlayerBootstrap &pb) {
            return pb.callout_add("", kInterruptCalloutType, kCalloutPointSize, ticks, callout_key, "Economy", text, kInterruptChatCategory);
        },
        conn);
    gather_callouts_.push_back({now + kCalloutLife, callout_key});
}
void PlayerSession::unlock_foliage_client(Connection &conn, uint32_t index, const std::string &owner, const std::string &component) {
    // WHY: Client sets its own FoliageData on ServerStartInteractionWithFoliage
    // and blocks further interacts until ClientInterruptInteractionWithFoliage
    // clears it.
    if (!room_for(2))
        return;
    queue(
        "ClientInterruptInteractionWithFoliage",
        [index, owner, component](bootstrap::PlayerBootstrap &pb) { return pb.foliage_interrupt(index, owner, component); }, conn);
    ++foliage_interrupts_sent_;
}
void PlayerSession::on_foliage_interact(Connection &conn, const repl::ClientRpc &rpc, Clock::time_point now) {
    ++foliage_interacts_;
    if (!within_budget(foliage_budget_, "ServerStartInteractionWithFoliage", now)) {
        ++foliage_refused_;
        ++foliage_refusals_["over the request budget"];
        return;
    }
    foliage::Foliage *f = npc_world_ ? npc_world_->foliage() : nullptr;
    gather::Gathering *g = npc_world_ ? npc_world_->gathering() : nullptr;
    const FoliageRpcParams p = parse_foliage_rpc_params(rpc);
    if (!f || !g) {
        if (!std::exchange(foliage_off_logged_, true)) {
            spdlog::info("[gather] {}: a tree or plant of the world's foliage was interacted with "
                         "(ServerStartInteractionWithFoliage), and foliage gathering is off in this world "
                         "(--no-gathering, or content.db has no foliage tables, or dist/content/region_foliage.tsv "
                         "did not load: see the log of the start); further foliage interactions are not logged",
                         name());
        }
        ++foliage_refused_;
        ++foliage_refusals_["foliage off"];
        return unlock_foliage_client(conn, p.index, p.owner, p.component);
    }
    if (!p.decoded) {
        ++foliage_refused_;
        ++foliage_refusals_["unreadable"];
        return unlock_foliage_client(conn, 0, std::string(), std::string());
    }
    const GoodPosition at = checker_.good();
    const FoliageStartAttempt attempt = try_start_foliage_channel({f, g, dead_, mounted(), foliage_.row != kNoInstance || gather_.node != 0, moved_at_, at,
                                                                   static_cast<uint64_t>(pb_.setup().player_id), &inventory_, &professions_,
                                                                   stats_ && stats_->in_combat(), [this](std::string &w) { return gather_run(w); }},
                                                                  p, now);
    if (!attempt.pre.ok) {
        ++foliage_refused_;
        ++foliage_refusals_[attempt.pre.kind];
        spdlog::log(level_for(foliage_refused_logged_, 12), "[foliage] {}: the cutting of {} is refused ({}): {}", name(), p.what, attempt.pre.kind,
                    attempt.pre.why);
        if (const auto intr = static_cast<Interrupt>(attempt.pre.interrupt); intr != Interrupt::None && room_for(4))
            send_interrupt_callout(conn, intr, now);
        if (foliage_.row == kNoInstance)
            unlock_foliage_client(conn, p.index, p.owner, p.component);
        return;
    }
    const Clock::time_point data_at = now + kFoliageDataDelay;
    const Clock::time_point completes = data_at + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(attempt.seconds));
    foliage_ = {attempt.pre.row, p.state, p.index, p.owner, p.component, attempt.pre.interaction, attempt.pre.def->name,
                attempt.pre.interaction->tool_slot, attempt.pre.worn->instance_guid, attempt.pre.worn->item_record_id,
                now, data_at, completes, attempt.seconds, at.x, at.y, false};
    ++foliage_started_;
    spdlog::log(level_for(foliage_logged_, 20), "[foliage] {}: {}", name(), format_foliage_start_log(*f, p, attempt.pre, professions_, attempt.seconds));
}
void PlayerSession::run_foliage(Connection &conn, Clock::time_point now) {
    foliage::Foliage *f = npc_world_ ? npc_world_->foliage() : nullptr;
    if (f)
        f->tick(now);
    if (foliage_.row == kNoInstance)
        return;
    if (!f)
        return cancel_foliage(&conn, "foliage gathering went away");
    const svc::InventoryItem *tool = inventory_.at(svc::ItemContainer::Equipment, foliage_.tool_slot);
    const ActiveFoliageSpec active{foliage_.row,       foliage_.state,       foliage_.index,   foliage_.owner,   foliage_.component,
                                   foliage_.tool_guid, foliage_.tool_record, foliage_.seconds, foliage_.start_x, foliage_.start_y};
    const FoliageChannelCheck check = check_active_foliage_channel(*f, active, tool, checker_.good(), {dead_, mounted()});
    if (check.cancel)
        return cancel_foliage(&conn, check.why, static_cast<Interrupt>(check.interrupt));
    if (!foliage_.data_sent) {
        if (now < foliage_.data_at || !room_for(2))
            return;
        send_foliage_data(conn, build_foliage_channel_wire(active, pb_.setup().world_time, session_seconds(foliage_.data_at)));
        foliage_.data_sent = true;
    }
    if (now >= foliage_.completes)
        complete_foliage(conn, now);
}
bool PlayerSession::complete_foliage(Connection &conn, Clock::time_point now) {
    foliage::Foliage *f = npc_world_ ? npc_world_->foliage() : nullptr;
    gather::Gathering *g = npc_world_ ? npc_world_->gathering() : nullptr;
    const content::ContentDb *db = world_ ? world_->content() : nullptr;
    if (!f || !g || !db || !foliage_.interaction) {
        cancel_foliage(&conn, "foliage gathering went away");
        return true;
    }
    if (!room_for(16 + (world_ ? world_->auto_grants().size() : 0)))
        return false;
    std::string why;
    const auto harvest =
        roll_completed_foliage({f, g, world_, foliage_.row, foliage_.interaction, progress_.level, [this](std::string &w) { return gather_run(w); }}, why);
    if (!harvest) {
        cancel_foliage(&conn, why);
        return true;
    }
    const uint32_t row = foliage_.row;
    HarvestFrame frame;
    frame.tag = "foliage";
    frame.what = foliage_.name + " (" + foliage_.owner + " / " + foliage_.component + " #" + std::to_string(foliage_.index) + ")";
    frame.profession = foliage_.interaction->profession_guid;
    frame.harvest = &*harvest;
    frame.tool_guid = foliage_.tool_guid;
    frame.tool_slot = foliage_.tool_slot;
    frame.after_items = [&conn, this] {
        queue(
            "ClientConfirmMultiInteractFoliage", [](bootstrap::PlayerBootstrap &pb) { return pb.foliage_confirm(); }, conn,
            /*must_build=*/true);
    };
    frame.after_experience = [&conn, f, row, now, this] {
        f->harvested(row, now);
        send_foliage_data(conn, bootstrap::PlayerBootstrap::FoliageChannel{});
    };
    frame.log_tail = fmt::format("; the instance is whole again in {:.0f} s", f->store().regrow_seconds(row));
    f->store().unlock(row, static_cast<uint64_t>(pb_.setup().player_id));
    foliage_ = FoliageRun{};
    ++foliage_completed_;
    deliver_harvest(conn, now, frame);
    return true;
}
void PlayerSession::cancel_foliage(Connection *conn, const std::string &why, Interrupt interrupt) {
    if (foliage_.row == kNoInstance)
        return;
    foliage::Foliage *f = npc_world_ ? npc_world_->foliage() : nullptr;
    const FoliageRun run = foliage_;
    foliage_ = FoliageRun{};
    if (f)
        f->store().unlock(run.row, static_cast<uint64_t>(pb_.setup().player_id));
    ++foliage_cancelled_;
    spdlog::log(level_for(foliage_logged_, 20), "[foliage] {}: the cutting of {} ({} / {} #{}) ends with no reward: {}", name(), run.name, run.owner,
                run.component, run.index, why);
    if (!conn)
        return;
    if (run.data_sent)
        send_foliage_data(*conn, bootstrap::PlayerBootstrap::FoliageChannel{});
    if (interrupt != Interrupt::None && room_for(4))
        send_interrupt_callout(*conn, interrupt, last_now_);
    if (interrupt != Interrupt::None || !run.data_sent)
        unlock_foliage_client(*conn, run.index, run.owner, run.component);
}
void PlayerSession::on_foliage_start(Connection &conn, const repl::ClientRpc &rpc, Clock::time_point now) {
    if (!rpc.call.decoded && ++foliage_unreadable_logged_ <= kLoggedPerKind) {
        spdlog::warn("[session] {}: ServerStartInteractionWithFoliage arrived and its parameters do not decode: no "
                     "cutting starts, and the client is told to end its own (ClientInterruptInteractionWithFoliage)",
                     name());
    }
    on_foliage_interact(conn, rpc, now);
}
void PlayerSession::end_gathering(const char *why) {
    cancel_gather(nullptr, why);
    while (!gather_piles_.empty()) {
        GatherPile pile = std::move(gather_piles_.front());
        gather_piles_.pop_front();
        make_gather_pile(pile);
    }
}
void PlayerSession::register_foliage_rpcs(RpcTable &t) {
    t.add(RpcScope::PlayerController, "ServerStartInteractionWithFoliage",
          [](PlayerSession &s, const RpcContext &c) { s.on_foliage_start(c.conn, c.rpc, c.now); });
}
} // namespace worldd
} // namespace aoc
