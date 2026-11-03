// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "session/item_use.h"
#include "session/player_session.h"

namespace aoc {
namespace worldd {

namespace {

std::optional<svc::ItemContainer> material_bag_of_location(uint8_t location) {
  for (int i = 0; i < items::kResourceContainers; ++i) {
    if (location == static_cast<uint8_t>(repl::ItemLocation::Resource0) + i) return items::resource_container(i);
  }
  return std::nullopt;
}

using QueueStepFn = std::function<void(const char*, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)>, bool)>;

struct GearStatCounts {
  size_t owner = 0, owner_proxy = 0, everyone = 0, without_item = 0;
};

bool count_replicated_gear_stat(const stats::StatValue& v, bool new_item, GearStatCounts& c) {
  if (v.float_array() || (v.replication != 1 && v.replication != 5 && v.replication != 7)) return false;
  if (new_item) ++c.without_item;
  (v.replication == 1 ? c.owner : v.replication == 5 ? c.owner_proxy : c.everyone) += 1;
  return true;
}

GearStatCounts collect_gear_stat_changes(const std::vector<stats::StatValue>& changed, const bootstrap::PlayerBootstrap& pb, AbilityRuntime* abilities,
                                         double gm_speed, bootstrap::AbilityChange& change) {
  GearStatCounts c;
  for (const stats::StatValue& v : changed) {
    const bool new_item = !pb.has_stat_item(v.guid);
    if (!count_replicated_gear_stat(v, new_item, c)) continue;
    const auto shown = v.guid == spawns::kStatMoveSpeedMult ? static_cast<decltype(v.value)>(v.value * gm_speed) : v.value;
    if (!new_item && abilities && abilities->tracks(v.guid)) {
      abilities->gear_stat(change, v.guid, shown);
    } else if (!(abilities && v.guid == content::kGuidStamina)) {
      change.stats.push_back({v.guid, shown, v.buckets(), v.base, v.equip, static_cast<uint8_t>(new_item ? v.replication : 0)});
    }
  }
  return c;
}

void sync_level_up_trees(const talents::Book& book, bootstrap::PlayerBootstrap& pb, std::string_view session_name) {
  for (const talents::Tree* tree : {book.catalog().universal(), book.class_tree()}) {
    if (tree && !pb.set_tree_ranks(tree->guid, book.list(*tree))) {
      spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", session_name, tree->name);
    }
  }
}

template <typename Markers>
bool apply_or_rollback_rank_effects(bootstrap::PlayerBootstrap& pb, const bootstrap::AbilityChange& rank_effects,
                                    std::map<int64_t, int64_t>& session_rank_effects, Markers& markers, std::string_view session_name) {
  if (rank_effects.empty()) return false;
  if (!pb.apply_ability_change(rank_effects)) {
    spdlog::warn("[session] {}: the pawn does not take the granted passives' effect items ({})", session_name,
                 pb.error().empty() ? "keys disagree" : pb.error());
    for (const bootstrap::EffectItem& e : rank_effects.effects_added) {
      std::erase_if(session_rank_effects, [&](const auto& kv) { return kv.second == e.effect_id; });
      std::erase_if(markers, [&](const auto& m) { return m.key == e.effect_id; });
    }
  }
  return true;
}

std::vector<talents::RankRef> collect_level_auto_ranks(talents::Book& book, int from, int to) {
  const talents::Catalog& catalog = book.catalog();
  std::vector<talents::RankRef> granted;
  book.set_level(to);
  for (const talents::Tree* tree : {catalog.universal(), book.class_tree()}) {
    if (!tree) continue;
    const std::vector<talents::RankRef> had = catalog.auto_ranks(*tree, from);
    for (const talents::RankRef& r : catalog.auto_ranks(*tree, to)) {
      if (std::none_of(had.begin(), had.end(), [&](const talents::RankRef& h) { return h.node->skill == r.node->skill; })) granted.push_back(r);
    }
  }
  std::stable_sort(granted.begin(), granted.end(),
                   [](const talents::RankRef& a, const talents::RankRef& b) { return a.node->level_requirement < b.node->level_requirement; });
  return granted;
}

void attach_profession_xp_runner(use::Context& ctx, const abilities::Catalog* ability_catalog, const stats::CharacterStats* stats,
                                 const std::vector<stats::WornItem>& worn) {
  if (!ctx.professions || !ability_catalog || !stats || !stats->started()) return;
  auto runner = std::make_shared<professions::XpRunner>();
  ctx.profession_xp = [runner, ability_catalog, stats, worn](int64_t expr, const std::map<std::string, int>& levels,
                                                             std::string& why) -> std::optional<double> {
    if (!*runner) *runner = professions::make_xp_runner(*ability_catalog, *stats, worn, why);
    return *runner ? (*runner)(expr, levels, why) : std::nullopt;
  };
}

using UseInvStep = std::pair<bool, std::vector<svc::InventoryItem>>;

UseInvStep make_use_inv_step(bool consume, const std::vector<use::Granted>& items, const std::function<std::string()>& mint_guid) {
  std::vector<svc::InventoryItem> fresh;
  const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
  for (const use::Granted& g : items) fresh.push_back({svc::ItemContainer::Backpack, 0, g.record, mint_guid(), g.quantity, g.quality, g.durability, created});
  return {consume, std::move(fresh)};
}

std::string check_use_inventory(const items::Inventory& inv, const items::ItemCatalog& catalog, int32_t slot, const std::string& guid, const UseInvStep& step) {
  if (step.second.empty()) return {};
  items::Inventory trial = inv;
  if (step.first) trial.consume(svc::ItemContainer::Backpack, slot, guid, 1);
  if (trial.can_grant(step.second, catalog)) return {};
  const size_t free_slots = static_cast<size_t>(items::kBackpackSlots) - inv.count(svc::ItemContainer::Backpack);
  return "the storages have no room for the use's " + std::to_string(step.second.size()) + " item(s) (the bag has " + std::to_string(free_slots) +
         " free slot(s)): nothing is used";
}

items::Outcome apply_use_inventory(items::Inventory& inv, const items::ItemCatalog& catalog, int32_t slot, const std::string& guid,
                                   std::pair<std::string_view, UseInvStep&> step) {
  items::Outcome all = step.second.first ? inv.consume(svc::ItemContainer::Backpack, slot, guid, 1) : items::Outcome{true, {}, {}};
  if (!all.ok || step.second.second.empty()) return all;
  const items::Outcome g = inv.grant(std::move(step.second.second), catalog);
  if (!g.ok) spdlog::error("[session] {}: the use's items are not granted ({}): the item was consumed and nothing came of it", step.first, g.refusal);
  else all.events.insert(all.events.end(), g.events.begin(), g.events.end());
  return all;
}

std::optional<content::ClassRecord> resolve_use_class(const content::ContentDb& db, bootstrap::PlayerBootstrap& pb, std::optional<int32_t> set_class,
                                                      std::string_view session_name) {
  if (!set_class) return std::nullopt;
  std::string error;
  const char* cname = bootstrap::client_class_name(*set_class);
  const auto cls = cname ? db.class_for_name(cname, error) : std::nullopt;
  if (!cls) {
    spdlog::error("[session] {}: the class {} is not in content.db ({}): the class stays", session_name, *set_class, error);
    return std::nullopt;
  }
  if (!pb.set_class(cls->guid)) {
    spdlog::error("[session] {}: the pawn does not take class {}: the class stays", session_name, cls->name);
    return std::nullopt;
  }
  return cls;
}

std::vector<weaponxp::Change> add_worn_weapon_xp(weaponxp::State& weapon, const std::vector<int64_t>& worn_lines, int64_t amount, bool& progress_changed) {
  std::vector<weaponxp::Change> changes;
  for (const int64_t type : worn_lines) {
    const weaponxp::Change c = weapon.add_xp(type, amount);
    if (c.changed()) changes.push_back(c);
  }
  if (!changes.empty()) progress_changed = true;
  return changes;
}

use::Context build_use_context(const PlayerWorld& world, int32_t class_id, const leveling::Progress& progress,
                               const std::pair<double, const std::optional<stats::CharacterStats>&>& xp_and_stats,
                               const std::pair<const professions::Professions&, std::vector<stats::WornItem>>& prof_and_worn) {
  use::Context ctx{class_id, progress, xp_and_stats.first, world.level_curve(), world.professions(), prof_and_worn.first, {}, world.currencies()};
  attach_profession_xp_runner(ctx, world.ability_catalog(), xp_and_stats.second ? &*xp_and_stats.second : nullptr, prof_and_worn.second);
  return ctx;
}

leveling::XpResult apply_use_adventuring_xp(const PlayerWorld& world, int64_t adventuring_xp, leveling::Progress& progress, bool& progress_changed,
                                            size_t& levels_gained) {
  leveling::XpResult xp{progress, progress, {}};
  if (adventuring_xp > 0 && world.level_curve()) {
    xp = leveling::add_xp(*world.level_curve(), world.auto_grants(), progress, adventuring_xp);
    progress = xp.after;
    progress_changed = true;
  }
  if (xp.leveled()) levels_gained += static_cast<size_t>(xp.after.level - xp.before.level);
  return xp;
}

std::optional<leveling::Progress> progress_if_use_changed(const leveling::Progress& progress, const leveling::XpResult& xp, bool new_class, bool profs_changed,
                                                          bool weapons_changed) {
  if (xp.changed() || xp.leveled() || new_class || profs_changed || weapons_changed) return progress;
  return std::nullopt;
}

std::pair<int, int> make_use_level_and_pawn_mode(const leveling::XpResult& xp, int level, bool new_class, bool weapon_pawn) {
  const int pawn_mode = (xp.leveled() || new_class) ? 2 : (weapon_pawn ? 1 : 0);
  return {xp.leveled() ? level : 0, pawn_mode};
}

template <typename ProfWires>
void queue_use_xp_tracker(const std::vector<bootstrap::ItemEvent>& events, std::optional<leveling::Progress> prog, const ProfWires& wire,
                          const std::vector<weaponxp::Change>& weapon_changes, const QueueStepFn& q) {
  for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
    const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                 events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
    q("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, true);
  }
  if (!prog) return;
  std::vector<std::pair<int64_t, int64_t>> lines;
  for (const weaponxp::Change& c : weapon_changes) lines.emplace_back(c.line, c.xp_after);
  q(
      "XPTracker",
      [p = *prog, wire, lines](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_progress(p.xp, p.debt)) return std::nullopt;
        for (const auto& w : wire) {
          if (!pb.set_profession(w.leveling_type, w.xp, w.certification)) return std::nullopt;
        }
        for (const auto& [type, line_total] : lines) {
          if (!pb.set_weapon_line(type, line_total)) return std::nullopt;
        }
        return pb.leveling_update();
      },
      true);
}

std::optional<repl::ActionbarPayload> cleared_bar_if(bool new_class, const std::optional<talents::Book>& book, repl::ActionbarPayload payload,
                                                     const actionbar::Bar& bar, const std::vector<std::pair<int, int>>& cleared_slots) {
  if (!new_class || !book) return std::nullopt;
  for (const auto& [bank, slot] : cleared_slots) {
    if (!bar.at(bank, slot)) payload.slots.push_back({bank, slot, {}});
  }
  return payload;
}

void queue_use_tail(std::pair<int, int> level_and_pawn, const std::optional<repl::ActionbarPayload>& bar, const std::vector<weaponxp::Change>& weapon_changes,
                    const std::vector<repl::SkillOnActionbar>& skill_updates, const QueueStepFn& q) {
  if (level_and_pawn.first > 0) {
    q(
        "BroadcastPlayLevelUpEffects",
        [lv = level_and_pawn.first](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, lv); }, false);
  }
  for (const weaponxp::Change& c : weapon_changes) {
    if (c.leveled()) q("BroadcastPlayLevelUpEffects", [c](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(c.line, c.level_after); }, false);
  }
  if (level_and_pawn.second != 0) {
    const bool full = level_and_pawn.second == 2;
    q("pawn state", [full](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update(full ? "level and stats" : "a weapon line"); }, full);
  }
  if (bar) q("ClientReceiveActionbarSlotPayload", [p = *bar](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(p); }, true);
  for (const repl::SkillOnActionbar& u : skill_updates) {
    q("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, true);
  }
}

const char* use_progress_reason(const leveling::XpResult& xp, bool class_changed, bool professions_changed,
                                const std::vector<weaponxp::Change>& weapon_changes) {
  if (!xp.changed() && !class_changed && !professions_changed && weapon_changes.empty()) return nullptr;
  const bool weapons_leveled = std::any_of(weapon_changes.begin(), weapon_changes.end(), [](const weaponxp::Change& c) { return c.leveled(); });
  if (xp.leveled() || weapons_leveled) return "a level-up";
  if (class_changed) return "a class change";
  return (professions_changed && !xp.changed()) ? "a profession changed" : "XP earned";
}

std::string skill_name_of(const talents::Book* book, const repl::SkillOnActionbar& u) {
  std::string skill = std::to_string(u.skill_id);
  if (!book) return skill;
  for (const talents::Tree* t : book->listed_trees()) {
    if (t->guid != u.skill_tree_id) continue;
    for (const talents::Node& n : t->nodes) {
      if (n.skill == u.skill_id) skill = n.skill_name;
    }
  }
  return skill;
}

std::string format_use_head(int64_t item_id, const use::Plan& plan, const leveling::XpResult& xp,
                            const std::pair<const professions::Professions&, std::span<const professions::Change>>& profs,
                            const std::pair<const std::optional<talents::Book>&, std::span<const repl::SkillOnActionbar>>& skills) {
  std::string line = "used item " + std::to_string(item_id) + ": " + plan.summary;
  if (xp.changed()) line += "; Adventuring " + std::to_string(xp.after.xp) + " XP (debt " + std::to_string(xp.after.debt) + ")";
  if (xp.leveled()) line += ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level);
  for (const professions::Change& c : profs.second) {
    const professions::Profession* pf = profs.first.book()->find(c.profession);
    if (!pf) continue;
    const professions::Book& pbook = *profs.first.book();
    line += "; " + pf->name + " level " + std::to_string(c.before.level) + " -> " + std::to_string(c.after.level) + ", " + pbook.tier(c.before.tier).name +
            " -> " + pbook.tier(c.after.tier).name + ", " + std::to_string(c.after.xp) + " XP";
  }
  std::string granted_names;
  for (const repl::SkillOnActionbar& u : skills.second) {
    granted_names += (granted_names.empty() ? "" : ", ") + skill_name_of(skills.first ? &*skills.first : nullptr, u);
  }
  if (!granted_names.empty()) line += ", granted " + granted_names;
  return line;
}

