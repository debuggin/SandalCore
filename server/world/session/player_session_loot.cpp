// SPDX-License-Identifier: Apache-2.0
// PlayerSession world interactions, quiet client state requests, death, kill rewards, and corpse looting.

#include "combat/combat_world.h"
#include "content/content_db.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/storage_rpc.h"
#include "session/player_session.h"

namespace aoc::worldd {

namespace {

bool is_huntable_npc(const world::WorldActor& a, const PlayerWorld* npc_world) {
  return a.kind == world::ActorKind::Npc && a.npc.interact_def != 0 && npc_world && npc_world->gathering() &&
         npc_world->gathering()->hunt_kind(a.npc.interact_def);
}

void log_interact_vendor_open(const std::string& player_name, spdlog::level::level_enum level, const world::WorldActor& a, const vendor::GmVendor* def,
                              const std::string& shop_name) {
  if (!shop_name.empty()) {
    spdlog::log(level, "[session] {}: opens the shop {} of {}", player_name, shop_name, a.npc.name);
  } else if (def) {
    spdlog::log(level, "[session] {}: opens the window of {} ({}, {} listing(s) in {} tab(s))", player_name, def->label, def->name, def->listings, def->tabs);
  }
}

/// The instance guid of the stack in slot `slot` of the corpse `npc` (the window and the events agree on it; it is not
/// a guid the bag holds).
repl::GuidValue loot_guid(uint64_t npc, size_t slot) {
  uint64_t s = npc * 0x9E3779B97F4A7C15ull + slot * 0xBF58476D1CE4E5B9ull + 0x5851F42D4C957F2Dull;
  const auto next = [&s]() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };
  const uint64_t a = next(), b = next();
  return {static_cast<uint32_t>(a), static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(b), static_cast<uint32_t>(b >> 32) | 1u};
}

/// Whether an item record is a material or currency (the retail PvE death takes about half of each stack of those, not
/// the gear).
bool is_material(const content::ContentDb& db, int64_t record) {
  std::string err;
  const auto gear = db.item_gear(record, err);
  if (!gear) return false;
  for (const std::string& t : gear->tags) {
    std::string lower;
    for (const char c : t) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("item.resource", 0) == 0) return true;
  }
  return false;
}

std::vector<combat::LootStack> collect_death_material_loss(items::Inventory& inventory, const content::ContentDb* db, bool loss_on, items::Outcome& all) {
  std::vector<combat::LootStack> lost;
  all.ok = true;
  if (!db || !loss_on) return lost;
  for (const svc::InventoryItem& it : inventory.items()) {
    if ((it.container != svc::ItemContainer::Backpack && !items::is_resource(it.container)) || it.quantity <= 0 || !is_material(*db, it.item_record_id))
      continue;
    const int32_t take = it.quantity == 1 ? 1 : (it.quantity + 1) / 2;
    const items::Outcome c = inventory.consume(it.container, it.slot, it.instance_guid, take);
    if (!c.ok) continue;
    all.events.insert(all.events.end(), c.events.begin(), c.events.end());
    lost.push_back({.item = it.item_record_id, .quantity = take, .quality = it.quality});
  }
  return lost;
}

uint64_t drop_death_pile(PlayerWorld* npc_world, const bootstrap::PlayerSetup& setup, const GoodPosition& at, const std::vector<combat::LootStack>& lost,
                         const std::string& player_name) {
  if (lost.empty() || !npc_world || !npc_world->aftermath()) return 0;
  const std::array<uint32_t, 4>& g = setup.character_guid;
  std::string err;
  const uint64_t pile =
      npc_world->aftermath()->drop_pile(at.x, at.y, at.z, repl::GuidValue{g[0], g[1], g[2], g[3]}, setup.name, lost, npc_world->world_clock(), err);
  if (pile == 0) spdlog::warn("[session] {}: the pile of the death is not made: {} ({} stack(s) of materials are lost)", player_name, err, lost.size());
  return pile;
}

std::vector<weaponxp::Change> add_kill_weapon_xp(weaponxp::State& weapon, const std::vector<int64_t>& worn_lines, int64_t weapon_xp, bool& weapons_leveled,
                                                 std::vector<std::pair<int64_t, int64_t>>& line_totals) {
  std::vector<weaponxp::Change> changes;
  weapons_leveled = false;
  for (const int64_t type : worn_lines) {
    const weaponxp::Change c = weapon.add_xp(type, weapon_xp);
    if (!c.changed()) continue;
    weapons_leveled = weapons_leveled || c.leveled();
    changes.push_back(c);
    line_totals.emplace_back(c.line, c.xp_after);
  }
  return changes;
}

std::string format_kill_weapon_text(const weaponxp::State& weapon, const std::vector<int64_t>& worn_lines, const std::vector<weaponxp::Change>& weapon_changes,
                                    int64_t weapon_xp) {
  if (weapon_xp <= 0) return {};
  std::string text = ", weapon lines +" + std::to_string(weapon_xp) + " each";
  if (!weapon.valid()) return text + " (this world has no weapon lines)";
  if (worn_lines.empty()) return text + " (no weapon is worn)";
  for (const int64_t type : worn_lines) {
    const weaponxp::Line* line = weapon.lines()->find(type);
    const bool up = std::any_of(weapon_changes.begin(), weapon_changes.end(), [&](const weaponxp::Change& c) { return c.line == type && c.leveled(); });
    text += ", " + (line ? line->name : std::to_string(type)) + " " + std::to_string(weapon.xp(type)) + " XP (level " + std::to_string(weapon.level(type)) +
            (up ? ", level-up" : "") + ")";
  }
  return text;
}

