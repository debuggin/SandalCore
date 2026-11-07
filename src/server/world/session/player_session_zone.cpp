#include "protocol/repl/move_packed.h"
#include "protocol/repl/rpc_fields.h"
#include "session/gm_travel.h"
#include "session/player_session.h"
#include "world/city_nodes.h"

namespace aoc {
namespace worldd {

namespace {

const repl::Value* rpc_value(const repl::ClientRpc& rpc, std::string_view name) {
    if (!rpc.call.decoded || !rpc.call.rpc) return nullptr;
    const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, name);
    if (!c) return nullptr;
    const auto it = rpc.call.params.values.find(c->handle);
    return it == rpc.call.params.values.end() ? nullptr : &it->second;
}

double number_value(const repl::Value* v) {
    if (!v) return 0.0;
    if (const auto* i = v->get_if<int64_t>()) return static_cast<double>(*i);
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<double>(*u);
    if (const auto* d = v->get_if<double>()) return *d;
    if (const auto* f = v->get_if<float>()) return static_cast<double>(*f);
    return 0.0;
}

std::string text_value(const repl::Value* v) {
    if (!v) return {};
    if (const auto* s = v->get_if<std::string>()) return *s;
    return {};
}

const char* gm_refusal(PlayerSession::GmMoveResult::Status s) {
    switch (s) {
        case PlayerSession::GmMoveResult::Status::Dead: return "the player is dead";
        case PlayerSession::GmMoveResult::Status::OutsideWorld: return "outside the world";
        case PlayerSession::GmMoveResult::Status::Busy: return "the reply queue is full";
        case PlayerSession::GmMoveResult::Status::Moved: break;
    }
    return "moved";
}

}  // namespace

struct PlayerSession::SessionInternal {
    static void handle_gm_node_clock_rpc(PlayerSession& s, const repl::ClientRpc& rpc, const char* params, Clock::time_point now);
};

PlayerSession::GmMoveResult PlayerSession::gm_teleport(Connection& conn, double x, double y, double z, Clock::time_point now, const std::string& by) {
    return move_to(conn, x, y, z, now, by, /*gm=*/true);
}

PlayerSession::GmMoveResult PlayerSession::move_to(Connection& conn, double x, double y, double z, Clock::time_point now, const std::string& by, bool gm) {
    GmMoveResult r;
    r.from = last_position();
    r.to = r.from;
    if (dead_) {
        r.status = GmMoveResult::Status::Dead;
        return r;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !base_limits_.world.contains(x, y, z)) {
        r.status = GmMoveResult::Status::OutsideWorld;
        return r;
    }
    if (!room_for(mounted() ? 6 : 2)) {
        r.status = GmMoveResult::Status::Busy;
        return r;
    }
    const double yaw = checker_.good().yaw_degrees;
    const double t = session_seconds(now);
    if (mounted() && ride_) {
        ride_->reposition(x, y, z, yaw, t);
        if (mounts_) mounts_->reposition_active(x, y, z);
        checker_.teleport(x, y, z, yaw, now);
        checker_.rebase(ride_->limits(base_limits_, t), checker_.good());
        r.dismounted = false;
    } else {
        checker_.teleport(x, y, z, yaw, now);
    }
    pending_fix_.reset();   // a correction a refused move left would put the client back where it was
    based_fix_.reset();
    const float ts = checker_.good().has_time_stamp ? checker_.good().time_stamp : 0.0f;
    constexpr uint8_t mode = repl::kMoveFalling;   // the client lands and walks again by itself
    queue(gm ? "GM teleport" : "floor net teleport", [ts, x, y, z, mode](bootstrap::PlayerBootstrap& pb) { return pb.correction(ts, repl::Vec3d{x, y, z}, repl::Vec3d{}, mode); }, conn, /*must_build=*/true);
    if (gm) ++gm_teleports_;
    if (npc_world_) {
        const uint64_t pid = static_cast<uint64_t>(pb_.setup().player_id);
        std::ignore = npc_world_->step_player_border_handover(pid, x, y);
        if (world_pawn_ != 0 && world_pawn_ != pid) {
            std::ignore = npc_world_->step_player_border_handover(world_pawn_, x, y);
        }
    }
    r.to = checker_.good();
    spdlog::info("[{}] {}: teleported by {} from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}){}", gm ? "gm" : "floor-net", name(), by, r.from.x, r.from.y, r.from.z, x, y, z,
                 r.dismounted ? "; dismounted first" : "");
    if (position_changed(/*with_yaw=*/false)) hand_over(gm ? "a GM teleport" : "the floor net", /*periodic=*/false);   // (a crash must not take the player back)
    return r;
}

void PlayerSession::run_floor_net(Connection& conn, Clock::time_point now) {
    if (!phase_in_done_ || dead_ || finished_ || failed_ || !npc_world_) return;   // (before the phase-in the pawn is held at the join height and cannot fall)
    const GoodPosition g = checker_.good();
    const auto rescue = npc_world_->floor_rescue(g.x, g.y, g.z);
    if (!rescue) {
        net_since_.reset();
        return;
    }
    if (!net_since_) net_since_ = now;
    if (std::chrono::duration<double>(now - *net_since_).count() < kNetHoldSeconds) return;
    if (net_last_valid_ && std::chrono::duration<double>(now - net_last_).count() < kNetRepeatSeconds) return;
    const GmMoveResult r = move_to(conn, rescue->x, rescue->y, rescue->z, now, "the floor net", /*gm=*/false);
    if (r.status == GmMoveResult::Status::Busy) return;   // (the reply queue is full: the next tick tries again)
    net_last_ = now;
    net_last_valid_ = true;
    net_since_.reset();
    if (r.status != GmMoveResult::Status::Moved) {
        spdlog::warn("[floor-net] {}: at ({:.0f}, {:.0f}, {:.0f}) cm, under the floor, and the move back is refused ({})", name(), g.x, g.y, g.z, gm_refusal(r.status));
        return;
    }
    ++net_rescues_;
    spdlog::warn("[floor-net] {}: its last good position ({:.0f}, {:.0f}, {:.0f}) cm is {:.0f} m under {}: put back at ({:.0f}, {:.0f}, {:.0f}) cm (rescue {} of this session)", name(), g.x, g.y, g.z,
                 (rescue->z - g.z) / 100.0, rescue->where, rescue->x, rescue->y, rescue->z, net_rescues_);
}

double PlayerSession::gm_limit_scale(Clock::time_point now) const {
    double m = gm_speed_;
    if (now < gm_grace_until_) m = std::max(m, gm_grace_speed_);   // the client learns of a lower speed a trip later
    return std::max(1.0, m);
}

void PlayerSession::scale_pawn_limits(Clock::time_point now) {
    const double s = gm_limit_scale(now);
    checker_.set_speed_limits(base_limits_.max_speed * s, base_limits_.dash_speed * s);
}

void PlayerSession::apply_gm_speed(Connection* conn, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);   // (first: it starts the session's ability clock)
    const double t = ability_seconds(now);
    const stats::StatValue* engine = stats_ ? stats_->find(spawns::kStatMoveSpeedMult) : nullptr;
    const double base = engine ? static_cast<double>(engine->value) : 1.0;
    if (!pb_.has_stat_item(spawns::kStatMoveSpeedMult)) {
        spdlog::warn("[gm] {}: the pawn has no MoveSpeedMult stat to carry the run speed: the move checker's limits scale, the client does not move faster", name());
    } else {
        bootstrap::AbilityChange change;
        if (runtime && runtime->tracks(spawns::kStatMoveSpeedMult)) {
            runtime->gear_stat(change, spawns::kStatMoveSpeedMult, base * gm_speed_);
        } else {
            bootstrap::StatChange sc;
            sc.record = spawns::kStatMoveSpeedMult;
            sc.value = static_cast<float>(base * gm_speed_);
            change.stats.push_back(sc);
        }
        if (!change.empty()) {
            if (pb_.apply_ability_change(change)) {
                abilities_dirty_ = true;
            } else {
                spdlog::warn("[gm] {}: the pawn does not take the run speed ({})", name(), pb_.error().empty() ? std::string("its stat has no item") : pb_.error());
            }
        }
    }
    if (ride_) ride_->set_gm_multiplier(gm_speed_, t);
    if (!mounted()) scale_pawn_limits(now);
    if (conn && abilities_dirty_) flush_abilities(*conn);
}