std::string format_use_tail(const use::Plan& plan, const std::pair<const weaponxp::State&, std::span<const int64_t>>& worn,
                            std::span<const weaponxp::Change> weap_changes, const std::optional<content::ClassRecord>& new_class, size_t storage_events) {
  std::string line;
  if (plan.weapon_xp > 0) {
    if (!worn.first.valid()) line += "; this world has no weapon lines";
    else if (worn.second.empty()) line += "; no weapon is worn, so no line gets it";
    bool first = true;
    for (const int64_t type : worn.second) {
      const weaponxp::Line* wl = worn.first.lines()->find(type);
      const bool up = std::any_of(weap_changes.begin(), weap_changes.end(), [&](const weaponxp::Change& c) { return c.line == type && c.leveled(); });
      line += (first ? "; " : ", ") + (wl ? wl->name : std::to_string(type)) + " " + std::to_string(worn.first.xp(type)) + " XP (level " +
              std::to_string(worn.first.level(type)) + (up ? ", level-up" : "") + ")";
      first = false;
    }
  }
  if (new_class && !new_class->name.empty()) {
    line += "; the class is " + new_class->name + " now (its tree, abilities and bar are sent live)";
  }
  line += "; " + std::to_string(storage_events) + " storage event(s)";
  for (const std::string& n : plan.notes) line += "; " + n;
  return line;
}