svc::InventoryItem make_corpse_slot_item(uint64_t corpse_npc, size_t slot, const combat::LootStack& s, const std::optional<content::ItemRecord>& rec,
                                         int64_t created) {
  const int32_t qty = static_cast<int32_t>(std::min<int64_t>(s.quantity, 1 << 20));
  const float dur = rec ? rec->max_durability_at(s.quality) : 0.0f;
  return {svc::ItemContainer::Backpack, 0, s.item, items::guid_hex(loot_guid(corpse_npc, slot)), qty, s.quality, dur, created};
}

std::string loot_event_refusal(const combat::Corpse& corpse, const repl::ItemStorageEvent& e, const std::vector<size_t>& slots) {
  if (e.move_source.object.ObjectId != corpse.parts->container || e.move_source.slots.size() != 1) return "an event names another container";
  const repl::StorageEventSlot& s = e.move_source.slots[0];
  if (s.slot < 0 || static_cast<size_t>(s.slot) >= corpse.loot.size()) return "an event names a slot that is none";
  const size_t slot = static_cast<size_t>(s.slot);
  if (corpse.loot[slot].taken) return "a stack was taken already";
  if (!e.item.has_item || e.item.record_id != static_cast<uint64_t>(corpse.loot[slot].item) || !(e.item.guid == loot_guid(corpse.npc, slot))) {
    return "an event's item is not the stack of its slot";
  }
  if (std::find(slots.begin(), slots.end(), slot) != slots.end()) return "a slot twice";
  return {};
}

std::vector<size_t> select_loot_slots(const combat::Corpse& corpse, const std::vector<repl::ItemStorageEvent>& events, size_t& skipped,
                                      std::string& first_why) {
  std::vector<size_t> slots;
  skipped = 0;
  for (const repl::ItemStorageEvent& e : events) {
    const std::string why = loot_event_refusal(corpse, e, slots);
    if (!why.empty()) {
      ++skipped;
      if (first_why.empty()) first_why = why;
      continue;
    }
    slots.push_back(static_cast<size_t>(e.move_source.slots[0].slot));
  }
  if (slots.empty() && first_why.empty()) first_why = "no event can be taken";
  return slots;
}

std::vector<svc::InventoryItem> build_loot_grant_stacks(const combat::LootStack& stack, const std::optional<content::ItemRecord>& rec, int64_t created,
                                                        const std::function<std::string()>& mint_guid) {
  const int32_t max_stack = rec ? std::max(1, rec->max_stack_size) : 1;
  const float dur = rec ? rec->max_durability_at(stack.quality) : 0.0f;
  std::vector<svc::InventoryItem> fresh;
  for (int64_t left = stack.quantity; left > 0;) {
    const int32_t qty = static_cast<int32_t>(std::min<int64_t>(left, max_stack));
    std::string guid = mint_guid();
    for (const svc::InventoryItem& other : fresh) {
      if (other.instance_guid == guid) guid = mint_guid();
    }
    fresh.push_back({svc::ItemContainer::Backpack, 0, stack.item, std::move(guid), qty, stack.quality, dur, created});
    left -= qty;
  }
  return fresh;
}

struct LootTakeBatch {
  std::vector<bootstrap::LootRemoval> removed;
  items::Outcome all;
  size_t bag_full = 0;
};

LootTakeBatch take_loot_batch(combat::Aftermath& after, const content::ContentDb& db, items::Inventory& inventory, const items::ItemCatalog& catalog,
                              const std::tuple<uint64_t, std::span<const size_t>, std::function<std::string()>>& req) {
  LootTakeBatch batch;
  batch.all.ok = true;
  const auto& [npc, slots, mint_guid] = req;
  const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
  for (const size_t slot : slots) {
    const combat::LootStack* stack = after.take(npc, slot);
    if (!stack) continue;
    std::string err;
    const auto rec = db.item(stack->item, err);
    const items::Outcome got = inventory.grant(build_loot_grant_stacks(*stack, rec, created, mint_guid), catalog);
    if (!got.ok) {
      after.put_back(npc, slot);
      ++batch.bag_full;
      continue;
    }
    batch.removed.push_back({static_cast<int32_t>(slot), items::to_instance(make_corpse_slot_item(npc, slot, *stack, rec, created))});
    batch.all.events.insert(batch.all.events.end(), got.events.begin(), got.events.end());
  }
  return batch;
}

std::string check_open_loot_target(combat::Aftermath* after, const content::ContentDb* db, const world::ActorRegistry* registry, uint64_t& loot_actor,
                                   const GoodPosition& at) {
  if (!after || !db || !registry) return "the world has no loot";
  if (loot_actor == 0) return "no loot window is open";
  const combat::Corpse* corpse = after->corpse_of(loot_actor);
  const world::WorldActor* actor = registry->find(loot_actor);
  const bool ok = corpse && corpse->parts && actor;
  const bool close_enough = ok && std::hypot(actor->x - at.x, actor->y - at.y) <= PlayerSession::kInteractRangeCm;
  if (!close_enough) loot_actor = 0;
  return !ok ? "the corpse is gone" : (!close_enough ? "the corpse is too far" : "");
}

