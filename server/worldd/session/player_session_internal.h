#pragma once

#include "session/player_session.h"
#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "content/content_db.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/rpc_fields.h"
#include "session/gm_travel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace sandal::worldd::session::detail {

namespace bootstrap = ::aoc::worldd::bootstrap;
namespace items = ::aoc::worldd::items;
namespace protocol = ::aoc::protocol;
namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace svc = ::aoc::worldd::svc;
namespace wire = ::aoc::protocol::wire;
namespace world = ::aoc::worldd::world;
using ::aoc::worldd::Chain;
using ::aoc::worldd::Connection;
using ::aoc::worldd::PlayerSession;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

constexpr double kMountGroundUpCm = 160.0;
constexpr double kMountGroundDownCm = 1000.0;

inline bool is_bag(svc::ItemContainer c) { return c == svc::ItemContainer::Backpack || c >= svc::ItemContainer::Resource0; }

inline const char* role_name(Role r) {
    switch (r) {
        case Role::PlayerController: return "PlayerController";
        case Role::GameState: return "GameState";
        case Role::Pawn: return "pawn";
        case Role::PlayerState: return "PlayerState";
    }
    return "?";
}

inline const char* mode_name(bootstrap::GameStateMode m) {
    switch (m) {
        case bootstrap::GameStateMode::Tier1: return "tier1";
        case bootstrap::GameStateMode::Tier2: return "tier2";
        case bootstrap::GameStateMode::Tier3: return "tier3";
        case bootstrap::GameStateMode::Captured: return "captured";
    }
    return "?";
}

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

inline size_t parts_of(const Chain& c) {
    const size_t bits = c.payload.bit_pos();
    const size_t content =
        bits > Connection::kMaxSingleBunchBits ? (bits + Connection::kMaxPartialBunchBits - 1) / Connection::kMaxPartialBunchBits : 1;
    return c.exports.size() + content;
}

struct NoCatalog : items::ItemCatalog {
    std::optional<items::ItemInfo> info(int64_t) const override { return std::nullopt; }
};

inline std::optional<svc::ItemContainer> container_of_location(uint8_t location) {
    if (location == static_cast<uint8_t>(repl::ItemLocation::Backpack)) return svc::ItemContainer::Backpack;
    if (location == static_cast<uint8_t>(repl::ItemLocation::Equipment)) return svc::ItemContainer::Equipment;
    return std::nullopt;
}

inline std::optional<svc::ItemContainer> material_bag_of_location(uint8_t location) {
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (location == static_cast<uint8_t>(repl::ItemLocation::Resource0) + i) return items::resource_container(i);
    }
    return std::nullopt;
}

inline FIntrepidNetworkGUID component_guid_of(const world::WorldActor& a, uint64_t object_id) {
    FIntrepidNetworkGUID g = a.actor.guid;
    g.ObjectId = object_id;
    return g;
}

inline const repl::Value* rpc_value(const repl::ClientRpc& rpc, std::string_view name) {
    if (!rpc.call.decoded || !rpc.call.rpc) return nullptr;
    const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, name);
    if (!c) return nullptr;
    const auto it = rpc.call.params.values.find(c->handle);
    return it == rpc.call.params.values.end() ? nullptr : &it->second;
}

inline double number_value(const repl::Value* v) {
    if (!v) return 0.0;
    if (const auto* i = v->get_if<int64_t>()) return static_cast<double>(*i);
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<double>(*u);
    if (const auto* d = v->get_if<double>()) return *d;
    if (const auto* f = v->get_if<float>()) return static_cast<double>(*f);
    return 0.0;
}

inline std::string text_value(const repl::Value* v) {
    if (!v) return {};
    if (const auto* s = v->get_if<std::string>()) return *s;
    return {};
}

inline const char* gm_refusal(PlayerSession::GmMoveResult::Status s) {
    switch (s) {
        case PlayerSession::GmMoveResult::Status::Dead: return "the player is dead";
        case PlayerSession::GmMoveResult::Status::OutsideWorld: return "outside the world";
        case PlayerSession::GmMoveResult::Status::Busy: return "the reply queue is full";
        case PlayerSession::GmMoveResult::Status::Moved: break;
    }
    return "moved";
}

}  // namespace sandal::worldd::session::detail