std::optional<svc::ItemContainer> resolve_session_container(const bootstrap::PlayerSetup& setup, const FIntrepidNetworkGUID& object) {
  const bootstrap::SessionGuids& g = setup.guids;
  if (object == g.backpack) return svc::ItemContainer::Backpack;
  if (object == g.equipment) return svc::ItemContainer::Equipment;
  if (object == g.quest_storage) return svc::ItemContainer::Quest;
  for (int i = 0; i < items::kResourceContainers; ++i) {
    if (setup.resource_bags[static_cast<size_t>(i)].present && object == g.resource_storage[static_cast<size_t>(i)]) return items::resource_container(i);
  }
  return std::nullopt;
}

items::Outcome apply_storage_destroy(items::Inventory& inv, const bootstrap::PlayerSetup& setup, const repl::ItemStorageEvent& event,
                                     svc::ItemContainer& where_out, svc::InventoryItem& before_out) {
  if (event.targets.size() != 1 || event.targets[0].slots.size() != 1) return {false, "a destroy names one container and one slot", {}};
  const auto where = resolve_session_container(setup, event.targets[0].object);
  if (!where) return {false, "a container that is none of this character's storages", {}};
  if (!is_bag(*where)) return {false, "only an item in a bag is destroyed (not a worn one, not a quest item)", {}};
  const repl::StorageEventSlot& slot = event.targets[0].slots[0];
  if (!(slot.item_guid == event.item.guid)) return {false, "the item of the event is not the instance of its slot", {}};
  const std::string guid = items::guid_hex(slot.item_guid);
  const svc::InventoryItem* held = inv.at(*where, slot.slot);
  if (!held || held->instance_guid != guid) return {false, "the slot does not hold that item", {}};
  where_out = *where;
  before_out = *held;
  return inv.consume(*where, slot.slot, guid, slot.count > 0 ? slot.count : before_out.quantity);
}

