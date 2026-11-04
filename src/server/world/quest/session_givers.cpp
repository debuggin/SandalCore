// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/player_session.h"

#include "content/content_db.h"
#include "quest/quest_wire.h"

#include <spdlog/spdlog.h>

#include <set>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

constexpr double kGiverRadiusCm = 36100.0;
constexpr std::chrono::seconds kGiversEvery{5};
constexpr size_t kMaxGivers = 600;
constexpr size_t kMaxNewGiversPerLook = 24;

struct GiverCandidate {
    int64_t     instance = 0, asset = 0;
    std::string name;
};

struct ChosenGiverOffer {
    int64_t                  registration = 0;
    uint8_t                  flavor = quest::kGiverFlavorNone;
    const content::QuestRow* quest = nullptr;
};

template <typename GiverMap>
std::vector<GiverCandidate> collect_giver_candidates(const world::ActorRegistry& registry, const quest::QuestBook& qb, const GoodPosition& at,
                                                     bool log_changed, const GiverMap& givers) {
    std::vector<GiverCandidate> candidates;
    std::vector<uint64_t> near_ids;
    registry.query(at.x, at.y, kGiverRadiusCm, near_ids);
    std::set<int64_t> seen;
    for (const uint64_t id : near_ids) {
        const world::WorldActor* a = registry.find(id);
        if (!a || a->kind != world::ActorKind::Npc || a->npc.instance_guid == 0) continue;
        if (qb.offers(a->npc.instance_guid, a->npc.asset_guid).empty() || !seen.insert(a->npc.instance_guid).second) continue;
        candidates.push_back({a->npc.instance_guid, a->npc.asset_guid, a->npc.name});
    }
    if (log_changed) {
        for (const auto& [instance, e] : givers) {
            if (seen.insert(instance).second) candidates.push_back({instance, e.asset, e.name});
        }
    }
    return candidates;
}

ChosenGiverOffer choose_giver_offer(const quest::QuestBook& qb, const quest::QuestLog& log, dialogue::Evaluator& ev, int64_t instance, int64_t asset) {
    ChosenGiverOffer out;
    for (const content::QuestOfferRow& offer : qb.offers(instance, asset)) {
        const content::QuestRow* def = qb.quest(offer.quest);
        if (!def || def->giver_registration == 0) continue;
        if (log.can_start(def, ev, /*bypass_rules=*/false) != quest::Create::Ok) continue;
        out.registration = def->giver_registration;
        out.flavor = quest::giver_flavor(def->flavor);
        out.quest = def;
        break;
    }
    return out;
}

template <typename Entry>
bool update_existing_giver(repl::FastArrayState& array, Entry& e, const GiverCandidate& c, const ChosenGiverOffer& offer, bool& givers_dirty) {
    // Client appends each registration and only clears on 0 ("none"); transition between two non-zero registrations via 0.
    const bool via_none = e.registration != 0 && offer.registration != 0;
    const int64_t send_registration = via_none ? 0 : offer.registration;
    const uint8_t send_flavor = via_none ? quest::kGiverFlavorNone : offer.flavor;
    const auto item = quest::giver_item(array.layout(), c.instance, send_flavor, send_registration);
    if (!item || !array.change(e.id, *item)) return false;
    e.registration = send_registration;
    e.flavor = send_flavor;
    if (via_none) givers_dirty = true;
    return true;
}

}  // namespace

void PlayerSession::log_quest_markers(const quest::MarkerContext& markers) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (markers.refused > 0) spdlog::warn("[session] {}: {} objective(s) have places to mark and no pin: they do not fit the layout of the quest component's marker array", name(), markers.refused);
    for (const quest::MarkerContext::Made& m : markers.made) {
        std::string objective = std::to_string(m.objective), quest_name = std::to_string(m.quest);
        if (const content::QuestRow* d = qb ? qb->quest(m.quest) : nullptr) {
            quest_name = d->name;
            for (const content::QuestObjectiveRow& o : d->objectives) {
                if (o.guid == m.objective) objective = o.name;
            }
        }
        spdlog::log(level_for(quests_logged_, 300), "[session] {}: map pin for the objective {} of quest {}: {} place(s), the first {} at ({:.0f}, {:.0f}){}", name(), objective, quest_name, m.places,
                    m.area ? "a circle" : "a pin", m.x, m.y, m.area ? ", radius " + std::to_string(static_cast<long long>(m.radius / 100.0)) + " m" : std::string());
    }
}

void PlayerSession::tick_quest_givers(Connection& conn, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    if (!quests_on_ || !qb || !book || !registry_ || !phase_in_done_ || dead_) return;
    if (!givers_dirty_ && now - givers_checked_ < kGiversEvery) return;
    const bool log_changed = givers_dirty_;
    givers_dirty_ = false;
    givers_checked_ = now;
    repl::FastArrays* arrays = pb_.quest_arrays();
    const protocol::schema::ClassSchema* cls = quest::component_schema();
    repl::FastArrayState* array = arrays && cls ? arrays->get(*cls, "QuestGiverInfos") : nullptr;
    if (!array) return;
    const std::vector<GiverCandidate> candidates = collect_giver_candidates(*registry_, *qb, checker_.good(), log_changed, givers_);
    if (candidates.empty()) return;
    const auto facts = dialogue_facts(nullptr);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a quest could not be answered and does not hold: {} (told once)", name(), what);
    });
    bool changed = false;
    size_t added = 0;
    for (const GiverCandidate& c : candidates) {
        const ChosenGiverOffer offer = choose_giver_offer(*qb, quest_log_, ev, c.instance, c.asset);
        const auto it = givers_.find(c.instance);
        if (it == givers_.end()) {
            if (offer.registration == 0) continue;
            if (givers_.size() >= kMaxGivers) {
                if (!givers_told_) spdlog::warn("[session] {}: the client was told of {} quest givers: no more are added", name(), givers_.size());
                givers_told_ = true;
                continue;
            }
            if (added >= kMaxNewGiversPerLook) {
                givers_dirty_ = true;
                continue;
            }
            const auto item = quest::giver_item(array->layout(), c.instance, offer.flavor, offer.registration);
            const auto id = item ? array->add(*item) : std::nullopt;
            if (!id) {
                spdlog::warn("[session] {}: a quest giver does not fit the layout of the quest component's giver array", name());
                return;
            }
            ++added;
            givers_[c.instance] = {*id, offer.registration, offer.flavor, c.asset, c.name};
            spdlog::log(level_for(quests_logged_, 300), "[session] {}: {} has a quest to take ({}): the client is told of its mark", name(), c.name, offer.quest ? offer.quest->name : std::string("?"));
            changed = true;
            continue;
        }
        GiverEntry& e = it->second;
        if (e.registration == offer.registration && e.flavor == offer.flavor) continue;
        const bool via_none = e.registration != 0 && offer.registration != 0;
        if (!update_existing_giver(*array, e, c, offer, givers_dirty_)) {
            spdlog::warn("[session] {}: a quest giver's item cannot be changed", name());
            return;
        }
        spdlog::log(level_for(quests_logged_, 300), "[session] {}: {} {}", name(), c.name,
                    via_none ? std::string("goes from one quest-available mark to another (through none)") : offer.quest ? "has a quest to take (" + offer.quest->name + "): the mark changes" : std::string("has no quest to take any more: the mark is taken off"));
        changed = true;
    }
    if (changed) queue("QuestConsumerComponent", [](bootstrap::PlayerBootstrap& pb) { return pb.quest_update(); }, conn, /*must_build=*/false);
}

}}  // namespace aoc::worldd