PlayerSession::GmSpeedChange PlayerSession::gm_set_speed(Connection& conn, double multiplier, Clock::time_point now, const std::string& by) {
    GmSpeedChange r;
    r.previous = gm_speed_;
    const double m = std::isfinite(multiplier) ? std::clamp(multiplier, 0.5, 10.0) : 1.0;
    if (m < gm_speed_) {
        gm_grace_speed_ = std::max(gm_speed_, now < gm_grace_until_ ? gm_grace_speed_ : 0.0);
        gm_grace_until_ = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(Ride::kSpeedGraceSeconds));
    }
    gm_speed_ = m;
    apply_gm_speed(&conn, now);
    r.now = gm_speed_;
    spdlog::info("[gm] {}: run speed x{:g} (was x{:g}) by {}{}", name(), gm_speed_, r.previous, by, mounted() ? "; riding: the mount's MoveSpeedMult follows" : "");
    return r;
}

bool PlayerSession::gm_rpc_admitted(const char* rpc_name, const std::string& params, Clock::time_point now) {
    ++gm_rpcs_;
    const bool on = world_ && world_->gm_commands();
    spdlog::log(level_for(gm_logged_, 100), "[gm] {}: the client sent {} ({}): {}", name(), rpc_name, log_text(params),
                on ? "answering" : "refused: GM commands are off (world --gm-commands, or AOC_GM_COMMANDS=1)");
    if (!on) return false;
    return gm_budget_.take(now);
}

