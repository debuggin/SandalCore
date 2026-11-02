#pragma once

#include "session/player_world.h"
#include "crafting/quick.h"
#include "net/connection.h"
#include "net/control_channel.h"
#include "session/mounts.h"
#include "session/riding.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace aoc {
namespace worldd {

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

inline constexpr std::array<std::string_view, 9> kCombatEchoPaths = {
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MayHarmAngle",
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ProximityRadius",
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MaxDistance",
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ForwardOffset",
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.TabTargetMethod",
    "CharacterInGameSettings.CombatSettings.Targeting.bEnableHoverCasting",
    "CharacterInGameSettings.CombatSettings.Targeting.SoftLock.VisualsMask",
    "CharacterInGameSettings.CombatSettings.Input.BufferDuration",
    "CharacterInGameSettings.CombatSettings.Rendering.PartyVFX",
};

class PlayerSessionTypes {
 public:
  class CombatSide;
  struct SessionInternal;
    static constexpr size_t kMaxPendingReplies = 64;
    static constexpr uint32_t                  kLoadBurst = 4;
    static constexpr std::chrono::seconds      kLoadInterval{2};
    static constexpr uint32_t                  kStorageBurst = 16;
    static constexpr std::chrono::milliseconds kStorageInterval{100};
    static constexpr uint32_t                  kAbilityBurst = 8;
    static constexpr std::chrono::milliseconds kAbilityInterval{100};
    static constexpr uint32_t                  kHitBurst = 40;
    static constexpr std::chrono::milliseconds kHitInterval{25};
    static constexpr uint32_t                  kCancelBurst = 16;
    static constexpr std::chrono::milliseconds kCancelInterval{50};
    static constexpr size_t                    kMaxPendingConfirms = 256;
    static constexpr size_t                    kMaxZoneOps = 64;
    static constexpr size_t                    kConfirmsPerCall = 20;
    static constexpr uint32_t                  kTalentBurst = 8;
    static constexpr std::chrono::milliseconds kTalentInterval{100};
    static constexpr uint32_t                  kBarBurst = 16;
    static constexpr std::chrono::milliseconds kBarInterval{100};
    static constexpr uint32_t                  kBarPurchaseAllowance = 64;
    static constexpr std::chrono::milliseconds kPurchaseReplyDelay{84};
    static constexpr int64_t                   kMaxRegenSteps = 5;
    static constexpr uint32_t                  kItemBurst = 8;
    static constexpr std::chrono::milliseconds kItemInterval{100};
    static constexpr uint32_t                  kResyncBurst = 4;
    static constexpr std::chrono::seconds      kResyncInterval{2};
    static constexpr double                    kMovingSpeed = 30.0;
    static constexpr uint32_t                  kRideBurst = 8;
    static constexpr std::chrono::milliseconds kRideInterval{100};
    static constexpr size_t kLoggedPerKind = 3;
    static constexpr size_t kLoggedMounts = 40;
    static constexpr size_t kLoggedAbilityTransitions = 100;
    static constexpr int64_t kFirstPassiveEffectId = 1000000;
    static constexpr std::chrono::milliseconds kCrateDropDelay{694};
    static constexpr int kDropCrateBank = 2, kDropCrateSlot = 11;
    static constexpr uint32_t kNoInstance = 0xFFFFFFFFu;
    static constexpr double                    kInteractRangeCm = 2500.0;
    static constexpr uint32_t                  kVendorBurst = 8;
    static constexpr std::chrono::milliseconds kVendorInterval{100};
    static constexpr uint32_t                  kTaxBurst = 3;
    static constexpr std::chrono::milliseconds kTaxInterval{1000};
    static constexpr uint32_t                  kLootBurst = 8;
    static constexpr std::chrono::milliseconds kLootInterval{100};
    static constexpr size_t                    kMaxLootTakes = 16;
    static constexpr uint32_t                  kQuietBurst = 16;
    static constexpr std::chrono::milliseconds kQuietInterval{50};
    static constexpr size_t                    kMaxMultiSlots = 64;
    static constexpr int64_t                   kRespawnRecord = 6064632337807835941ll;
    struct GmMoveResult {
        enum class Status : uint8_t { Moved, Dead, OutsideWorld, Busy } status = Status::Moved;
        GoodPosition from, to;
        bool         dismounted = false;
    };
    static constexpr double kNetHoldSeconds = 1.0;     // the position must stay under the plane this long (a glitch in one move is not a fall)
    static constexpr double kNetRepeatSeconds = 3.0;   // ... and a rescue that did not hold (the client had no floor yet) is tried again after this
    struct GmSpeedChange {
        double previous = 1.0, now = 1.0;
    };
    struct GmMoneyResult {
        bool    ok = false;
        std::string refusal;
        int64_t previous = 0, balance = 0, added = 0;
    };
    struct GmWeaponResult {
        bool        ok = false;
        std::string refusal;
        std::string line;                       // the line's name
        int64_t     xp_before = 0, xp_after = 0;
        int         level_before = 1, level_after = 1;
        int64_t     points_earned = 0, points_left = 0;   // the Weapon points the line's level gives, and what the ranks bought in its tree leave
    };
    struct GmItemResult {
        bool        ok = false;
        bool        bad_request = false;   // refused for the rarity or the quality asked for (not for the item, the count or the room)
        std::string refusal;
        std::string item_name;
        int32_t     count = 0;
        std::string container;   // where the first stack went: Backpack or a resource container
        int32_t     quality = 0;   // the quality the stacks were given at
        std::string rarity;        // ... and the rarity the player sees them in
    };
    static constexpr size_t kMaxNodeRequests = 256;