items::Outcome apply_storage_move_or_swap(items::Inventory& inv, const bootstrap::PlayerSetup& setup, const items::ItemCatalog& catalog,
                                          const repl::ItemStorageEvent& event, repl::StorageEventType type) {
  if (type != repl::StorageEventType::Move && type != repl::StorageEventType::Swap) {
    return {false, "an event of type " + std::to_string(event.type) + " is not made (only a Move and a Swap of whole stacks, and the destroy)", {}};
  }
  if (event.move_source.slots.size() != 1 || event.targets.size() != 1 || event.targets[0].slots.size() != 1) {
    return {false, "a move names one source slot and one target slot", {}};
  }
  const auto from = resolve_session_container(setup, event.move_source.object);
  const auto to = resolve_session_container(setup, event.targets[0].object);
  if (!from || !to) return {false, "a container that is none of this character's storages", {}};
  const repl::StorageEventSlot& src = event.move_source.slots[0];
  const repl::StorageEventSlot& dst = event.targets[0].slots[0];
  if (!(src.item_guid == event.item.guid)) return {false, "the item of the event is not the instance of its source slot", {}};
  const std::string guid = items::guid_hex(src.item_guid);
  return type == repl::StorageEventType::Move ? inv.move(*from, src.slot, src.count, guid, *to, dst.slot, &catalog)
                                              : inv.swap(*from, src.slot, src.count, guid, *to, dst.slot, items::guid_hex(dst.item_guid), dst.count);
}

void log_unreadable_item_rpc(bool& logged, std::string_view session_name, const char* rpc_name, size_t param_bits) {
  if (logged) return;
  logged = true;
  spdlog::warn("[session] {}: a {} does not read ({} parameter bits): ignored (further ones are not logged)", session_name, rpc_name, param_bits);
}

const svc::InventoryItem* find_usable_backpack_item(const items::Inventory& inv, svc::ItemContainer storage, const repl::ItemUse& use) {
  if (storage != svc::ItemContainer::Backpack) return nullptr;
  const svc::InventoryItem* item = inv.at(storage, use.slot);
  if (!item || item->instance_guid != items::guid_hex(use.expected_item)) return nullptr;
  return item;
}

bool is_activatable_backpack_item(const items::ItemCatalog& catalog, int64_t item_record_id) {
  const auto info = catalog.info(item_record_id);
  return !info || !info->wearable();
}

