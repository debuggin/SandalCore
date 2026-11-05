// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "protocol/repl/item_rpc.h"
#include "session/player_session.h"

namespace aoc {
namespace worldd {

void stacks_of(const StackSpec& spec, const content::ItemRecord& rec, std::vector<svc::InventoryItem>& out) {
  const int32_t stack = rec.max_stack_size > 0 ? rec.max_stack_size : 1;
  for (int32_t left = spec.count; left > 0;) {
    svc::InventoryItem it;
    it.container = svc::ItemContainer::Backpack;
    it.item_record_id = spec.record;
    it.quantity = std::min(left, stack);
    it.quality = spec.quality > 0 ? spec.quality : rec.default_quality;
    it.durability = rec.max_durability_at(it.quality);
    it.created_at_ms = spec.created_ms;
    left -= it.quantity;
    out.push_back(std::move(it));
  }
}

namespace {

bool erase_rank_ability(int64_t a, std::vector<int64_t>& known, std::vector<int64_t>& records) {
  if (a == 0) return false;
  known.erase(std::remove(known.begin(), known.end(), a), known.end());
  records.erase(std::remove(records.begin(), records.end(), a), records.end());
  return true;
}

bool append_rank_ability(int64_t a, std::vector<int64_t>& known, std::vector<int64_t>& records) {
  if (a == 0) return false;
  if (std::find(known.begin(), known.end(), a) == known.end()) known.push_back(a);
  if (std::find(records.begin(), records.end(), a) == records.end()) records.push_back(a);
  return true;
}

template <typename Markers>
void rollback_failed_effects(const bootstrap::AbilityChange& effects, std::map<int64_t, int64_t>& rank_effects,
                             Markers& markers) {
  for (const bootstrap::EffectItem& e : effects.effects_added) {
    std::erase_if(rank_effects, [&](const auto& kv) { return kv.second == e.effect_id; });
    std::erase_if(markers, [&](const auto& m) { return m.key == e.effect_id; });
  }
}

repl::SkillOnActionbar make_skill_bar_update(const talents::RankRef& ref) {
  repl::SkillOnActionbar u;
  u.skill_id = ref.node->skill;
  u.skill_tree_id = ref.tree->guid;
  u.rank_id = 0;
  u.is_refunding = false;
  u.use_hint = true;
  return u;
}

PlayerSession::GmWeaponResult validate_gm_weapon_xp(const weaponxp::State& weapon, const talents::Book* book,
                                                    const talents::Tree* tree, std::pair<int64_t, int64_t> type_and_xp,
                                                    std::string_view session_name) {
  PlayerSession::GmWeaponResult r;
  const weaponxp::Lines* lines = weapon.lines();
  const weaponxp::Line* line = lines ? lines->find(type_and_xp.first) : nullptr;
  if (!line) {
    r.refusal = weapon.valid() ? "no weapon line has the leveling type " + std::to_string(type_and_xp.first)
                               : "this world has no weapon lines (its content.db lacks their leveling types)";
    return r;
  }
  r.line = line->name;
  r.xp_before = weapon.xp(type_and_xp.first);
  r.level_before = weapon.level(type_and_xp.first);
  if (type_and_xp.second < 0) {
    r.refusal = "an XP total cannot be negative";
    return r;
  }
  r.xp_after = std::min(type_and_xp.second, lines->max_xp());
  r.level_after = lines->level_of(r.xp_after);
  if (tree && book && r.level_after < r.level_before) {
    const std::string why = book->line_shortfall(*tree, r.level_after);
    if (!why.empty()) {
      r.refusal = std::string(session_name) + "'s " + line->name + " cannot go down to level " +
                  std::to_string(r.level_after) + ": " + why + " (refund them in the Skill Tree first)";
    }
  }
  return r;
}

void fill_gm_weapon_points(const talents::Book* book, const talents::Tree* tree, int64_t leveling_type,
                           PlayerSession::GmWeaponResult& r) {
  if (!book) return;
  const int64_t point = book->catalog().point_type(talents::Point::Weapon);
  if (tree) {
    r.points_earned = book->catalog().earned(tree->leveling_type, r.level_after, point);
    r.points_left = book->points_left(*tree, point);
  } else {
    r.points_earned = book->catalog().earned(leveling_type, r.level_after, point);
    r.points_left = r.points_earned;
  }
}

struct GmGiveSpec {
  int64_t item_record = 0;
  int32_t count = 0;
  std::string_view rarity;
  int32_t quality = 0;
};

std::optional<content::ItemRecord> prepare_gm_give_item(const content::ContentDb* db, const GmGiveSpec& spec,
                                                        PlayerSession::GmItemResult& r, int32_t& given_quality_out) {
  if (!db) {
    r.refusal = "worldd has no content.db to read the item from";
    return std::nullopt;
  }
  if (spec.count < 1 || spec.count > 100000) {
    r.refusal = "the count must be 1 to 100,000";
    return std::nullopt;
  }
  std::string error;
  const auto rec = db->item(spec.item_record, error);
  if (!rec) {
    r.refusal = error.empty() ? "the data holds no item record " + std::to_string(spec.item_record)
                              : "content.db failed: " + error;
    return std::nullopt;
  }
  r.item_name = rec->name;
  given_quality_out = 0;
  if (!spec.rarity.empty() || spec.quality != 0) {
    r.bad_request = true;
    if (!rec->has_quality) {
      r.refusal = rec->name +
                  " has no quality in the game's data (its rarity is fixed by the record): give it without a rarity or "
                  "a quality";
      return std::nullopt;
    }
    std::string why;
    given_quality_out = items::quality_for_request(*rec, rec->name, std::string(spec.rarity), spec.quality, why);
    if (given_quality_out == 0) {
      r.refusal = why;
      return std::nullopt;
    }
    r.bad_request = false;
  }
  r.quality = given_quality_out > 0 ? given_quality_out : rec->default_quality;
  r.rarity = std::string(items::rarity_name(rec->rarity_of(static_cast<double>(r.quality))));
  return rec;
}

std::vector<svc::InventoryItem> mint_gm_item_stacks(int64_t item_record, int32_t count, int32_t given_quality,
                                                    const content::ItemRecord& rec,
                                                    const std::function<std::string()>& mint_guid) {
  std::vector<svc::InventoryItem> fresh;
  const int64_t created_ms =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() *
      1000;
  stacks_of({item_record, count, created_ms, given_quality}, rec, fresh);
  for (svc::InventoryItem& it : fresh) {
    it.instance_guid = mint_guid();
    for (const svc::InventoryItem& other : fresh) {
      if (&other != &it && other.instance_guid == it.instance_guid) it.instance_guid = mint_guid();
    }
  }
  return fresh;
}

void sync_plan_trees(const talents::Book& book, const std::vector<int64_t>& trees, bootstrap::PlayerBootstrap& pb,
                     std::string_view session_name) {
  for (const int64_t t : trees) {
    const talents::Tree* tree = book.catalog().tree(t);
    if (tree && !pb.set_tree_ranks(t, book.list(*tree))) {
      spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", session_name, tree->name);
    }
  }
}

void log_talent_purchase(const repl::SkillBuild& build, const talents::Plan& plan,
                         const bootstrap::AbilityChange& effects, size_t known_count,
                         std::pair<std::string_view, spdlog::level::level_enum> session_and_level) {
  const size_t bought = static_cast<size_t>(
      std::count_if(plan.changes.begin(), plan.changes.end(), [](const talents::Change& c) { return c.purchase; }));
  spdlog::log(session_and_level.second,
              "[session] {}: ServerPurchaseSkillBuild: {} rank(s) bought, {} refunded{}; {} effect item(s) added, {} "
              "removed; KnownAbilities {}",
              session_and_level.first, bought, plan.changes.size() - bought, build.refund_only() ? " (a respec)" : "",
              effects.effects_added.size(), effects.effects_removed.size(), known_count);
}

template <typename Markers>
void apply_or_rollback_talent_effects(bootstrap::PlayerBootstrap& pb, const bootstrap::AbilityChange& effects,
                                      std::map<int64_t, int64_t>& rank_effects, Markers& markers,
                                      const std::pair<std::string_view, const char*>& name_and_what) {
  if (effects.empty() || pb.apply_ability_change(effects)) return;
  spdlog::warn("[session] {}: the pawn does not take the {} ({}): the ranks stand without them", name_and_what.first,
               name_and_what.second, pb.error().empty() ? "keys disagree" : pb.error());
  rollback_failed_effects(effects, rank_effects, markers);
}

struct WeaponLineStepDiff {
  std::vector<talents::RankRef> removed;
  std::vector<talents::RankRef> added;
};

bool contains_rank_guid(const std::vector<talents::RankRef>& v, const talents::RankRef& x) {
  return std::any_of(v.begin(), v.end(), [&](const talents::RankRef& y) { return y.rank->guid == x.rank->guid; });
}

WeaponLineStepDiff step_weapon_line_ranks(talents::Book& book, const talents::Tree* tree, const weaponxp::Change& c,
                                          bootstrap::PlayerBootstrap& pb, std::string_view session_name) {
  WeaponLineStepDiff diff;
  const std::vector<talents::RankRef> had = tree ? book.auto_ranks_of(*tree) : std::vector<talents::RankRef>();
  book.set_line_level(c.line, c.level_after);
  if (!tree) return diff;
  const std::vector<talents::RankRef> has = book.auto_ranks_of(*tree);
  for (const talents::RankRef& gone : had) {
    if (!contains_rank_guid(has, gone)) diff.removed.push_back(gone);
  }
  for (const talents::RankRef& granted : has) {
    if (!contains_rank_guid(had, granted)) diff.added.push_back(granted);
  }
  if (!pb.set_tree_ranks(tree->guid, book.list(*tree))) {
    spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", session_name, tree->name);
  }
  return diff;
}

std::optional<repl::SkillBuild> parse_and_log_skill_build(const repl::ClientRpc& rpc, bool& unreadable_logged,
                                                          std::string_view session_name) {
  const auto build = repl::parse_purchase_skill_build(rpc.call);
  if (!build && !unreadable_logged) {
    unreadable_logged = true;
    spdlog::warn(
        "[session] {}: a ServerPurchaseSkillBuild does not read ({} parameter bits): ignored (further ones are not "
        "logged)",
        session_name, rpc.call.param_bits);
  }
  return build;
}

void place_purchased_skill_on_bar(const talents::RankRef& ref, actionbar::Bar& bar,
                                  std::vector<repl::SkillOnActionbar>& updates) {
  if (!ref.node->auto_hotbar) return;
  if (ref.rank->ability != 0) bar.place_skill(ref.rank->ability, ref.rank->guid);
  updates.push_back(make_skill_bar_update(ref));
}

}  // namespace

std::vector<int64_t> PlayerSession::equipped_weapon_lines() const {
  std::vector<int64_t> found;
  const content::ContentDb* db = world_ ? world_->content() : nullptr;
  if (!weapon_.valid() || !db) return found;
  for (const svc::InventoryItem& it : inventory_.items()) {
    if (it.container != svc::ItemContainer::Equipment) continue;
    std::string error;
    const auto w = db->item_weapon(it.item_record_id, error);
    if (!w || w->skill_line == 0 || w->skill_line_name.empty()) continue;
    const weaponxp::Line* line = weapon_.lines()->find_named(w->skill_line_name);
    if (line && std::find(found.begin(), found.end(), line->leveling_type) == found.end())
      found.push_back(line->leveling_type);
  }
  std::vector<int64_t> ordered;
  for (const weaponxp::Line& l : weapon_.lines()->all()) {
    if (std::find(found.begin(), found.end(), l.leveling_type) != found.end()) ordered.push_back(l.leveling_type);
  }
  return ordered;
}

const talents::Tree* PlayerSession::weapon_tree_of(int64_t leveling_type) const {
  if (!book_) return nullptr;
  for (const talents::Tree& t : book_->catalog().trees()) {
    if (t.kind == talents::Kind::Weapon && t.leveling_type == leveling_type) return &t;
  }
  return nullptr;
}

bool PlayerSession::apply_weapon_levels(const std::vector<weaponxp::Change>& changes, Clock::time_point now) {
  if (!book_) return false;
  bootstrap::AbilityChange effects;
  bool lists_changed = false;
  bool level_moved = false;
  for (const weaponxp::Change& c : changes) {
    if (c.line == 0 || c.level_before == c.level_after) continue;
    level_moved = true;
    const WeaponLineStepDiff diff = step_weapon_line_ranks(*book_, weapon_tree_of(c.line), c, pb_, name());
    for (const talents::RankRef& gone : diff.removed) {
      remove_rank_effect(gone.rank->guid, effects);
      lists_changed = erase_rank_ability(gone.rank->ability, known_, records_) || lists_changed;
    }
    for (const talents::RankRef& granted : diff.added) {
      lists_changed = append_rank_ability(granted.rank->ability, known_, records_) || lists_changed;
      add_rank_effect(granted, now, effects);
    }
  }
  if (lists_changed) push_abilities();
  apply_or_rollback_talent_effects(pb_, effects, rank_effects_, markers_, {name(), "weapon ranks' effect items"});
  if (!effects.empty()) abilities_dirty_ = true;
  if (level_moved) apply_gear_stats("a weapon line's level");
  return level_moved || !effects.empty();
}

PlayerSession::GmWeaponResult PlayerSession::gm_set_weapon_xp(Connection& conn, int64_t leveling_type, int64_t xp,
                                                              Clock::time_point now, const std::string& by) {
  const talents::Tree* tree = weapon_tree_of(leveling_type);
  GmWeaponResult r = validate_gm_weapon_xp(weapon_, book_ ? &*book_ : nullptr, tree, {leveling_type, xp}, name());
  if (!r.refusal.empty()) return r;
  if (!room_for(4)) {
    r.refusal = name() + "'s connection is busy: try again in a moment";
    return r;
  }
  const weaponxp::Change change = weapon_.set_xp(leveling_type, r.xp_after);
  progress_changed_ = true;
  const bool pawn_changed = apply_weapon_levels({change}, now);
  fill_gm_weapon_points(book_ ? &*book_ : nullptr, tree, leveling_type, r);
  const int64_t total = r.xp_after;
  queue(
      "XPTracker (a weapon line)",
      [leveling_type, total](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_weapon_line(leveling_type, total)) return std::nullopt;
        return pb.leveling_update();
      },
      conn, /*must_build=*/true);
  if (change.leveled()) {
    const int level = r.level_after;
    queue(
        "BroadcastPlayLevelUpEffects",
        [leveling_type, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(leveling_type, level); },
        conn);
  }
  if (pawn_changed) {
    queue(
        "pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("a weapon line"); }, conn,
        /*must_build=*/false);
  }
  r.ok = true;
  spdlog::info("[gm] {}: {} set by {}: {} XP, level {} -> {} XP, level {}; {} Weapon point(s) earned, {} left", name(),
               r.line, by, r.xp_before, r.level_before, r.xp_after, r.level_after, r.points_earned, r.points_left);
  hand_over_progress("a game master set a weapon line");
  return r;
}

PlayerSession::GmItemResult PlayerSession::gm_give_item(Connection& conn, int64_t item_record, int32_t count,
                                                        Clock::time_point now, const std::string& by,
                                                        const std::string& rarity, int32_t quality) {
  (void)now;
  GmItemResult r;
  int32_t given_quality = 0;
  const auto rec = prepare_gm_give_item(world_ ? world_->content() : nullptr, {item_record, count, rarity, quality}, r,
                                        given_quality);
  if (!rec) return r;
  if (!room_for(2 + static_cast<size_t>(count / std::max(1, rec->max_stack_size)) / bootstrap::kMaxEventsPerChain)) {
    r.refusal = name() + "'s connection is busy: try again in a moment";
    return r;
  }
  std::vector<svc::InventoryItem> fresh =
      mint_gm_item_stacks(item_record, count, given_quality, *rec, [this] { return fresh_guid(); });
  const auto crate = item_catalog().info(item_record);
  if (crate && crate->cargo_crate() && count == 1 && fresh.size() == 1 &&
      !inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
    items::Outcome put = inventory_.grant_cargo(fresh.front(), item_catalog());
    if (!put.ok) {
      r.refusal = "the cargo slot does not take it: " + put.refusal;
      return r;
    }
    r.ok = true;
    r.count = 1;
    r.container = items::container_name(svc::ItemContainer::Equipment);
    apply_outcome(conn, put, "a crate was put in the cargo slot");
    spdlog::info("[gm] {}: {} given by {} into the cargo slot (quality {})", name(), r.item_name, by, r.quality);
    return r;
  }
  const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
  if (!outcome.ok) {
    r.refusal = "no room: " + outcome.refusal;
    return r;
  }
  r.ok = true;
  r.count = count;
  r.container = outcome.events.empty() ? "" : items::container_name(outcome.events.front().container);
  inventory_dirty_ = inventory_changed_ = true;
  ++items_applied_;
  const std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
  for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
    const std::vector<bootstrap::ItemEvent> part(
        events.begin() + static_cast<std::ptrdiff_t>(at_event),
        events.begin() +
            static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
    queue(
        "ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn,
        /*must_build=*/true);
  }
  spdlog::info("[gm] {}: {} x {} given by {} as {} (quality {}), {} storage event(s), first into {}", name(), count,
               r.item_name, by, r.rarity, r.quality, outcome.events.size(), r.container);
  return r;
}

repl::ActionbarPayload PlayerSession::bar_payload() const {
  repl::ActionbarPayload p;
  p.slots = bar_.wire_slots();
  p.auto_granted = false;
  return p;
}

bool PlayerSession::push_abilities() {
  if (pipeline_) pipeline_->set_known(known_);
  std::vector<bootstrap::AbilityRecord> records;
  records.reserve(records_.size());
  for (const int64_t g : records_) records.push_back({g, content::kTypeAbilityRecord});
  if (!pb_.set_abilities(known_, records)) {
    spdlog::debug("[session] {}: the pawn does not take the ability lists yet", name());
    return false;
  }
  return true;
}

void PlayerSession::rebuild_ability_lists() {
  const content::ContentDb* db = world_ ? world_->content() : nullptr;
  if (!book_ || !db) return;
  const talents::AbilityLists lists = talents::join_abilities(
      *book_, bootstrap::general_unstuck(), combo_, bootstrap::carried_combos(*db, inventory_),
      bootstrap::static_known_tail(),
      bootstrap::carried_item_abilities(world_->mounts() ? &world_->mounts()->data() : nullptr, inventory_, db));
  known_ = lists.known;
  records_ = lists.records;
}

void PlayerSession::sync_item_abilities(Connection& conn) {
  const content::ContentDb* db = world_ ? world_->content() : nullptr;
  if (!world_ || (!world_->mounts() && !db) || !phase_in_done_ || !join_.empty() || finished_ || failed_ ||
      abilities_failed_)
    return;
  const std::vector<int64_t> carried_records = inventory_.records();
  if (carried_records == item_ability_records_) return;
  const std::vector<int64_t> carried = bootstrap::carried_item_abilities(
      world_->mounts() ? &world_->mounts()->data() : nullptr, inventory_, db);
  std::vector<int64_t> missing;
  for (const int64_t a : carried) {
    if (std::find(records_.begin(), records_.end(), a) == records_.end()) missing.push_back(a);
  }
  if (missing.empty()) {
    item_ability_records_ = carried_records;
    return;
  }
  if (!room_for(1)) return;
  for (const int64_t a : missing) records_.push_back(a);
  if (!push_abilities()) {
    for (const int64_t a : missing) records_.erase(std::remove(records_.begin(), records_.end(), a), records_.end());
    return;
  }
  item_ability_records_ = carried_records;
  ++item_ability_syncs_;
  spdlog::log(level_for(item_ability_logged_),
              "[session] {}: {} item ability record(s) added to ReplicatedRecordsToLoad (an item that casts an ability "
              "is carried: the client needs the ability loaded to send its trigger)",
              name(), missing.size());
  queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("item abilities"); }, conn);
}