std::optional<repl::InteractRequest> parse_and_log_interact(const repl::ClientRpc& rpc, bool& unreadable_logged, const std::string& player_name) {
  const auto req = repl::parse_interact_request(rpc.call);
  if (!req && !unreadable_logged) {
    unreadable_logged = true;
    spdlog::warn("[session] {}: a ServerInteractWithInteractableComponent does not read ({} parameter bits): ignored (further ones are not logged)",
                 player_name, rpc.call.param_bits);
  }
  return req;
}

std::string check_interact_preconditions(const world::ActorRegistry* registry, const std::unique_ptr<ActorView>& view, bool is_mounted, uint64_t component_id,
                                         const world::WorldActor*& actor_out) {
  if (!registry || !view) return "the world has no actors";
  if (is_mounted) return "a rider does not interact (it is on its mount)";
  actor_out = registry->find_by_component(component_id);
  return actor_out ? std::string{} : "it is no component of an actor of this world";
}

std::string check_actor_awake_and_in_range(const ActorView& view, const world::WorldActor& a, const GoodPosition& at) {
  if (view.state_of(a.id) != world::ClientState::Awake) return "the client does not have that actor open";
  const double distance = std::hypot(a.x - at.x, a.y - at.y);
  return distance > PlayerSession::kInteractRangeCm ? "the actor is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away" : std::string{};
}

const combat::Corpse* find_interactable_corpse(PlayerWorld* npc_world, uint64_t component_id) {
  return (npc_world && npc_world->aftermath()) ? npc_world->aftermath()->corpse_by_interactable(component_id) : nullptr;
}

bool has_npc_dialogue_root(const world::WorldActor& a, bool offers_quests) {
  return a.kind == world::ActorKind::Npc && (a.npc.dialogue_root != 0 || offers_quests);
}

struct InteractVendorResolution {
  const vendor::GmVendor* def = nullptr;
  int64_t shop = 0;
  std::string shop_name;
  std::string refusal;
};

InteractVendorResolution resolve_interact_vendor(const PlayerWorld* pw, const world::WorldActor& a, int64_t npc_shop, const GoodPosition& at) {
  InteractVendorResolution res;
  const vendor::GmVendors* gm = pw ? pw->gm_vendors() : nullptr;
  res.def = (gm && a.npc.is_gm_vendor) ? gm->find(a.npc.vendor_guid) : nullptr;
  res.shop = (!res.def && a.kind == world::ActorKind::Npc) ? npc_shop : 0;
  const double shop_dist = std::hypot(a.x - at.x, a.y - at.y, a.z - at.z);
  if (res.shop != 0 && shop_dist >= vendor::kShopRangeCm) {
    res.refusal = "the shopkeeper is " + std::to_string(static_cast<int>(shop_dist / 100.0)) + " m away (a shop opens within 12 m)";
    return res;
  }
  if (!res.def && res.shop == 0) {
    res.refusal = "the actor " + a.npc.name + " has nothing to offer yet";
    return res;
  }
  if (res.shop != 0) {
    const auto* sdef = (pw && pw->town_shops()) ? pw->town_shops()->def(res.shop) : nullptr;
    res.shop_name = sdef ? sdef->name : std::to_string(res.shop);
  }
  return res;
}

struct ParsedActiveTarget {
  bool readable = false;
  uint64_t slot = 0;
  const FIntrepidNetworkGUID* object = nullptr;
};

ParsedActiveTarget parse_active_target_rpc(const repl::ClientRpc& rpc) {
  ParsedActiveTarget out;
  if (!rpc.call.decoded) return out;
  const auto slot = repl::rpcf::uint_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Slot"), 0);
  const repl::Value* named = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "InActiveTarget");
  out.object = named ? named->get_if<FIntrepidNetworkGUID>() : nullptr;
  if (!slot || (named && !out.object)) return out;
  out.readable = true;
  out.slot = *slot;
  return out;
}

FIntrepidNetworkGUID resolve_active_target_guid(const FIntrepidNetworkGUID* object, const FIntrepidNetworkGUID& own, const world::ActorRegistry* registry,
                                                bool& unknown_out) {
  unknown_out = false;
  if (!object || !object->is_valid()) return {};
  if (object->ObjectId == own.ObjectId) return own;
  if (const world::WorldActor* actor = registry ? registry->find(object->ObjectId) : nullptr) return actor->actor.guid;
  unknown_out = true;
  return {};
}

bool parse_close_loot_rpc(const repl::ClientRpc& rpc, const FIntrepidNetworkGUID*& container_out) {
  container_out = nullptr;
  if (!rpc.call.decoded) return false;
  const repl::Value* named = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "LootContainer");
  container_out = named ? named->get_if<FIntrepidNetworkGUID>() : nullptr;
  return !named || container_out != nullptr;
}

bool matches_open_loot_container(const FIntrepidNetworkGUID* container, const combat::Corpse* corpse) {
  return container && (!corpse || !corpse->parts || container->ObjectId == corpse->parts->container);
}

bool parse_map_marker_rpc(const repl::ClientRpc& rpc, repl::Vec3d& where_out) {
  where_out = repl::Vec3d{};
  if (!rpc.call.decoded) return false;
  const repl::Value* named = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Location");
  const repl::Vec3d* at = named ? named->get_if<repl::Vec3d>() : nullptr;
  if (named && !at) return false;
  if (at) where_out = *at;
  return true;
}