std::string item_display_or_name(const std::optional<content::ItemRecord>& record, int64_t fallback_id) {
  if (!record) return std::to_string(fallback_id);
  return !record->display_name.empty() ? record->display_name : record->name;
}

const svc::InventoryItem* validate_tier_up_slot_and_use(const items::Inventory& inv, const content::ContentDb* db, svc::ItemContainer container,
                                                        const repl::ItemUse& use, std::string& why_out) {
  const char* bag = items::container_name(container);
  const svc::InventoryItem* held = inv.at(container, use.slot);
  if (!held) {
    why_out = std::string("no item in ") + bag + " slot " + std::to_string(use.slot);
    return nullptr;
  }
  if (held->instance_guid != items::guid_hex(use.expected_item)) {
    why_out = std::string("the item in ") + bag + " slot " + std::to_string(use.slot) + " is not the instance the client named";
    return nullptr;
  }
  if (!db) {
    why_out = "worldd has no content.db to read the item's use from";
    return nullptr;
  }
  std::string error;
  const auto activation = db->item_use(held->item_record_id, error);
  if (!error.empty()) {
    why_out = "content.db failed: " + error;
    return nullptr;
  }
  if (!activation || activation->activation != "TierUpItem") {
    why_out = std::string("items of the ") + bag + " are used only to change a Glint stack into the next kind";
    return nullptr;
  }
  return held;
}

}  // namespace

bool PlayerSession::set_current_value(int64_t record, float value) {
  if (!stats_ || !stats_->started() || abilities_failed_ || !std::isfinite(value)) return false;
  const stats::StatValue* v = stats_->find(record);
  if (!v || !v->held() || !pb_.has_stat_item(record)) return false;
  bootstrap::AbilityChange c;
  c.stats.push_back({record, value, v->buckets(), v->base, v->equip, 0});
  if (!pb_.apply_ability_change(c)) return false;
  stats_->set_current(record, value);
  abilities_dirty_ = true;
  return true;
}

void PlayerSession::sync_ability_values() {
  if (pipeline_) pipeline_->sync_a7_values(pipeline_->now());
}

void PlayerSession::refresh_rank_mods() {
  if (stats_ && book_) {
    stats_->set_passives(book_->passives(wielded_line_));
    stats_->set_ranks(book_->held_ranks());
  }
  if (pipeline_ && book_) pipeline_->set_reaction_carriers(book_->reaction_carriers(wielded_line_));
  sync_ability_values();
}

std::vector<std::string> PlayerSession::weapon_tags() const {
  if (!world_) return {};
  const stats::GearCatalog* gear = world_->stat_context().gear;
  const svc::InventoryItem* held = inventory_.at(svc::ItemContainer::Equipment, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
  if (!gear || !held) return {};
  const stats::ItemGear* item = gear->item(held->item_record_id);
  return item ? item->tags : std::vector<std::string>{};
}

void PlayerSession::apply_gear_stats(const char* what) {
  if (!stats_ || !stats_->started()) return;
  refresh_rank_mods();
  if (pipeline_ && pipeline_->resource_class() != stats_->class_enum()) pipeline_->refresh(pipeline_->now(), "the class changed", false);
  std::vector<stats::StatValue> changed;
  stats::CharacterStats::Next next;
  std::string error;
  if (!stats_->prepare(bootstrap::worn_items_of(inventory_), changed, next, error)) {
    ++stat_updates_failed_;
    spdlog::error("[session] {}: the stats cannot be computed for {} ({}): the client keeps the stats it has", name(), what, error);
    return;
  }
  if (next.faults != 0) {
    ++stat_updates_failed_;
    spdlog::error(
        "[session] {}: the stat program faulted {} time(s) for {} (a division by zero, a bad curve read or a value out "
        "of range): "
        "its stats are not sent, the client keeps the stats it has",
        name(), next.faults, what);
    return;
  }
  if (changed.empty()) {
    stats_->commit(std::move(next));
    spdlog::debug("[session] {}: {} changes no stat", name(), what);
    return;
  }
  const auto runtime_before = abilities_ ? std::optional(abilities_->gear_snapshot()) : std::nullopt;
  bootstrap::AbilityChange change;
  const GearStatCounts counts = collect_gear_stat_changes(changed, pb_, abilities_ ? &*abilities_ : nullptr, gm_speed_, change);
  if (change.empty()) {
    stats_->commit(std::move(next));
    return;
  }
  if (!pb_.apply_ability_change(change)) {
    if (runtime_before) abilities_->restore_gear(*runtime_before);
    ++stat_updates_failed_;
    spdlog::error("[session] {}: the pawn does not take the stats of {} ({}): the client keeps the stats it has", name(), what,
                  pb_.error().empty() ? std::string("a stat has no item in its arrays") : pb_.error());
    return;
  }
  stats_->commit(std::move(next));
  ++stat_updates_;
  stat_items_changed_ += change.stats.size();
  spdlog::log(level_for(stat_logged_), "[session] {}: {} changes {} stat(s): {} Owner, {} OwnerProxy, {} EveryoneProxy{}", name(), what, change.stats.size(),
              counts.owner, counts.owner_proxy, counts.everyone,
              counts.without_item ? " (" + std::to_string(counts.without_item) + " of them new to the client's arrays: added)" : std::string());
}

double PlayerSession::xp_add_mult() const {
  if (!stats_) return 0;
  const stats::StatValue* v = stats_->find(content::kGuidAdventuringXPAddMult);
  return v && std::isfinite(v->value) ? v->value : 0.0;
}

std::string PlayerSession::fresh_guid() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  for (;;) {
    const uint64_t a = rng(), b = rng();
    const repl::GuidValue g{static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a), static_cast<uint32_t>(b >> 32), static_cast<uint32_t>(b)};
    if (!(g.a | g.b | g.c | g.d)) continue;
    const std::string hex = items::guid_hex(g);
    bool taken = false;
    for (const svc::InventoryItem& it : inventory_.items()) taken = taken || it.instance_guid == hex;
    if (!taken) return hex;
  }
}