void PlayerSession::add_rank_effect(const talents::RankRef& ref, Clock::time_point now,
                                    bootstrap::AbilityChange& effects) {
  const talents::Rank& rank = *ref.rank;
  if (rank.effect == 0 || rank_effects_.count(rank.guid)) return;
  if (ref.tree->kind == talents::Kind::Weapon && ref.tree->weapon_line != wielded_line_) return;
  bootstrap::EffectItem e;
  e.effect_id = next_passive_effect_id_++;
  e.effect_guid = rank.effect;
  e.start_time = bootstrap::effect_clock(pb_.setup().world_time + session_seconds(now));
  e.duration = rank.effect_infinite ? std::numeric_limits<float>::max() : static_cast<float>(rank.effect_duration);
  e.tick_time = 0;
  e.stacks = 1;
  effects.effects_added.push_back(e);
  rank_effects_[rank.guid] = e.effect_id;
  if (!rank.effect_infinite) {
    markers_.push_back(
        {now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(rank.effect_duration)),
         e.effect_id});
  }
}

void PlayerSession::remove_rank_effect(int64_t rank_guid, bootstrap::AbilityChange& effects) {
  const auto it = rank_effects_.find(rank_guid);
  if (it == rank_effects_.end()) return;
  const int64_t key = it->second;
  rank_effects_.erase(it);
  markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == key; }),
                 markers_.end());
  const auto added = std::find_if(effects.effects_added.begin(), effects.effects_added.end(),
                                  [&](const bootstrap::EffectItem& e) { return e.effect_id == key; });
  if (added != effects.effects_added.end()) {
    effects.effects_added.erase(added);
    return;
  }
  effects.effects_removed.push_back(key);
}