bool is_finite_vec3d(const repl::Vec3d& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool is_zero_vec3d(const repl::Vec3d& v) { return v.x == 0 && v.y == 0 && v.z == 0; }

leveling::XpResult apply_kill_adventuring_xp(const PlayerWorld* pw, int64_t adventuring_xp, leveling::Progress& progress, bool& progress_changed,
                                             size_t& levels_gained) {
  const leveling::LevelCurve* curve = pw ? pw->level_curve() : nullptr;
  const std::span<const content::AutoGrant> grants = pw ? pw->auto_grants() : std::span<const content::AutoGrant>();
  leveling::XpResult xp{progress, progress, {}};
  if (curve && adventuring_xp > 0) {
    xp = leveling::add_xp(*curve, grants, progress, adventuring_xp);
    progress = xp.after;
    progress_changed = true;
  }
  if (xp.leveled()) levels_gained += static_cast<size_t>(xp.after.level - xp.before.level);
  return xp;
}

std::optional<bootstrap::Send> build_kill_xp_tracker_send(bootstrap::PlayerBootstrap& pb, bool adventuring, const leveling::Progress& p,
                                                          const std::vector<std::pair<int64_t, int64_t>>& lines) {
  if (adventuring && !pb.set_progress(p.xp, p.debt)) return std::nullopt;
  for (const auto& [type, line_total] : lines) {
    if (!pb.set_weapon_line(type, line_total)) return std::nullopt;
  }
  return pb.leveling_update();
}

void log_kill_reward(const std::string& player_name, spdlog::level::level_enum level, const combat::KillReward& k,
                     const std::pair<leveling::XpResult, std::string>& xp_and_weapon) {
  const auto& [xp, weapon_text] = xp_and_weapon;
  spdlog::log(level, "[session] {}: killed {} (level {}): Adventuring +{} XP{}{} (now {} XP, debt {}){}", player_name, k.name.empty() ? k.blueprint : k.name,
              k.level, k.adventuring_xp, weapon_text, xp.leveled() ? ", level " + std::to_string(xp.after.level) : "", xp.after.xp, xp.after.debt,
              k.loot ? "; there is loot" : "");
}

const char* kill_handover_reason(const leveling::XpResult& xp, const std::vector<weaponxp::Change>& weapon_changes, bool weapons_leveled) {
  if (!xp.changed() && weapon_changes.empty()) return nullptr;
  return (xp.leveled() || weapons_leveled) ? "a level-up" : "XP earned";
}

std::optional<std::vector<repl::ItemStorageEvent>> parse_and_log_loot_events(const repl::ClientRpc& rpc, bool& unreadable_logged,
                                                                             const std::string& player_name) {
  const auto events = repl::parse_loot_events(rpc.call);
  if (!events && !unreadable_logged) {
    unreadable_logged = true;
    spdlog::warn("[session] {}: a ServerRequestLootStorageEvents does not read ({} parameter bits): ignored (further ones are not logged)", player_name,
                 rpc.call.param_bits);
  }
  return events;
}

std::string validate_loot_request_preconditions(const combat::Corpse& corpse, uint64_t pawn_id, const std::vector<repl::ItemStorageEvent>& events) {
  if (!corpse.is_pile && !corpse.rights.count(pawn_id)) return "the player may not take from it";
  if (events.empty() || events.size() > PlayerSession::kMaxLootTakes) return events.empty() ? "no events" : "too many events";
  return {};
}

}  // namespace

void PlayerSession::on_interact(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  if (dead_) return;
  const auto req = parse_and_log_interact(rpc, vendor_unreadable_logged_, name());
  if (!req || !within_budget(vendor_budget_, "ServerInteractWithInteractableComponent", now)) return;
  const auto ignore = [&](const std::string& why) {
    ++interact_ignored_;
    spdlog::log(level_for(interact_logged_), "[session] {}: an interaction with component {} is ignored: {}", name(), req->interactable.ObjectId, why);
  };
  const world::WorldActor* a = nullptr;
  if (const std::string why = check_interact_preconditions(registry_, view_, mounted(), req->interactable.ObjectId, a); !why.empty()) return ignore(why);
  using KindHandler = void (PlayerSession::*)(Connection&, const world::WorldActor&, Clock::time_point);
  static constexpr std::pair<world::ActorKind, KindHandler> kKindHandlers[] = {
      {world::ActorKind::Mount, &PlayerSession::on_mount_interact},     {world::ActorKind::Gatherable, &PlayerSession::on_gather_interact},
      {world::ActorKind::Station, &PlayerSession::on_station_interact}, {world::ActorKind::Shrine, &PlayerSession::on_shrine_interact},
      {world::ActorKind::Prop, &PlayerSession::on_prop_interact},
  };
  for (const auto& [kind, fn] : kKindHandlers) {
    if (a->kind == kind) return (this->*fn)(conn, *a, now);
  }
  const GoodPosition at = checker_.good();
  if (const std::string why = check_actor_awake_and_in_range(*view_, *a, at); !why.empty()) return ignore(why);
  if (is_huntable_npc(*a, npc_world_)) return on_hunt_interact(conn, *a, now);
  if (const combat::Corpse* corpse = find_interactable_corpse(npc_world_, req->interactable.ObjectId)) return open_loot(conn, *corpse, *a, now);
  if (has_npc_dialogue_root(*a, npc_offers_quests(*a)) && open_dialogue(conn, *a, now)) return;
  const InteractVendorResolution vres = resolve_interact_vendor(world_, *a, town_shop_of(*a), at);
  if (!vres.refusal.empty()) return ignore(vres.refusal);
  if (!room_for(1)) return;
  vendor_actor_ = a->id;
  vendor_def_ = vres.shop != 0 ? vres.shop : vres.def->guid;
  vendor_by_talk_ = false;
  ++vendor_opens_;
  const int64_t merch = vendor_def_;
  const FIntrepidNetworkGUID npc = a->actor.guid;
  log_interact_vendor_open(name(), level_for(vendor_logged_), *a, vres.def, vres.shop_name);
  queue("ClientOpenVendor", [merch, npc](bootstrap::PlayerBootstrap& pb) { return pb.open_vendor(merch, npc); }, conn, true);
}

