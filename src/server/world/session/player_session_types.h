#pragma once

#include "crafting/quick.h"
#include "net/connection.h"
#include "net/control_channel.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "session/item_cast.h"
#include "session/mounts.h"
#include "session/player_world.h"
#include "session/summoner_pets.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#if __has_include("session/rpc_table.h")
#include "session/rpc_table.h"
#endif

namespace aoc {
namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

#if !__has_include("session/rpc_table.h")
class PlayerSession;

enum class RpcScope : uint8_t { PlayerController, Pawn, AbilityComponent, FtueComponent };

struct RpcContext {
    Connection&                      conn;
    bootstrap::Role                  role;
    const protocol::repl::ClientRpc& rpc;
    const InBunch&                   bunch;
    Clock::time_point                now;
};

using RpcHandler = void (*)(PlayerSession&, const RpcContext&);

class RpcTable {
public:
    void add(RpcScope scope, std::string_view name, RpcHandler handler) {
        for (const Entry& e : names_) {
            if (e.scope == scope && e.name == name) throw std::logic_error("the client call " + std::string(name) + " is registered twice");
        }
        names_.push_back({scope, std::string(name), handler});
    }
    void add_prefix(RpcScope scope, std::string_view prefix, RpcHandler handler) { prefixes_.push_back({scope, std::string(prefix), handler}); }

    RpcHandler find(RpcScope scope, std::string_view name) const {
        for (const Entry& e : names_) {
            if (e.scope == scope && name == e.name) return e.handler;
        }
        for (const Entry& e : prefixes_) {
            if (e.scope == scope && name.substr(0, e.name.size()) == e.name) return e.handler;
        }
        return nullptr;
    }
    size_t size() const { return names_.size() + prefixes_.size(); }

private:
    struct Entry {
        RpcScope    scope;
        std::string name;
        RpcHandler  handler;
    };
    std::vector<Entry> names_, prefixes_;
};
#endif

class RequestBudget {
public:
    RequestBudget(uint32_t burst, Clock::duration per_token) : burst_(burst), per_token_(per_token), tokens_(burst) {}