 protected:
    static GoodPosition start_of(const bootstrap::PlayerSetup& setup) {
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
        bool must_build = false;   // a step that builds nothing is a warning, not a debug line (the phase-in)
    };

    struct ChainExports {
        std::vector<std::vector<protocol::emit::FIntrepidNetworkGUID>> by_bunch;
        bool   reliable = true;
        size_t outstanding = 0;   // export parts without a final event yet
    };
    struct ProfessionWire {
        int64_t leveling_type = 0, xp = 0, certification = 0;
    };
    enum class DismountWhy : uint8_t { Request, Desummon, Summon, Ability, PlayerDied, MountDied, MountGone, GmTeleport };
    enum class Interrupt : uint8_t { None, Movement, Death, Mounted, Generic };
    struct HarvestFrame {
        std::string                  what;            // the log's name of the thing harvested ("Plant_Daffodil_Medium_1 (node 12)"); also the pile's record
        int64_t                      profession = 0;  // the profession whose XP the harvest gives
        const gather::Harvest*       harvest = nullptr;
        std::string                  tool_guid;       // the tool's instance and Equipment slot (the wear)
        int                          tool_slot = 0;
        std::function<void()>        after_items;     // queued after the storage events and the full-bag callout, before the XPTracker
        std::function<void()>        after_experience;   // queued after the XPTracker, the level-up frames and the skill updates
        std::string                  log_tail;        // appended to the log line ("; the node returns in 30 s")
        std::string                  tag = "gather";  // the log line's prefix: [gather] for a node, [foliage] for a tree or plant
    };
    static constexpr std::chrono::seconds kTimeSyncAfterPhaseIn{1};
    static constexpr std::chrono::seconds kTimeSyncSecondAfterPhaseIn{5};
    static constexpr std::chrono::seconds kTimeSyncInterval{120};
    struct Delayed {
        Clock::time_point due;
        Step              step;
    };
    static constexpr size_t kMaxUnreadKinds = 16;
    static constexpr size_t kMaxInPlaceNames = 12, kMaxInPlaceSharedLogged = 20;
    static constexpr size_t kStaleStreakLog = 30;
    static constexpr std::chrono::seconds kPositionLogInterval{30};
    struct SpeedTally { double cm = 0, seconds = 0; };
};

}  // namespace worldd
}  // namespace aoc