void PlayerSession::grant_ranks(const std::vector<talents::RankRef>& granted, Clock::time_point now,
                                bootstrap::AbilityChange& effects, std::vector<repl::SkillOnActionbar>& updates) {
  bool lists_changed = false;
  for (const talents::RankRef& r : granted) {
    if (r.rank->ability != 0) {
      if (std::find(known_.begin(), known_.end(), r.rank->ability) == known_.end()) {
        known_.push_back(r.rank->ability);
        lists_changed = true;
      }
      if (std::find(records_.begin(), records_.end(), r.rank->ability) == records_.end()) {
        records_.push_back(r.rank->ability);
        lists_changed = true;
      }
    }
    add_rank_effect(r, now, effects);
    if (r.node->auto_hotbar) {
      if (r.rank->ability != 0 && bar_.place_skill(r.rank->ability, r.rank->guid)) progress_changed_ = true;
      updates.push_back(make_skill_bar_update(r));
    }
  }
  if (lists_changed) push_abilities();
}

void PlayerSession::switch_class(const content::ClassRecord& cls, Clock::time_point now,
                                 bootstrap::AbilityChange& effects, std::vector<repl::SkillOnActionbar>& updates,
                                 std::vector<std::pair<int, int>>& cleared) {
  (void)updates;
  const talents::Tree* old_tree = book_->class_tree();
  std::set<int64_t> old_ranks;
  if (old_tree) {
    for (const talents::RankRef& r : book_->listed(*old_tree)) {
      old_ranks.insert(r.rank->guid);
      remove_rank_effect(r.rank->guid, effects);
    }
  }
  book_->set_class(cls.guid);
  const talents::Tree* new_tree = book_->class_tree();
  if (old_tree && old_tree != new_tree) pb_.set_tree_ranks(old_tree->guid, {});
  std::vector<talents::RankRef> listed;
  if (new_tree) {
    pb_.set_tree_ranks(new_tree->guid, book_->list(*new_tree));
    listed = book_->listed(*new_tree);
  }
  for (const auto& slot : bar_.remove_skills(old_ranks)) cleared.push_back(slot);
  for (const talents::RankRef& r : listed) {
    add_rank_effect(r, now, effects);
    if (r.node->auto_hotbar && r.rank->ability != 0) bar_.place_skill(r.rank->ability, r.rank->guid);
  }
  rebuild_ability_lists();
  push_abilities();
  progress_changed_ = true;
}