void PlayerSession::on_set_active_target(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  ++target_calls_;
  const ParsedActiveTarget parsed = parse_active_target_rpc(rpc);
  if (!parsed.readable) {
    if (++quiet_unreadable_logged_ <= kLoggedPerKind)
      spdlog::warn("[session] {}: a ServerSetActiveTarget does not read ({} parameter bits): the target is not kept (further ones are not logged)", name(),
                   rpc.call.param_bits);
    return;
  }
  if (!within_budget(quiet_budget_, "ServerSetActiveTarget", now) || !room_for(1)) return;
  if (parsed.slot < 1 || parsed.slot > 2) {
    if (++quiet_refused_logged_ <= kLoggedPerKind)
      spdlog::info("[session] {}: ServerSetActiveTarget names target slot {}: only the slots 1 and 2 exist, ignored", name(), parsed.slot);
    return;
  }
  bool unknown = false;
  const FIntrepidNetworkGUID target = resolve_active_target_guid(parsed.object, pb_.setup().guids.pawn, registry_, unknown);
  if (unknown) {
    ++target_unknown_;
    if (++quiet_refused_logged_ <= kLoggedPerKind)
      spdlog::info("[session] {}: ServerSetActiveTarget names object {}, which is no actor the world knows: the slot is left empty", name(),
                   parsed.object->ObjectId);
  }
  FIntrepidNetworkGUID& held = active_targets_[static_cast<size_t>(parsed.slot - 1)];
  if (held == target) return;
  held = target;
  ++target_changes_;
  if (!pb_.set_active_targets(active_targets_)) {
    spdlog::warn("[session] {}: the pawn does not take CombatInfo.ActiveTargets: the client is not told", name());
    return;
  }
  queue_later("CombatInfo (target)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("active target"); }, conn, now, false);
  spdlog::log(level_for(target_logged_, 20), "[session] {}: target slot {} is {} now", name(), parsed.slot,
              target.is_valid() ? "object " + std::to_string(target.ObjectId) : std::string("empty"));
}

void PlayerSession::on_set_hover_target(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  ++hover_calls_;
  std::array<uint32_t, 4> words{};
  bool ok = rpc.call.decoded;
  constexpr const char* kWords[4] = {"InHoverTargetGuid.A", "InHoverTargetGuid.B", "InHoverTargetGuid.C", "InHoverTargetGuid.D"};
  for (size_t i = 0; ok && i < 4; ++i) {
    const auto w = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, kWords[i]), 0);
    ok = w && *w >= std::numeric_limits<int32_t>::min() && *w <= std::numeric_limits<uint32_t>::max();
    if (ok) words[i] = static_cast<uint32_t>(*w);
  }
  if (!ok) {
    if (++quiet_unreadable_logged_ <= kLoggedPerKind)
      spdlog::warn("[session] {}: a ServerSetHoverTargetGuid does not read ({} parameter bits): the hover target is not kept (further ones are not logged)",
                   name(), rpc.call.param_bits);
    return;
  }
  if (!within_budget(quiet_budget_, "ServerSetHoverTargetGuid", now) || !room_for(1)) return;
  if (words == hover_words_) return;
  hover_words_ = words;
  ++hover_changes_;
  if (!pb_.set_hover_target(words)) {
    spdlog::warn("[session] {}: the pawn does not take CombatInfo.HoverTargetGuid: the client is not told", name());
    return;
  }
  queue_later("CombatInfo (hover)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("hover target"); }, conn, now, false);
  const bool nothing = words == std::array<uint32_t, 4>{};
  spdlog::log(level_for(hover_logged_, 20), "[session] {}: points at {}", name(),
              nothing ? std::string("nothing") : fmt::format("{:08x}-{:08x}-{:08x}-{:08x}", words[0], words[1], words[2], words[3]));
}