void PlayerSession::finish_level_up(const leveling::XpResult& xp, bool class_changed, Clock::time_point now, bootstrap::AbilityChange& rank_effects,
                                    std::vector<repl::SkillOnActionbar>& skill_updates) {
  const bool leveled = xp.leveled();
  if (leveled) {
    if (!pb_.set_level(progress_.level)) spdlog::error("[session] {}: the pawn does not take level {}", name(), progress_.level);
    if (stats_) stats_->set_level(progress_.level);
    if (book_) {
      grant_ranks(collect_level_auto_ranks(*book_, xp.before.level, xp.after.level), now, rank_effects, skill_updates);
      sync_level_up_trees(*book_, pb_, name());
    }
  }
  if (leveled || class_changed) {
    apply_gear_stats(leveled && class_changed ? "the new level and class" : leveled ? "the new level" : "the new class");
  }
  if (leveled && !pb_.mark_temp_health()) spdlog::debug("[session] {}: no TempHealth item to mark", name());
  if (apply_or_rollback_rank_effects(pb_, rank_effects, rank_effects_, markers_, name())) abilities_dirty_ = true;
}

void PlayerSession::on_item_activated(Connection& conn, const repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now) {
  const auto refuse = [&](const std::string& why) {
    ++uses_refused_;
    refuse_item_request(conn, "ServerItemUsed (a use)", why, now);
  };
  const content::ContentDb* db = world_ ? world_->content() : nullptr;
  if (!db) return refuse("worldd has no content.db to read the item's actions from");
  const use::Context ctx = build_use_context(*world_, class_id_, progress_, {xp_add_mult(), stats_}, {professions_, bootstrap::worn_items_of(inventory_)});
  const use::Plan plan = use::plan_use(*db, item.item_record_id, ctx);
  if (!plan.ok) return refuse(plan.refusal);
  UseInvStep inv_step = make_use_inv_step(plan.consume, plan.items, [this] { return fresh_guid(); });
  if (const std::string why = check_use_inventory(inventory_, item_catalog(), use.slot, item.instance_guid, inv_step); !why.empty()) return refuse(why);
  const std::vector<int64_t> worn_lines = plan.weapon_xp > 0 ? equipped_weapon_lines() : std::vector<int64_t>();
  if (!room_for(13 + (plan.items.size() + 1) / bootstrap::kMaxEventsPerChain + world_->auto_grants().size() + plan.profession_changes.size() +
                worn_lines.size()))
    return;
  const items::Outcome all = apply_use_inventory(inventory_, item_catalog(), use.slot, item.instance_guid, {name(), inv_step});
  if (!all.ok) return refuse(all.refusal);
  if (!all.events.empty()) inventory_dirty_ = inventory_changed_ = true;
  const leveling::XpResult xp = apply_use_adventuring_xp(*world_, plan.adventuring_xp, progress_, progress_changed_, levels_gained_);
  const auto prof_changes = plan.profession_changes.empty() ? std::vector<professions::Change>() : set_professions(plan.professions_after);
  const auto new_class = resolve_use_class(*db, pb_, plan.set_class, name());
  bootstrap::AbilityChange rank_effects;
  std::vector<repl::SkillOnActionbar> skill_updates;
  std::vector<std::pair<int, int>> cleared_slots;
  if (new_class) {
    class_id_ = *plan.set_class;
    if (stats_) stats_->set_class(class_id_);
    progress_changed_ = true;
    if (book_) switch_class(*new_class, now, rank_effects, skill_updates, cleared_slots);
  }
  finish_level_up(xp, new_class.has_value(), now, rank_effects, skill_updates);
  const std::vector<weaponxp::Change> weapon_changes = add_worn_weapon_xp(weapon_, worn_lines, plan.weapon_xp, progress_changed_);
  const bool weapon_pawn = apply_weapon_levels(weapon_changes, now);
  ++uses_applied_;
  ++items_applied_;
  const QueueStepFn q = [&](const char* w, auto fn, bool must) { queue(w, std::move(fn), conn, must); };
  queue_use_xp_tracker(item_events_for(all), progress_if_use_changed(progress_, xp, new_class.has_value(), !prof_changes.empty(), !weapon_changes.empty()),
                       profession_wire(prof_changes), weapon_changes, q);
  queue_profession_level_ups(conn, prof_changes);
  queue_use_tail(make_use_level_and_pawn_mode(xp, progress_.level, new_class.has_value(), weapon_pawn),
                 cleared_bar_if(new_class.has_value(), book_, bar_payload(), bar_, cleared_slots), weapon_changes, skill_updates, q);
  spdlog::log(level_for(use_logged_), "[session] {}: {}{}", name(),
              format_use_head(item.item_record_id, plan, xp, {professions_, prof_changes}, {book_, skill_updates}),
              format_use_tail(plan, {weapon_, worn_lines}, weapon_changes, new_class, all.events.size()));
  if (const char* reason = use_progress_reason(xp, new_class.has_value(), !prof_changes.empty(), weapon_changes)) hand_over_progress(reason);
}