void PlayerSession::refuse_purchase(Connection& conn, const repl::SkillBuild& build, const std::string& why,
                                    Clock::time_point now) {
  ++talents_refused_;
  spdlog::log(level_for(talent_refused_logged_),
              "[session] {}: ServerPurchaseSkillBuild ({} change(s)) refused, nothing changed: {}", name(),
              build.changes.size(), why);
  if (!resync_budget_.take(now) || !room_for(1)) return;
  const talents::Catalog& catalog = book_->catalog();
  std::set<int64_t> trees;
  for (const repl::SkillRankChange& c : build.changes) {
    if (trees.size() >= 4) break;
    if (catalog.tree(c.tree.guid)) trees.insert(c.tree.guid);
  }
  if (trees.empty()) {
    if (const talents::Tree* ct = book_->class_tree()) trees.insert(ct->guid);
  }
  bool any = false;
  for (const int64_t t : trees) {
    const talents::Tree* tree = catalog.tree(t);
    if (!tree) continue;
    const std::vector<int64_t> list = book_->list(*tree);
    if (list.empty()) continue;
    if (!pb_.resend_tree(t)) pb_.set_tree_ranks(t, list);
    any = true;
  }
  if (any) queue("XPTracker", [](bootstrap::PlayerBootstrap& pb) { return pb.leveling_update(); }, conn);
}