void PlayerSession::on_set_pvp_flag(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  ++flag_calls_;
  const auto flag = rpc.call.decoded ? repl::rpcf::bool_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Flag"), false) : std::nullopt;
  if (!flag) {
    if (++quiet_unreadable_logged_ <= kLoggedPerKind)
      spdlog::warn("[session] {}: a ServerSetPvpFlag does not read ({} parameter bits): the flag is not changed (further ones are not logged)", name(),
                   rpc.call.param_bits);
    return;
  }
  if (!within_budget(quiet_budget_, "ServerSetPvpFlag", now) || !room_for(1)) return;
  if (*flag == pvp_flag_) return;
  pvp_flag_ = *flag;
  ++flag_changes_;
  if (!pb_.set_pvp_force_flag(pvp_flag_)) {
    spdlog::warn("[session] {}: the pawn does not take CombatInfo.bPvPForceFlag: the client is not told", name());
    return;
  }
  queue_later("CombatInfo (PvP flag)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("PvP flag"); }, conn, now, false);
  spdlog::log(level_for(flag_logged_, 20), "[session] {}: the PvP flag is {} (kept until the session ends)", name(), pvp_flag_ ? "on" : "off");
}

void PlayerSession::on_close_loot(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  (void)conn;
  const FIntrepidNetworkGUID* container = nullptr;
  if (!parse_close_loot_rpc(rpc, container)) {
    if (++quiet_unreadable_logged_ <= kLoggedPerKind)
      spdlog::warn("[session] {}: a ServerCloseLoot does not read ({} parameter bits): the loot window is not closed (further ones are not logged)", name(),
                   rpc.call.param_bits);
    return;
  }
  if (!within_budget(quiet_budget_, "ServerCloseLoot", now) || loot_actor_ == 0) return;
  combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
  const combat::Corpse* corpse = after ? after->corpse_of(loot_actor_) : nullptr;
  if (!matches_open_loot_container(container, corpse)) {
    if (++quiet_refused_logged_ <= kLoggedPerKind)
      spdlog::info("[session] {}: ServerCloseLoot names another container than the open loot window's: the window stays open", name());
    return;
  }
  const std::string closed = corpse ? corpse->name : std::string("a corpse that is gone");
  loot_actor_ = 0;
  ++loot_closes_;
  spdlog::log(level_for(close_logged_, 20), "[session] {}: closes the loot window of {}", name(), closed);
}

void PlayerSession::on_set_slots(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  (void)conn;
  if (!book_) return;
  ++slot_calls_;
  const auto slots = repl::parse_set_actionbar_slots(rpc.call);
  if (!slots) {
    if (++quiet_unreadable_logged_ <= kLoggedPerKind)
      spdlog::warn("[session] {}: a ServerSetActionbarMultiSlotUseData does not read ({} parameter bits): ignored (further ones are not logged)", name(),
                   rpc.call.param_bits);
    return;
  }
  if (slots->empty()) return;
  if (slots->size() > kMaxMultiSlots) {
    ++bar_refused_;
    if (++quiet_refused_logged_ <= kLoggedPerKind)
      spdlog::info("[session] {}: ServerSetActionbarMultiSlotUseData names {} slots, more than {}: refused, nothing changed", name(), slots->size(),
                   kMaxMultiSlots);
    return;
  }
  if (!within_budget(bar_budget_, "ServerSetActionbarMultiSlotUseData", now)) return;
  size_t stored = 0, refused = 0;
  for (const repl::ActionbarSlot& slot : *slots) {
    std::string why;
    if (!bar_.set(slot, &why)) {
      ++refused;
      ++bar_refused_;
      spdlog::log(level_for(bar_refused_logged_), "[session] {}: ServerSetActionbarMultiSlotUseData (bank {}, slot {}) refused, nothing changed: {}", name(),
                  slot.bank_index, slot.slot_index, why);
      continue;
    }
    if (slot.use.use_type == repl::ActionbarUseType::Skill && slot.use.skill_rank_id != 0 && slot.bank_index < actionbar::kClassBanks) {
      bar_.remove_other_copies(slot.use.skill_rank_id, slot.bank_index, slot.slot_index);
    }
    ++stored;
    ++bar_sets_;
  }
  spdlog::log(level_for(slots_logged_, 20), "[session] {}: the client set {} action bar slot(s) at once: {} stored, {} refused", name(), slots->size(), stored,
              refused);
  if (stored > 0) {
    progress_changed_ = true;
    hand_over_progress("the action bar");
  }
}

void PlayerSession::on_set_map_marker(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  (void)conn;
  ++marker_calls_;
  repl::Vec3d where{};
  if (!parse_map_marker_rpc(rpc, where)) {
    if (++quiet_unreadable_logged_ <= kLoggedPerKind)
      spdlog::warn("[session] {}: a ServerSetMapMarkerLocation does not read ({} parameter bits): the marker is not changed (further ones are not logged)",
                   name(), rpc.call.param_bits);
    return;
  }
  if (!is_finite_vec3d(where)) {
    if (++quiet_refused_logged_ <= kLoggedPerKind)
      spdlog::warn("[session] {}: ServerSetMapMarkerLocation names a location that is not a number: the marker is not changed", name());
    return;
  }
  if (!within_budget(quiet_budget_, "ServerSetMapMarkerLocation", now)) return;
  if (is_zero_vec3d(where)) {
    if (!map_marker_) return;
    map_marker_.reset();
    spdlog::log(level_for(marker_logged_, 20), "[session] {}: the map marker is taken away", name());
    return;
  }
  if (map_marker_ && *map_marker_ == where) return;
  map_marker_ = where;
  spdlog::log(level_for(marker_logged_, 20), "[session] {}: the map marker is at ({:.0f}, {:.0f}, {:.0f}) (kept until the session ends)", name(), where.x,
              where.y, where.z);
}

void PlayerSession::check_death(Connection& conn, Clock::time_point now) {
  if (dead_ || !stats_ || !stats_->started()) return;
  const stats::StatValue* h = stats_->find(content::kGuidHealth);
  if (!h || h->value > 0) return;
  die(conn, now);
}

void PlayerSession::die(Connection& conn, Clock::time_point now) {
  dead_ = true;
  ++deaths_;
  cancel_quick_make(&conn, "the player died");
  cancel_gather(&conn, "the player died", Interrupt::Death);
  if (pipeline_) pipeline_->set_dead(true);
  if (mounted()) dismount(conn, now, DismountWhy::PlayerDied);
  if (mounts_ && abilities_) mounts_->desummon(ability_seconds(now));
  const GoodPosition at = checker_.good();
  death_x_ = at.x;
  death_y_ = at.y;
  death_z_ = at.z;
  loot_actor_ = 0;
  vendor_actor_ = 0;
  vendor_by_talk_ = false;
  end_dialogue(conn, "the player died");
  set_current_value(content::kGuidMana, 0.0f);
  pb_.set_dead(true);
  queue("ClientSetLastDeathLocation", [at](bootstrap::PlayerBootstrap& pb) { return pb.death_location(at.x, at.y, at.z); }, conn, true);
  queue("ClientRestart (death)", [](bootstrap::PlayerBootstrap& pb) { return pb.death_restart(); }, conn, true);
  const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
  int64_t debt_added = 0;
  if (curve) {
    const leveling::Progress next = leveling::with_death_debt(*curve, progress_, 0.02);
    debt_added = next.debt - progress_.debt;
    progress_ = next;
    progress_changed_ = true;
  }
  items::Outcome all;
  const bool loss_on = !npc_world_ || npc_world_->aftermath_config().death_loss;
  const std::vector<combat::LootStack> lost = collect_death_material_loss(inventory_, world_ ? world_->content() : nullptr, loss_on, all);
  if (!all.events.empty()) {
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
    for (size_t at2 = 0; at2 < events.size(); at2 += bootstrap::kMaxEventsPerChain) {
      const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at2),
                                                   events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at2 + bootstrap::kMaxEventsPerChain)));
      queue("ClientReceiveStorageEvent (death)", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, true);
    }
  }
  queue("pawn state (death)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("death"); }, conn, true);
  if (curve) {
    const leveling::Progress p = progress_;
    queue("XPTracker (death)",
          [p](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> { return pb.set_progress(p.xp, p.debt) ? pb.leveling_update() : std::nullopt; },
          conn, true);
    hand_over_progress("a death");
  }
  pile_actor_ = drop_death_pile(npc_world_, pb_.setup(), at, lost, name());
  if (inventory_dirty_) hand_over_inventory("a death", false);
  spdlog::log(level_for(death_logged_), "[session] {}: died at ({:.0f}, {:.0f}, {:.0f}): experience debt +{} (now {}), {} stack(s) of materials lost{}", name(),
              at.x, at.y, at.z, debt_added, progress_.debt, lost.size(), pile_actor_ != 0 ? " into a pile where it fell" : "");
}

void PlayerSession::apply_kill(Connection& conn, const combat::KillReward& k, Clock::time_point now) {
  ++kills_rewarded_;
  const leveling::XpResult xp = apply_kill_adventuring_xp(world_, k.adventuring_xp, progress_, progress_changed_, levels_gained_);
  const bool leveled = xp.leveled();
  bootstrap::AbilityChange rank_effects;
  std::vector<repl::SkillOnActionbar> skill_updates;
  if (leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);
  const std::vector<int64_t> worn_lines = k.weapon_xp > 0 ? equipped_weapon_lines() : std::vector<int64_t>();
  bool weapons_leveled = false;
  std::vector<std::pair<int64_t, int64_t>> lines;
  const std::vector<weaponxp::Change> weapon_changes = add_kill_weapon_xp(weapon_, worn_lines, k.weapon_xp, weapons_leveled, lines);
  if (!weapon_changes.empty()) progress_changed_ = true;
  const bool weapon_pawn = apply_weapon_levels(weapon_changes, now);
  if (xp.changed() || !weapon_changes.empty()) {
    const bool adventuring = xp.changed();
    const leveling::Progress p = progress_;
    queue("XPTracker", [adventuring, p, lines](bootstrap::PlayerBootstrap& pb) { return build_kill_xp_tracker_send(pb, adventuring, p, lines); }, conn, true);
  }
  if (leveled) {
    queue("BroadcastPlayLevelUpEffects", [lv = progress_.level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, lv); },
          conn);
  }
  for (const weaponxp::Change& c : weapon_changes) {
    if (c.leveled()) queue("BroadcastPlayLevelUpEffects", [c](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(c.line, c.level_after); }, conn);
  }
  if (leveled || weapon_pawn) {
    const char* why = leveled ? "level and stats" : "a weapon line";
    queue("pawn state", [why](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update(why); }, conn, leveled);
  }
  for (const repl::SkillOnActionbar& u : skill_updates) {
    queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, true);
  }
  log_kill_reward(name(), level_for(use_logged_), k, {xp, format_kill_weapon_text(weapon_, worn_lines, weapon_changes, k.weapon_xp)});
  if (const char* handover = kill_handover_reason(xp, weapon_changes, weapons_leveled)) hand_over_progress(handover);
  quest_kill(conn, k, now);
}