void PlayerSession::on_gm_teleport_xyz(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const double x = number_value(rpc_value(rpc, "X")), y = number_value(rpc_value(rpc, "Y")), z = number_value(rpc_value(rpc, "Z"));
    const double radius = number_value(rpc_value(rpc, "OptionalRadius"));
    char params[160];
    std::snprintf(params, sizeof params, "X=%.0f Y=%.0f Z=%.0f OptionalRadius=%g%s", x, y, z, radius, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleportXYZ", params, now) || !rpc.call.decoded) return;
    const GmMoveResult r = gm_teleport(conn, x, y, z, now, "ServerTeleportXYZ");
    if (r.status == GmMoveResult::Status::Moved) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleportXYZ to ({:.0f}, {:.0f}, {:.0f}) is refused ({})", name(), x, y, z, gm_refusal(r.status));
}

void PlayerSession::on_gm_teleport_key(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string key = text_value(rpc_value(rpc, "DestinationLookupKey"));
    const double radius = number_value(rpc_value(rpc, "OptionalRadius"));
    char params[256];
    std::snprintf(params, sizeof params, "DestinationLookupKey=\"%.100s\" OptionalRadius=%g%s", key.c_str(), radius, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleport", params, now) || !rpc.call.decoded) return;
    const world::Places places = world_->places();
    const world::Place* place = places.find(key);
    if (!place) {
        spdlog::warn("[gm] {}: ServerTeleport \"{}\": world knows no such place (the city nodes and \"login\"; worldctl places lists them)", name(), log_text(key));
        return;
    }
    const GmMoveResult r = gm_teleport(conn, place->x, place->y, world_->place_height(*place), now, "ServerTeleport");
    if (r.status == GmMoveResult::Status::Moved) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleport \"{}\" is refused ({})", name(), log_text(key), gm_refusal(r.status));
}

void PlayerSession::on_gm_teleport_player(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string names = text_value(rpc_value(rpc, "CharacterNames"));
    const double x = number_value(rpc_value(rpc, "X")), y = number_value(rpc_value(rpc, "Y")), z = number_value(rpc_value(rpc, "Z"));
    char params[256];
    std::snprintf(params, sizeof params, "CharacterNames=\"%.100s\" X=%.0f Y=%.0f Z=%.0f%s", names.c_str(), x, y, z, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleportPlayer", params, now) || !rpc.call.decoded) return;
    size_t moved = 0;
    if (GmPort* port = world_ ? world_->gm_port() : nullptr) {
        moved = port->teleport_characters(names, x, y, z, "ServerTeleportPlayer by " + name(), now);
    } else if (iequals(names, name())) {   // (no port: a world with only this session)
        moved = gm_teleport(conn, x, y, z, now, "ServerTeleportPlayer").status == GmMoveResult::Status::Moved ? 1 : 0;
    }
    if (moved > 0) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleportPlayer \"{}\" moved no one", name(), log_text(names));
}

void PlayerSession::note_node_request(std::deque<int64_t>& wait, std::set<int64_t>& queued, int64_t node, const char* what) {
    if (node == 0) {
        spdlog::debug("[nodes] {}: {} for node 0: ignored", name(), what);
        return;
    }
    if (queued.count(node)) return;   // its answer is already waiting
    if (wait.size() >= kMaxNodeRequests) {
        ++node_requests_dropped_;
        if (!node_overflow_logged_) {
            node_overflow_logged_ = true;
            spdlog::warn("[nodes] {}: {} requests are waiting for their answers: further ones are dropped (said once)", name(), wait.size());
        }
        return;
    }
    queued.insert(node);
    wait.push_back(node);
}

void PlayerSession::on_node_summary_request(const repl::ClientRpc& rpc) {
    ++node_requests_;
    const auto node = repl::parse_node_summary_request(rpc.call);
    if (!node) {
        if (!node_unreadable_logged_) {
            node_unreadable_logged_ = true;
            spdlog::warn("[nodes] {}: a ServerRequestNodeSummary does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    note_node_request(node_summary_wait_, node_summary_queued_, *node, "ServerRequestNodeSummary");
}

void PlayerSession::on_siege_summary_request(const repl::ClientRpc& rpc) {
    ++node_requests_;
    const auto node = repl::parse_siege_summary_request(rpc.call);
    if (!node) {
        if (!node_unreadable_logged_) {
            node_unreadable_logged_ = true;
            spdlog::warn("[nodes] {}: a ServerRequestCurrentSiegeSummary does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    note_node_request(siege_wait_, siege_queued_, *node, "ServerRequestCurrentSiegeSummary");
}

void PlayerSession::on_siege_cooldowns_request() {
    ++node_requests_;
    cooldowns_pending_ = true;
}

void PlayerSession::on_citizenship_window_request() {
    ++citizenship_requests_;
    if (citizenship_logged_) return;
    citizenship_logged_ = true;
    spdlog::info("[nodes] {}: the client asks for the citizenship window's data (ServerSetEssentialCitizenshipWindowData): not answered, the struct's layout is not decoded yet "
                 "(the citizenship milestone); further ones are not logged", name());
}

void PlayerSession::run_nodes(Connection& conn) {
    if (finished_ || failed_ || !join_.empty()) return;   // (the join includes the pawn's second state: the client's world exists, as retail's answers assume)
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const world::NodeWire* wire = world_ ? world_->node_wire() : nullptr;
    const auto has_room = [this] { return !failed_ && replies_.size() + delayed_.size() + 8 <= kMaxPendingReplies; };

    if (pb_.mode() == bootstrap::GameStateMode::Tier3 && nodes && wire && nodes->version() != node_maps_version_ && has_room()) {
        node_maps_version_ = nodes->version();
        auto maps = std::make_shared<protocol::repl::NodeStateMaps>(wire->maps(*nodes));
        ++node_map_updates_;
        spdlog::info("[nodes] {}: the node levels changed (registry version {}): sending the GameState's node maps ({})", name(), node_maps_version_, nodes->describe_levels());
        queue("GameState node maps", [maps](bootstrap::PlayerBootstrap& pb) { return pb.node_maps_update(*maps); }, conn, /*must_build=*/false);
    }
    if (cooldowns_pending_ && has_room()) {
        cooldowns_pending_ = false;
        ++siege_cooldowns_sent_;
        queue("ClientReceiveAllCurrentNodeSiegeCooldowns", [](bootstrap::PlayerBootstrap& pb) { return pb.siege_cooldowns(); }, conn, /*must_build=*/true);
    }
    const auto unknown = [this](int64_t node, const char* what) {
        if (node_unknown_logged_) return;
        node_unknown_logged_ = true;
        spdlog::warn("[nodes] {}: the client asked {} for node {}, which world does not know: not answered (further ones are not logged)", name(), what, node);
    };
    static const size_t kMaxNodeSummariesPerTick = []() -> size_t {
        const char* s = std::getenv("AOC_WORLD_NODE_SUMMARIES_PER_TICK");
        if (!s || !*s) return 4;
        char* end = nullptr;
        const long n = std::strtol(s, &end, 10);
        return end != s ? static_cast<size_t>(std::clamp<long>(n, 1, 128)) : 4;
    }();
    size_t sent_this_tick = 0;
    while (!siege_wait_.empty() && sent_this_tick < kMaxNodeSummariesPerTick && has_room()) {
        const int64_t node = siege_wait_.front();
        siege_wait_.pop_front();
        siege_queued_.erase(node);
        const auto s = nodes && wire ? wire->siege(node, *nodes) : std::nullopt;
        if (!s) {
            if (nodes && nodes->by_guid(node)) spdlog::debug("[nodes] {}: no siege summary for {}: it is no node of a map (retail does not answer it either)", name(), node);
            else unknown(node, "for its siege summary");
            continue;
        }
        ++siege_summaries_sent_;
        ++sent_this_tick;
        queue("ClientReceiveSiegeSummary", [s](bootstrap::PlayerBootstrap& pb) { return pb.siege_summary(*s); }, conn, /*must_build=*/true);
    }
    while (siege_wait_.empty() && !node_summary_wait_.empty() && sent_this_tick < kMaxNodeSummariesPerTick && has_room()) {
        const int64_t node = node_summary_wait_.front();
        node_summary_wait_.pop_front();
        node_summary_queued_.erase(node);
        const int64_t unix_now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto s = nodes && wire ? wire->summary(node, *nodes, unix_now) : std::nullopt;
        if (!s) {
            unknown(node, "for its summary");
            continue;
        }
        ++node_summaries_sent_;
        ++sent_this_tick;
        if (node_summaries_sent_ == 1 || node_summaries_sent_ == 49) {
            spdlog::info("[nodes] {}: node summary #{}: {} at level {} with {} plots", name(), node_summaries_sent_,
                         nodes->by_guid(node) ? nodes->by_guid(node)->display_name : std::to_string(node), s->level, s->plots.size());
        }
        queue("ClientReceiveNodeSummary", [s](bootstrap::PlayerBootstrap& pb) { return pb.node_summary(*s); }, conn, /*must_build=*/true);
    }
}

void PlayerSession::SessionInternal::handle_gm_node_clock_rpc(PlayerSession& s, const repl::ClientRpc& rpc, const char* params, Clock::time_point now) {
    const std::string rpc_name = rpc.call.rpc->name;
    world::NodeControl* control = s.world_ ? s.world_->node_control() : nullptr;
    const std::string by = rpc_name + " by " + s.name();
    const auto admitted = [&](const std::string& shown) { return s.gm_rpc_admitted(rpc_name.c_str(), shown, now) && rpc.call.decoded; };
    const auto unavailable = [&] { spdlog::warn("[gm] {}: {}: the node service is unavailable (world runs without a citynode connection)", s.name(), rpc_name); };
    if (rpc_name == "ServerGMWhatTimeIsIt") {
        if (!admitted(params)) return;
        if (!control) return unavailable();
        ++s.gm_rpcs_answered_;
        control->clock(by);
    } else if (rpc_name == "ServerGMFastForwardRealTime") {
        int64_t seconds = 0;
        if (rpc.call.decoded) {
            if (const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "FastForwardDuration")) {
                if (const repl::Ticks* t = v->get_if<repl::Ticks>()) seconds = t->ticks / 10000000;
            }
        }
        if (!admitted("FastForwardDuration=" + std::to_string(seconds) + " s")) return;
        if (seconds <= 0) {
            spdlog::warn("[gm] {}: ServerGMFastForwardRealTime of {} s: nothing to skip", s.name(), seconds);
            return;
        }
        if (!control) return unavailable();
        ++s.gm_rpcs_answered_;
        control->advance_clock(seconds, by);
    } else {
        s.gm_rpc_admitted(rpc_name.c_str(), params, now);
        spdlog::log(s.level_for(s.node_logged_, 20),
                    "[gm] {}: {} is not implemented in Nodes 1 (only ServerSetNodeLevel, ServerAddNodeExperience, ServerGMGetNodeData, ServerGMWhatTimeIsIt and "
                    "ServerGMFastForwardRealTime reach the node service): ignored",
                    s.name(), rpc_name);
    }
}

void PlayerSession::on_gm_node_rpc(const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string rpc_name = rpc.call.rpc->name;
    ++node_gm_rpcs_;
    char params[256];
    std::snprintf(params, sizeof params, "%zu parameter bits%s", rpc.call.param_bits, rpc.call.decoded ? "" : "; the parameters did not decode");
    world::NodeControl* control = world_ ? world_->node_control() : nullptr;
    const std::string by = rpc_name + " by " + name();
    const auto admitted = [&](const std::string& shown) { return gm_rpc_admitted(rpc_name.c_str(), shown, now) && rpc.call.decoded; };
    const auto unavailable = [&] { spdlog::warn("[gm] {}: {}: the node service is unavailable (world runs without a citynode connection)", name(), rpc_name); };
    if (rpc_name == "ServerSetNodeLevel") {
        const auto g = repl::parse_gm_set_node_level(rpc.call);
        const std::string shown = g ? "InNode=\"" + g->node.substr(0, 80) + "\" InLevel=" + std::to_string(g->level) + " InCulture=" + std::to_string(g->culture) +
                                          " bDisableVassalshipSystem=" + (g->disable_vassalship ? "1" : "0")
                                    : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->set_level(g->node, g->level, g->disable_vassalship, by);
    } else if (rpc_name == "ServerAddNodeExperience") {
        const auto g = repl::parse_gm_add_node_experience(rpc.call);
        const std::string shown = g ? "InNode=\"" + g->node.substr(0, 80) + "\" Experience=" + std::to_string(g->experience) + " InCulture=" + std::to_string(g->culture) +
                                          " bUseRawXP=" + (g->raw_xp ? "1" : "0")
                                    : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->add_experience(g->node, g->experience, by);
    } else if (rpc_name == "ServerGMGetNodeData") {
        const auto g = repl::parse_gm_get_node_data(rpc.call);
        const std::string shown = g ? "NodeName=\"" + g->substr(0, 80) + "\"" : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->node_data(*g, by);
    } else {
        SessionInternal::handle_gm_node_clock_rpc(*this, rpc, params, now);
    }
}

}  // namespace worldd
}  // namespace aoc