void PlayerSession::on_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  if (!book_) return;
  const auto build = parse_and_log_skill_build(rpc, talents_unreadable_logged_, name());
  if (!build || !within_budget(talent_budget_, "ServerPurchaseSkillBuild", now)) return;
  const size_t purchases = build->purchases();
  if (purchases + 3 > kMaxPendingReplies)
    return refuse_purchase(conn, *build, "more ranks than one reply can announce", now);
  if (!room_for(purchases + 3)) return;
  const talents::Plan plan = book_->plan(*build);
  if (!plan.ok) return refuse_purchase(conn, *build, plan.refusal, now);
  bootstrap::AbilityChange effects;
  std::vector<repl::SkillOnActionbar> updates;
  bool lists_changed = false;
  for (const talents::Change& c : plan.changes) {
    if (c.purchase) continue;
    remove_rank_effect(c.ref.rank->guid, effects);
    lists_changed = erase_rank_ability(c.ref.rank->ability, known_, records_) || lists_changed;
    bar_.remove_skills({c.ref.rank->guid});
  }
  book_->apply(plan);
  for (const talents::Change& c : plan.changes) {
    if (!c.purchase) continue;
    lists_changed = append_rank_ability(c.ref.rank->ability, known_, records_) || lists_changed;
    add_rank_effect(c.ref, now, effects);
    place_purchased_skill_on_bar(c.ref, bar_, updates);
  }
  sync_plan_trees(*book_, plan.trees, pb_, name());
  if (lists_changed) push_abilities();
  apply_or_rollback_talent_effects(pb_, effects, rank_effects_, markers_, {name(), "rank effects' items"});
  apply_gear_stats("the talents");
  ++talents_applied_;
  queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("talents"); }, conn, now);
  queue_later(
      "XPTracker", [](bootstrap::PlayerBootstrap& pb) { return pb.leveling_update(); }, conn, now, /*must_build=*/true);
  for (const repl::SkillOnActionbar& u : updates) {
    queue_later(
        "ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn,
        now, /*must_build=*/true);
  }
  bar_allow_ =
      static_cast<uint32_t>(std::min<size_t>(kBarPurchaseAllowance, static_cast<size_t>(bar_allow_) + updates.size()));
  log_talent_purchase(*build, plan, effects, known_.size(), {name(), level_for(talent_logged_)});
  progress_changed_ = true;
  hand_over_progress("a talent change");
}