void PlayerSession::open_loot(Connection& conn, const combat::Corpse& corpse, const world::WorldActor& actor, Clock::time_point now) {
  const auto ignore = [&](const std::string& why) {
    ++loot_refused_;
    spdlog::log(level_for(loot_logged_), "[session] {}: the loot of {} is not opened: {}", name(), corpse.name, why);
  };
  if (!within_budget(loot_budget_, "a loot interaction", now)) return;
  if (!corpse.parts || corpse.remaining() == 0) return ignore("there is nothing left in it");
  if (!corpse.is_pile && !corpse.rights.count(pb_.setup().guids.pawn.ObjectId)) return ignore("the player did not fight it");
  const GoodPosition at = checker_.good();
  if (std::hypot(actor.x - at.x, actor.y - at.y) > kInteractRangeCm)
    return ignore("it is " + std::to_string(static_cast<int>(std::hypot(actor.x - at.x, actor.y - at.y) / 100.0)) + " m away");
  const content::ContentDb* db = world_ ? world_->content() : nullptr;
  if (!db || !room_for(2)) return;
  repl::StoragePayload data;
  data.maximum_items = static_cast<int32_t>(corpse.loot.size());
  data.client_retrieve_only = true;
  const repl::GuidValue me{pb_.setup().character_guid[0], pb_.setup().character_guid[1], pb_.setup().character_guid[2], pb_.setup().character_guid[3]};
  const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
  for (size_t i = 0; i < corpse.loot.size(); ++i) {
    repl::StorageSlot slot;
    const combat::LootStack& s = corpse.loot[i];
    if (!s.taken) {
      std::string err;
      slot.item = items::to_instance(make_corpse_slot_item(corpse.npc, i, s, db->item(s.item, err), created));
      slot.slot = static_cast<int32_t>(i);
      slot.check_loot_permission = true;
      slot.loot_permission.push_back(me);
    }
    data.storage.push_back(std::move(slot));
  }
  loot_actor_ = corpse.npc;
  ++loot_opens_;
  spdlog::log(level_for(loot_logged_), "[session] {}: opens the loot of {} ({} stack(s))", name(), corpse.name, corpse.remaining());
  queue("ClientReceiveLootStoragePayload", [ag = actor.actor.guid, data](bootstrap::PlayerBootstrap& pb) { return pb.loot_payload(ag, data); }, conn, true);
}