    bool take(Clock::time_point now) {
        if (!started_) {
            started_ = true;
            last_ = now;
        } else if (now > last_) {
            const int64_t earned = (now - last_) / per_token_;
            if (earned > 0) {
                tokens_ = static_cast<uint32_t>(std::min<int64_t>(burst_, static_cast<int64_t>(tokens_) + earned));
                last_ += earned * per_token_;
            }
            if (tokens_ == burst_) last_ = now;   // a full bucket does not save up
        }
        if (tokens_ == 0) return false;
        --tokens_;
        return true;
    }
    uint32_t tokens() const { return tokens_; }

private:
    uint32_t          burst_;
    Clock::duration   per_token_;
    uint32_t          tokens_;
    bool              started_ = false;
    Clock::time_point last_{};
};

std::string url_option(std::string_view url, std::string_view key);

bootstrap::LoginIdentity make_login_identity(const std::optional<LoginMessage>& message, const svc::Character& character,
                                             std::vector<std::string>* notes);

inline constexpr std::string_view kCombatEchoPrefix = "CharacterInGameSettings.";

struct GmMoveResult {
    enum class Status : uint8_t { Moved, Dead, OutsideWorld, Busy } status = Status::Moved;
    GoodPosition from, to;
    bool         dismounted = false;
};

struct GmSpeedChange {
    double previous = 1.0, now = 1.0;
};

struct GmMoneyResult {
    bool        ok = false;
    std::string refusal;
    int64_t     previous = 0, balance = 0, added = 0;
};

struct GmWeaponResult {
    bool        ok = false;
    std::string refusal;
    std::string line;
    int64_t     xp_before = 0, xp_after = 0;
    int         level_before = 1, level_after = 1;
    int64_t     points_earned = 0, points_left = 0;
};

struct GmItemResult {
    bool        ok = false;
    bool        bad_request = false;
    std::string refusal;
    std::string item_name;
    int32_t     count = 0;
    std::string container;
    int32_t     quality = 0;
    std::string rarity;
};

inline GoodPosition start_of(const bootstrap::PlayerSetup& setup) {
    GoodPosition g;
    g.x = setup.x;
    g.y = setup.y;
    g.z = setup.z;
    g.yaw_degrees = setup.yaw_degrees;
    return g;
}

enum class Outcome { Sent, Wait, Refused };

struct Step {
    const char* what;
    std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build;
    bool must_build = false;
};

struct ChainExports {
    std::vector<std::vector<protocol::emit::FIntrepidNetworkGUID>> by_bunch;
    bool   reliable = true;
    size_t outstanding = 0;
};

struct ProfessionWire {
    int64_t leveling_type = 0, xp = 0, certification = 0;
};

enum class DismountWhy : uint8_t { Request, Desummon, Summon, Ability, PlayerDied, MountDied, MountGone, GmTeleport };
enum class Interrupt : uint8_t { None, Movement, Death, Mounted, Generic };

struct HarvestFrame {
    std::string            what;
    int64_t                profession = 0;
    const gather::Harvest* harvest = nullptr;
    std::string            tool_guid;
    int                    tool_slot = 0;
    std::function<void()>  after_items;
    std::function<void()>  after_experience;
    std::string            log_tail;
    std::string            tag = "gather";
};

struct Delayed {
    Clock::time_point due;
    Step              step;
};

struct SpeedTally { double cm = 0, seconds = 0; };

struct GatherRun {
    uint64_t                             node = 0;
    protocol::emit::FIntrepidNetworkGUID target;
    const content::GatherDefRow*         def = nullptr;
    int                                  tool_slot = 0;
    std::string                          tool_guid;
    Clock::time_point                    started{};
    double                               seconds = 0;
    double                               start_x = 0, start_y = 0;
    bool                                 hunt = false;
    bool                                 gives_nothing = false;
    std::string                          what;
};

struct GatherPile {
    Clock::time_point              due{};
    std::vector<combat::LootStack> stacks;
    double                         x = 0, y = 0, z = 0;
    std::string                    node_name;
};

struct GatherCallout {
    Clock::time_point due{};
    uint64_t          key = 0;
};

struct FoliageRun {
    uint32_t                     row = 0xFFFFFFFFu;
    int                          state = 0;
    uint32_t                     index = 0;
    std::string                  owner, component;
    const content::GatherDefRow* interaction = nullptr;
    std::string                  name;
    int                          tool_slot = 0;
    std::string                  tool_guid;
    int64_t                      tool_record = 0;
    Clock::time_point            started{}, data_at{}, completes{};
    double                       seconds = 0;
    double                       start_x = 0, start_y = 0;
    bool                         data_sent = false;
};

struct Marker {
    Clock::time_point due;
    int64_t           key = 0;
};

struct ZoneActor {
    uint64_t actor = 0;
};

struct WhComponent {
    warehouse::Key                key;
    warehouse::Spec               spec;
    bootstrap::WarehouseComponent comp;
};

struct WaitingReward {
    std::vector<int64_t> tables;
    std::string          why;
    int64_t              quest = 0;
};

struct CrateEvent {
    bool    deposit = false;
    int64_t node = 0, project = 0, item = 0;
    int32_t count = 0;
};

struct GiverEntry {
    int32_t     id = -1;
    int64_t     registration = 0;
    uint8_t     flavor = 7;
    int64_t     asset = 0;
    std::string name;
};

inline bool is_bag(svc::ItemContainer c) { return c == svc::ItemContainer::Backpack || c >= svc::ItemContainer::Resource0; }

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

inline std::optional<svc::ItemContainer> container_of_location(uint8_t location) {
    if (location == static_cast<uint8_t>(repl::ItemLocation::Backpack)) return svc::ItemContainer::Backpack;
    if (location == static_cast<uint8_t>(repl::ItemLocation::Equipment)) return svc::ItemContainer::Equipment;
    return std::nullopt;
}

struct StackSpec {
    int64_t record = 0;
    int32_t count = 0;
    int64_t created_ms = 0;
    int32_t quality = 0;
};

void stacks_of(const StackSpec& spec, const content::ItemRecord& rec, std::vector<svc::InventoryItem>& out);

struct BasedPlace {
    protocol::repl::Vec3d location, acceleration;
};

struct BaseKey {
    uint64_t    object = 0;
    std::string path;
    std::string bone;
    bool operator==(const BaseKey&) const = default;
};

struct BaseTrack {
    BaseKey               key;
    protocol::repl::Vec3d offset;
    protocol::repl::Vec3d place;
};

struct HeldMoveAck {
    float time_stamp = 0;
    bool  on_mount = false;
};

struct TickPlan {
    bool   stopped = false;
    bool   view = false;
    bool   view_due = false;
    double x = 0, y = 0;
};

}  // namespace worldd
}  // namespace aoc