void PlayerSession::on_set_slot(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  (void)conn;
  if (!book_) return;
  const auto slot = repl::parse_set_actionbar_slot(rpc.call);
  if (!slot) {
    if (!talents_unreadable_logged_) {
      talents_unreadable_logged_ = true;
      spdlog::warn(
          "[session] {}: a ServerSetActionbarSlotUseData does not read ({} parameter bits): ignored (further ones are "
          "not logged)",
          name(), rpc.call.param_bits);
    }
    return;
  }
  if (bar_allow_ > 0)
    --bar_allow_;
  else if (!within_budget(bar_budget_, "ServerSetActionbarSlotUseData", now))
    return;
  const std::optional<std::pair<int, int>> server_slot = bar_.first_free();
  std::string why;
  if (!bar_.set(*slot, &why)) {
    ++bar_refused_;
    spdlog::log(level_for(bar_refused_logged_),
                "[session] {}: ServerSetActionbarSlotUseData (bank {}, slot {}) refused, nothing changed: {}", name(),
                slot->bank_index, slot->slot_index, why);
    return;
  }
  if (slot->use.use_type == repl::ActionbarUseType::Skill && slot->use.skill_rank_id != 0 &&
      slot->bank_index < actionbar::kClassBanks) {
    bar_.remove_other_copies(slot->use.skill_rank_id, slot->bank_index, slot->slot_index);
  }
  ++bar_sets_;
  ++bar_logged_;
  spdlog::info(
      "[session] {}: the client set action bar bank {} slot {} (use type {}, ability {}, rank {}): stored; the "
      "server's first free class slot was {}",
      name(), slot->bank_index, slot->slot_index, static_cast<unsigned>(slot->use.use_type), slot->use.ability_id,
      slot->use.skill_rank_id,
      server_slot ? std::to_string(server_slot->first) + "/" + std::to_string(server_slot->second)
                  : std::string("none"));
  progress_changed_ = true;
  hand_over_progress("the action bar");
}