void PlayerSession::on_loot_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  const auto events = parse_and_log_loot_events(rpc, vendor_unreadable_logged_, name());
  if (!events || !within_budget(loot_budget_, "ServerRequestLootStorageEvents", now)) return;
  const auto refuse = [&](const std::string& why) {
    ++loot_refused_;
    spdlog::log(level_for(loot_logged_), "[session] {}: a loot request is refused: {}", name(), why);
  };
  combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
  const content::ContentDb* db = world_ ? world_->content() : nullptr;
  if (const std::string why = check_open_loot_target(after, db, registry_, loot_actor_, checker_.good()); !why.empty()) return refuse(why);
  const combat::Corpse* corpse = after->corpse_of(loot_actor_);
  const world::WorldActor* actor = registry_->find(loot_actor_);
  if (const std::string why = validate_loot_request_preconditions(*corpse, pb_.setup().guids.pawn.ObjectId, *events); !why.empty()) return refuse(why);
  if (!room_for(4)) return;
  size_t skipped = 0;
  std::string first_why;
  const std::vector<size_t> slots = select_loot_slots(*corpse, *events, skipped, first_why);
  if (slots.empty()) return refuse(first_why);
  if (skipped > 0) {
    spdlog::log(level_for(loot_logged_), "[session] {}: {} event(s) of the loot request are skipped ({}); the other {} are taken", name(), skipped, first_why,
                slots.size());
  }
  const LootTakeBatch batch = take_loot_batch(*after, *db, inventory_, item_catalog(), {corpse->npc, slots, [this] { return fresh_guid(); }});
  loot_taken_ += batch.removed.size();
  if (batch.bag_full > 0) {
    spdlog::log(level_for(loot_logged_), "[session] {}: the bag has no room for {} stack(s): they stay in the corpse", name(), batch.bag_full);
  }
  if (batch.removed.empty()) return;
  inventory_dirty_ = true;
  inventory_changed_ = true;
  ++items_applied_;
  queue("ClientReceiveStorageEvent (loot)",
        [cg = corpse->parts->container_guid, rem = batch.removed](bootstrap::PlayerBootstrap& pb) { return pb.loot_removed(cg, rem); }, conn, true);
  std::vector<bootstrap::ItemEvent> adds = item_events_for(batch.all);
  add_item_toasts(adds, false);
  for (size_t at2 = 0; at2 < adds.size(); at2 += bootstrap::kMaxEventsPerChain) {
    const std::vector<bootstrap::ItemEvent> part(adds.begin() + static_cast<std::ptrdiff_t>(at2),
                                                 adds.begin() + static_cast<std::ptrdiff_t>(std::min(adds.size(), at2 + bootstrap::kMaxEventsPerChain)));
    queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, true);
  }
  spdlog::log(level_for(loot_logged_), "[session] {}: takes {} stack(s) from {}: {} storage event(s) into the bag", name(), batch.removed.size(), corpse->name,
              batch.all.events.size());
  if (corpse->remaining() == 0) {
    queue("ClientRemoveListenerFromStorage", [ag = actor->actor.guid](bootstrap::PlayerBootstrap& pb) { return pb.loot_listener_removed(ag); }, conn, true);
    after->emptied(corpse->npc, npc_world_->world_clock());
    loot_actor_ = 0;
  }
}

}  // namespace aoc::worldd