void PlayerSession::refuse_item_request(Connection& conn, const char* what, const std::string& why, Clock::time_point now) {
  ++items_refused_;
  spdlog::log(level_for(item_refused_logged_), "[session] {}: {} refused, nothing changed: {}", name(), what, why);
  if (!resync_budget_.take(now) || !room_for(5)) return;
  std::vector<bootstrap::StorageKind> kinds = {bootstrap::StorageKind::Backpack, bootstrap::StorageKind::Equipment};
  for (int i = 0; i < items::kResourceContainers; ++i) {
    if (pb_.setup().resource_bags[static_cast<size_t>(i)].present) kinds.push_back(bootstrap::resource_kind(i));
  }
  for (const bootstrap::StorageKind k : kinds) {
    queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
  }
}

void PlayerSession::on_item_used(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  const auto use = repl::parse_item_used(rpc.call);
  if (!use) {
    log_unreadable_item_rpc(item_unreadable_logged_, name(), "ServerItemUsed", rpc.call.param_bits);
    return;
  }
  if (!within_budget(item_budget_, "ServerItemUsed", now) || !room_for(8)) return;
  if (!catalog_) return refuse_item_request(conn, "ServerItemUsed", "worldd has no item catalog", now);
  const auto storage = container_of_location(use->storage);
  if (!storage) {
    if (const auto bag = material_bag_of_location(use->storage)) return on_tier_up(conn, *use, *bag, now);
    return refuse_item_request(conn, "ServerItemUsed", "items of storage " + std::to_string(use->storage) + " are not used", now);
  }
  if (const svc::InventoryItem* item = find_usable_backpack_item(inventory_, *storage, *use)) {
    if (mounts_ && mounts_->is_mount_item(item->item_record_id)) return on_mount_item_used(svc::InventoryItem(*item));
    if (is_activatable_backpack_item(*catalog_, item->item_record_id)) return on_item_activated(conn, *use, svc::InventoryItem(*item), now);
  }
  const items::Outcome outcome = inventory_.use(*storage, use->slot, items::guid_hex(use->expected_item), use->requested_equip_slot, *catalog_);
  if (!outcome.ok) {
    if (outcome.holds_items) send_economy_callout(conn, "CantUnequipResourceBag", now);
    return refuse_item_request(conn, "ServerItemUsed", outcome.refusal, now);
  }
  apply_outcome(conn, outcome, *storage == svc::ItemContainer::Backpack ? "an item is worn" : "an item is taken off");
}

void PlayerSession::on_tier_up(Connection& conn, const repl::ItemUse& use, svc::ItemContainer container, Clock::time_point now) {
  const char* what = "ServerItemUsed (a Glint tier change)";
  const auto refuse = [&](const std::string& why) {
    ++uses_refused_;
    refuse_item_request(conn, what, why, now);
  };
  const content::ContentDb* db = world_ ? world_->content() : nullptr;
  std::string why;
  const svc::InventoryItem* held = validate_tier_up_slot_and_use(inventory_, db, container, use, why);
  if (!held) return refuse(why);
  const items::CurrencyTable* currencies = world_->currencies();
  if (!currencies) return refuse("worldd has no currency tiers (rebuild content.db with this checkout's contentc): no stack changes kind");
  const items::CurrencyKind* kind = currencies->kind_of(held->item_record_id);
  const items::CurrencyKind* next = currencies->next_of(held->item_record_id);
  if (!kind) return refuse("item record " + std::to_string(held->item_record_id) + " is no kind of a currency");
  if (!next) return refuse("item record " + std::to_string(held->item_record_id) + " is the top kind of its currency: there is no next kind");
  std::string error;
  const auto product_record = db->item(next->item, error);
  if (!product_record)
    return refuse(error.empty() ? "the next kind, item record " + std::to_string(next->item) + ", is not in content.db" : "content.db failed: " + error);
  const auto stack_record = db->item(held->item_record_id, error);
  const int32_t pq = std::max(1, product_record->default_quality);
  const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
  const svc::InventoryItem product{container, 0, next->item, fresh_guid(), 1, pq, product_record->max_durability_at(pq), created};
  const svc::InventoryItem stack = *held;
  const items::Outcome outcome =
      inventory_.tier_up(container, use.slot, stack.instance_guid, static_cast<int32_t>(kind->rate), product, *product_record, item_catalog());
  if (!outcome.ok) return refuse(outcome.refusal);
  inventory_dirty_ = inventory_changed_ = true;
  ++uses_applied_;
  ++items_applied_;
  std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
  add_item_toasts(events, /*removals=*/false);
  queue(
      "ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn,
      /*must_build=*/true);
  const items::Event& arrival = outcome.events.back();
  const std::string from_name = item_display_or_name(stack_record, stack.item_record_id);
  const std::string to_name = item_display_or_name(product_record, next->item);
  spdlog::log(level_for(use_logged_), "[session] {}: a Glint tier change: {} x{} in {} slot {} became {} x1 in {} slot {}, {} storage event(s)", name(),
              from_name, stack.quantity, items::container_name(container), use.slot, to_name, items::container_name(arrival.container), arrival.slot,
              outcome.events.size());
}