void PlayerSession::on_set_ranged(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  if (!book_ || !world_ || !world_->content()) return;
  const auto use = repl::parse_set_use_ranged_weapon(rpc.call);
  if (!use) return;
  if (!within_budget(item_budget_, "ServerSetUseRangedWeapon", now) || !room_for(2)) return;
  if (*use == use_ranged_) return;
  use_ranged_ = *use;
  ++weapon_swaps_;
  if (!pb_.set_using_ranged_weapon(use_ranged_))
    spdlog::warn("[session] {}: the pawn does not take CombatInfo.bIsUsingRangedWeapon", name());
  const auto weapon = bootstrap::worn_weapon(*world_->content(), inventory_,
                                             use_ranged_ ? items::equip::Ranged : items::equip::Primary);
  if (!weapon) {
    spdlog::info("[session] {}: the weapon set is the {} one now and nothing is worn in its slot: the combo stays",
                 name(), use_ranged_ ? "ranged" : "melee");
    queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("weapon swap"); }, conn);
    return;
  }
  put_weapon_in_hand(weapon->combo, weapon->line, weapon->line_name);
  apply_gear_stats("the weapon swap");
  spdlog::info("[session] {}: the {} weapon set: combo {} is the last known ability, weapon line {}", name(),
               use_ranged_ ? "ranged" : "melee", weapon->combo, weapon->line_name);
  queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("weapon swap"); }, conn);
}