namespace aoc {
namespace worldd {

using protocol::emit::FIntrepidNetworkGUID;

class PlayerSessionTypes::CombatSide final : public combat::PlayerPort, public combat::Fighter {
public:
    explicit CombatSide(PlayerSession& s) : s_(s), aligns_(combat::CombatData::player_alignments()) {}

    uint64_t actor_id() const override { return s_.pb_.setup().guids.pawn.ObjectId; }
    combat::Fighter& fighter() override { return *this; }
    void receive_hit(const combat::IncomingHit& hit, double) override {
        if (inbox.size() < 256) inbox.push_back(hit);
    }
    void set_in_combat(bool in_combat, double) override { wants_in_combat = in_combat; }
    std::array<uint32_t, 4> character_guid() const override { return s_.pb_.setup().character_guid; }
    FIntrepidNetworkGUID pawn_guid() const override { return s_.pb_.setup().guids.pawn; }
    FIntrepidNetworkGUID cylinder_guid() const override { return s_.pb_.setup().guids.collision_cylinder; }
    void receive_kill(const combat::KillReward& kill, double) override {
        if (kills.size() < 64) kills.push_back(kill);
    }

    uint64_t id() const override { return actor_id(); }
    bool is_player() const override { return true; }
    int level() const override { return s_.stats_ ? s_.stats_->level() : 1; }
    combat::Vec3 position() const override {
        const GoodPosition& g = s_.checker_.good();
        return {g.x, g.y, g.z};
    }
    double radius_cm() const override { return 34.0; }
    double facing_degrees() const override { return s_.checker_.good().yaw_degrees; }   // the control rotation's yaw, as the client's last good move gave it
    bool alive() const override {
        const stats::StatValue* h = s_.stats_ ? s_.stats_->find(content::kGuidHealth) : nullptr;
        return h && h->value > 0;
    }
    const std::vector<int64_t>& alignments() const override { return aligns_; }
    stats::CharacterStats& stats() override { return *s_.stats_; }
    std::vector<stats::WornItem> worn() const override { return bootstrap::worn_items_of(s_.inventory_); }

    std::vector<combat::IncomingHit> inbox;
    std::vector<combat::KillReward>  kills;
    bool                             wants_in_combat = false;

private:
    PlayerSession&       s_;
    std::vector<int64_t> aligns_;
};

struct PlayerSessionTypes::SessionInternal {
  static uint64_t enrich_join_setup(PlayerWorld& world, const svc::Character& character, bootstrap::CharacterSetupResult& built);
  static void init_joined_state(PlayerSession& p, PlayerWorld& world, const svc::Character& character, bootstrap::CharacterSetupResult& built, uint64_t nodes_version);
  static void init_mounts_and_view(PlayerSession& p, PlayerWorld& world);
  static void log_join_summary(PlayerSession& p, PlayerWorld& world);
  static void tick_view_and_saves(PlayerSession& s, Connection& conn, Clock::time_point now);
  static bool dispatch_controller_early_rpc(PlayerSession& s, Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
  static bool dispatch_controller_actor_rpc(PlayerSession& s, Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
  static bool dispatch_pawn_actor_rpc(PlayerSession& s, Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now);
  static bool check_stale_or_borderline_move(PlayerSession& s, const repl::NetworkMove& move, const MoveVerdict& verdict);
  static void accept_move(PlayerSession& s, Connection& conn, const repl::NetworkMove& move, const GoodPosition& before, Clock::time_point now);
  static void refuse_move(PlayerSession& s, const repl::NetworkMove& move, const MoveVerdict& verdict);
  static bool begin_mount_possession(PlayerSession& s, const world::WorldActor& a, const content::MountRow& row, double t);
  static void finish_mount_up(PlayerSession& s, Connection& conn, const world::WorldActor& a, const content::MountRow& row, double t);
  static void handle_gm_node_clock_rpc(PlayerSession& s, const repl::ClientRpc& rpc, const char* params, Clock::time_point now);
};

}  // namespace worldd
}  // namespace aoc