void PlayerSession::on_storage_event_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  const auto event = repl::parse_storage_event(rpc.call);
  if (!event) {
    log_unreadable_item_rpc(item_unreadable_logged_, name(), "ServerRequestStorageEvent", rpc.call.param_bits);
    return;
  }
  if (warehouse_event(conn, *event, now)) return;
  if (!within_budget(item_budget_, "ServerRequestStorageEvent", now) || !room_for(4)) return;
  const char* what = "ServerRequestStorageEvent";
  const auto type = static_cast<repl::StorageEventType>(event->type);
  if (type == repl::StorageEventType::Remove) {
    svc::ItemContainer where = svc::ItemContainer::Backpack;
    svc::InventoryItem before;
    const items::Outcome outcome = apply_storage_destroy(inventory_, pb_.setup(), *event, where, before);
    if (!outcome.ok) return refuse_item_request(conn, what, outcome.refusal, now);
    inventory_dirty_ = inventory_changed_ = true;
    ++items_applied_;
    ++items_destroyed_;
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    add_item_toasts(events, /*removals=*/true);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
      const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                   events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
      queue(
          "ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn,
          /*must_build=*/true);
    }
    spdlog::log(level_for(item_applied_logged_), "[session] {}: an item is destroyed: {} x{} from {} slot {}", name(), before.item_record_id, before.quantity,
                where == svc::ItemContainer::Backpack ? "the item bag" : "a material bag", event->targets[0].slots[0].slot);
    return;
  }
  const items::Outcome outcome = apply_storage_move_or_swap(inventory_, pb_.setup(), item_catalog(), *event, type);
  if (!outcome.ok) return refuse_item_request(conn, what, outcome.refusal, now);
  apply_outcome(conn, outcome, type == repl::StorageEventType::Move ? "an item is moved in the bag" : "two items swap places in the bag");
}

void PlayerSession::on_combat_settings(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  constexpr std::string_view kPrefix = "CharacterInGameSettings.";
  std::vector<bootstrap::PropertyValue> values;
  if (rpc.call.decoded) {
    for (const std::string_view path : kCombatEchoPaths) {
      const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, path.substr(kPrefix.size()));
      if (!c) continue;
      const auto it = rpc.call.params.values.find(c->handle);
      if (it != rpc.call.params.values.end()) values.push_back({path, it->second});
    }
  }
  if (values.empty()) {
    spdlog::log(level_for(combat_empty_logged_), "[session] {}: the client's combat settings carry none of the {} values worldd echoes", name(),
                kCombatEchoPaths.size());
    return;
  }
  if (!within_budget(combat_budget_, "ServerUpdateAccountCombatSettings", now) || !room_for(1)) return;
  spdlog::log(level_for(combat_logged_), "[session] {}: the client's combat settings: echoing {} of the {} values worldd sends", name(), values.size(),
              kCombatEchoPaths.size());
  queue("combat settings echo", [values = std::move(values)](bootstrap::PlayerBootstrap& pb) { return pb.combat_settings_echo(values); }, conn);
}

void PlayerSession::on_storage_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  const auto it = rpc.call.params.values.find(1);
  const auto* container = rpc.call.decoded && it != rpc.call.params.values.end() ? it->second.get_if<FIntrepidNetworkGUID>() : nullptr;
  if (container && warehouse_payload(conn, *container, now)) return;
  std::optional<bootstrap::StorageKind> kind;
  if (container) {
    if (const auto resolved = resolve_session_container(pb_.setup(), *container)) {
      if (*resolved == svc::ItemContainer::Backpack) kind = bootstrap::StorageKind::Backpack;
      else if (*resolved == svc::ItemContainer::Equipment) kind = bootstrap::StorageKind::Equipment;
      else if (*resolved == svc::ItemContainer::Quest) kind = bootstrap::StorageKind::QuestStorage;
      else kind = bootstrap::resource_kind(static_cast<int>(*resolved) - static_cast<int>(svc::ItemContainer::Resource0));
    }
  }
  if (!kind) {
    spdlog::debug("[session] {}: ServerRequestStorageData names a container worldd has no payload for", name());
    return;
  }
  if (!within_budget(storage_budget_, "ServerRequestStorageData", now) || !room_for(1)) return;
  const bootstrap::StorageKind k = *kind;
  queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
}

}  // namespace worldd
}  // namespace aoc