void PlayerSession::put_weapon_in_hand(int64_t combo, int64_t line, const std::string& line_name) {
  if (combo_ != 0) known_.erase(std::remove(known_.begin(), known_.end(), combo_), known_.end());
  known_.erase(std::remove(known_.begin(), known_.end(), combo), known_.end());
  known_.push_back(combo);
  records_.erase(std::remove(records_.begin(), records_.end(), combo), records_.end());
  records_.push_back(combo);
  combo_ = combo;
  wielded_line_ = line;
  push_abilities();
  pb_.set_weapon_skill_line(line, line_name);
}

void PlayerSession::follow_worn_weapon() {
  if (!book_ || !world_ || !world_->content()) return;
  const content::ContentDb& db = *world_->content();
  std::optional<bootstrap::WeaponInfo> weapon =
      bootstrap::worn_weapon(db, inventory_, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
  bool moved_hand = false;
  if (!weapon) {
    if (const auto other =
            bootstrap::worn_weapon(db, inventory_, use_ranged_ ? items::equip::Primary : items::equip::Ranged)) {
      use_ranged_ = !use_ranged_;
      if (!pb_.set_using_ranged_weapon(use_ranged_))
        spdlog::warn("[session] {}: the pawn does not take CombatInfo.bIsUsingRangedWeapon", name());
      weapon = other;
      moved_hand = true;
    }
  }
  if (weapon) {
    if (!moved_hand && weapon->combo == combo_) {
      wielded_line_ = weapon->line;
      return;
    }
    put_weapon_in_hand(weapon->combo, weapon->line, weapon->line_name);
    spdlog::info("[session] {}: {}: the {} weapon set is in hand, combo {} is the last known ability, weapon line {}",
                 name(),
                 moved_hand ? "the weapon in hand was taken off and the other set holds a weapon"
                            : "the weapon worn in the set in hand changed",
                 use_ranged_ ? "ranged" : "melee", weapon->combo, weapon->line_name);
    return;
  }
  wielded_line_ = 0;
  if (combo_ == 0) return;
  const int64_t gone = combo_;
  known_.erase(std::remove(known_.begin(), known_.end(), gone), known_.end());
  combo_ = 0;
  push_abilities();
  spdlog::info("[session] {}: the weapon in hand was taken off and no other is worn: combo {} leaves KnownAbilities",
               name(), gone);
}

void PlayerSession::register_talents_rpcs(RpcTable& t) {
  t.add(RpcScope::PlayerController, "ServerSetActionbarSlotUseData",
        [](PlayerSession& s, const RpcContext& c) { s.on_set_slot(c.conn, c.rpc, c.now); });
  t.add(RpcScope::PlayerController, "ServerSetActionbarMultiSlotUseData",
        [](PlayerSession& s, const RpcContext& c) { s.on_set_slots(c.conn, c.rpc, c.now); });
  t.add(RpcScope::Pawn, "ServerPurchaseSkillBuild",
        [](PlayerSession& s, const RpcContext& c) { s.on_purchase(c.conn, c.rpc, c.now); });
  t.add(RpcScope::Pawn, "ServerSetUseRangedWeapon",
        [](PlayerSession& s, const RpcContext& c) { s.on_set_ranged(c.conn, c.rpc, c.now); });
}

}  // namespace worldd
}  // namespace aoc
