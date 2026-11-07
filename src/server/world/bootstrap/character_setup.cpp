// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "bootstrap/character_setup.h"

#include "bootstrap/captured_abilities.h"
#include "bootstrap/captured_setup.h"
#include "items/wallet.h"
#include "protocol/repl/codecs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <set>

namespace aoc::worldd::bootstrap {

namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void add_item_records(std::vector<AbilityRecord>& records, std::span<const int64_t> abilities) {
    const std::span<const int64_t> tail = static_known_tail();
    size_t at = records.size();
    if (records.size() >= tail.size()) {
        bool ends_with_tail = true;
        for (size_t i = 0; ends_with_tail && i < tail.size(); ++i) ends_with_tail = records[records.size() - tail.size() + i].guid == tail[i];
        if (ends_with_tail) at = records.size() - tail.size();
    }
    for (const int64_t a : abilities) {
        const bool known = std::any_of(records.begin(), records.end(), [&](const AbilityRecord& r) { return r.guid == a; });
        if (a == 0 || known) continue;
        records.insert(records.begin() + static_cast<std::ptrdiff_t>(at++), AbilityRecord{a, content::kTypeAbilityRecord});
    }
}

StatItem make_stat_item(int32_t id, const stats::StatValue& v) {
    StatItem it;
    it.id = id;
    it.record = v.guid;
    it.value = std::bit_cast<uint32_t>(v.value);
    it.extended = v.buckets();
    if (it.extended) {
        it.base = std::bit_cast<uint32_t>(v.base);
        it.equipment = std::bit_cast<uint32_t>(v.equip);
    }
    return it;
}

void fill_stat_array(const StatArray& from, uint8_t mask, const std::vector<stats::StatValue>& values,
                     const std::map<int64_t, const stats::StatValue*>& by_guid, StatArray& to) {
    std::set<int64_t> placed;
    int32_t max_id = 0;
    for (const StatItem& it : from.items) max_id = std::max(max_id, it.id);
    for (const StatItem& it : from.items) {
        const auto found = by_guid.find(it.record);
        if (found == by_guid.end() || found->second->replication != mask || found->second->float_array()) continue;
        to.items.push_back(make_stat_item(it.id, *found->second));
        placed.insert(it.record);
    }
    for (const stats::StatValue& v : values) {
        if (v.replication != mask || v.float_array() || placed.count(v.guid)) continue;
        to.items.push_back(make_stat_item(++max_id, v));
        placed.insert(v.guid);
    }
    to.array_key = from.array_key;
}

bool validate_character_identity(const svc::Character& ch, const content::ContentDb& content, PlayerSetup& s, CharacterSetupResult& out) {
    const auto fail = [&](std::string msg, SetupRefusal refusal = SetupRefusal::Other) {
        out.error = std::move(msg);
        out.refusal = refusal;
        return false;
    };
    const auto words = character_guid_words(ch.id);
    if (!words) return fail("the character id '" + ch.id + "' is not 32 hex digits");
    s.character_guid = *words;
    if (ch.name.empty()) return fail("the character record has no name");
    if (!repl::fstring_fits(ch.name)) return fail("the character's name cannot be sent (not UTF-8 text, a NUL, or too long)");
    s.name = ch.name;

    const auto race = client_race_from_id(ch.race_id);
    if (!race) {
        return fail("race id " + std::to_string(ch.race_id) + " is not a playable race (0 Kaelar to 7 Nikua; 8 Tulnar cannot be created)",
                    SetupRefusal::UnsupportedRace);
    }
    const auto gender = client_gender_from_id(ch.gender_id);
    if (!gender) return fail("gender id " + std::to_string(ch.gender_id) + " is not 0 (Male) or 1 (Female)");
    s.race = static_cast<uint8_t>(*race);
    s.gender = static_cast<uint8_t>(*gender);

    if (ch.level < content::kMinLevel || ch.level > content::kMaxLevel) {
        return fail("the character's level " + std::to_string(ch.level) + " is outside content.db's levels " +
                    std::to_string(content::kMinLevel) + " to " + std::to_string(content::kMaxLevel));
    }
    s.level = ch.level;

    const char* class_name = client_class_name(ch.class_id);
    if (!class_name) {
        return fail("class id " + std::to_string(ch.class_id) + " is not a class (1 Ranger to 8 Mage)" +
                    (ch.class_id == 0 ? ": the character service could not derive it for this record" : ""));
    }
    std::string content_error;
    const auto cls = content.class_for_name(class_name, content_error);
    if (!cls) return fail(content_error);
    if (!ch.class_name.empty() && ch.class_name != cls->name) {
        out.notes.push_back("the record's class name '" + ch.class_name + "' is not '" + cls->name + "', the name of class " +
                            std::to_string(ch.class_id) + ": the class id decides");
    }
    if (ch.archetype_id != 0 && ch.archetype_id != cls->archetype_id) {
        out.notes.push_back("the record's archetype id " + std::to_string(ch.archetype_id) + " is not " + std::to_string(cls->archetype_id) +
                            ", the archetype of " + cls->name + ": the class id decides");
    }
    s.class_record_guid = cls->guid;
    return true;
}

bool setup_character_inventory(const svc::Character& ch, const content::ContentDb& content, PlayerSetup& s, CharacterSetupResult& out) {
    std::string items_error;
    const items::ContentItemCatalog item_catalog(content);
    auto inventory = items::Inventory::from_items(ch.items, items_error, &item_catalog);
    if (!inventory) {
        out.error = "the character's items cannot be played: " + items_error;
        return false;
    }
    const items::ResourceRepair repair = inventory->repair_resources(item_catalog);
    if (!repair.moved.empty() || !repair.stayed.empty()) {
        out.items_repaired = !repair.moved.empty();
        out.notes.push_back("resource items in the Backpack (the harvests of Gathering stage 2): " + std::to_string(repair.moved.size()) +
                            " moved to the resource containers, " + std::to_string(repair.stayed.size()) + " stay in the Backpack for want of room");
    }
    for (const svc::InventoryItem& it : inventory->items()) {
        StoredItem stored;
        stored.storage = item_storage_of(it.container);
        stored.slot = it.slot;
        stored.item = items::to_instance(it);
        s.items.push_back(std::move(stored));
    }
    for (int i = 0; i < items::kResourceContainers; ++i) {
        const items::ResourceGrid& grid = inventory->grid(items::resource_container(i));
        ResourceBagSetup& bag = s.resource_bags[static_cast<size_t>(i)];
        if (!grid.empty()) {
            bag.present = true;
            bag.columns = grid.columns();
            bag.rows = grid.rows();
            bag.override_matrix = grid.override_matrix();
        } else if (i != 0) {
            bag = ResourceBagSetup{};
        } else if (inventory->size() > 0) {
            out.notes.push_back("no resource bag in Equipment slot 23: Resource0 is the starter's empty 6 x 4 container and holds nothing");
        }
    }
    if (inventory->count(svc::ItemContainer::Equipment) > 0) {
        s.held_items.array_key = kInitialHeldItemsKey;
        s.held_items.items = held_items_for(*inventory);
    }
    s.appearance_ids = items::appearance_ids(*inventory);
    for (const svc::WalletEntry& e : items::Wallet(ch.wallet).entries()) s.currencies.push_back({e.currency_guid, e.amount});
    if (inventory->size() > 0) {
        out.notes.push_back("items: " + std::to_string(inventory->count(svc::ItemContainer::Backpack)) + " in the bag, " +
                            std::to_string(inventory->count(svc::ItemContainer::Equipment)) + " worn, " +
                            std::to_string(inventory->count(svc::ItemContainer::Quest)) + " quest items, " +
                            std::to_string(inventory->count(svc::ItemContainer::Resource0)) + "/" +
                            std::to_string(inventory->count(svc::ItemContainer::Resource1)) + "/" +
                            std::to_string(inventory->count(svc::ItemContainer::Resource2)) + " in the resource containers");
    }
    out.inventory = std::move(inventory);
    return true;
}

std::vector<const content::AutoGrant*> setup_character_progress(const svc::Character& ch, const StatContext& stat_context, PlayerSetup& s,
                                                                CharacterSetupResult& out) {
    std::vector<const content::AutoGrant*> granted;
    if (stat_context.curve) {
        std::optional<int64_t> xp;
        int64_t debt = 0;
        for (const svc::Experience& e : ch.experience) {
            if (e.leveling_type_guid == content::kGuidAdventuring) {
                xp = e.xp;
                debt = e.debt;
            }
        }
        out.progress = leveling::progress_of(*stat_context.curve, ch.level, xp, debt);
        if (ch.level > out.progress.level && ch.level <= content::kMaxLevel) {
            out.progress.level = ch.level;
        }
        if (xp && *xp < stat_context.curve->xp_for(ch.level)) {
            out.notes.push_back("the record's Adventuring XP " + std::to_string(*xp) + " is below level " + std::to_string(ch.level) +
                                "'s threshold: the threshold is used");
        }
        if (stat_context.grants) granted = leveling::granted_by(*stat_context.grants, out.progress.level);
        s.progress.xp = out.progress.xp;
        s.progress.debt = out.progress.debt;
        if (!stat_context.talents && !granted.empty()) {
            TreeSetup universal;
            universal.tree = content::kGuidUniversalTree;
            for (const content::AutoGrant* g : granted) universal.ranks.push_back(g->rank_guid);
            s.progress.trees.push_back(std::move(universal));
        }
        if (!granted.empty()) out.notes.push_back(std::to_string(granted.size()) + " rank(s) of the Universal tree granted by the level");
    } else {
        out.progress.level = ch.level;
    }

    if (stat_context.professions) {
        std::vector<std::string> corrected;
        out.professions = professions::Professions::load(*stat_context.professions, ch.professions, &corrected);
        for (std::string& n : corrected) out.notes.push_back(std::move(n));
        for (const professions::Profession& p : stat_context.professions->all()) {
            const professions::State st = out.professions.state(p.guid);
            s.progress.professions.push_back({p.leveling_type, st.xp, stat_context.professions->tier(st.tier).guid});
        }
    }
    if (stat_context.weapon_lines) {
        std::vector<std::string> corrected;
        out.weapon = weaponxp::State::load(*stat_context.weapon_lines, ch.experience, &corrected);
        for (std::string& n : corrected) out.notes.push_back(std::move(n));
        for (const auto& [type, xp] : out.weapon.with_xp()) s.progress.weapon_lines.push_back({type, xp});
    }
    return granted;
}

void setup_character_action_bar(const svc::Character& ch, const talents::Book& book, CharacterSetupResult& out) {
    if (ch.action_bar.empty()) {
        out.action_bar = actionbar::Bar::defaults({content::kGuidGuard, content::kGuidSprint, content::kGuidDodge});
        if (const talents::Tree* ct = book.class_tree()) {
            for (const talents::RankRef& r : book.listed(*ct)) {
                if (r.node->auto_hotbar && r.rank->ability != 0) out.action_bar.place_skill(r.rank->ability, r.rank->guid);
            }
        }
        return;
    }
    for (const svc::ActionbarSlot& a : ch.action_bar) {
        if (a.use_type < 0 || a.use_type > 255 || a.instigator < 0 || a.instigator > 255 || a.item_rarity < 0 || a.item_rarity > 255 ||
            a.combo_button < 0 || a.combo_button > 255) {
            out.notes.push_back("a stored action bar slot is left out: a value is outside the byte the wire holds");
            continue;
        }
        repl::ActionbarSlot slot;
        slot.bank_index = a.bank;
        slot.slot_index = a.slot;
        slot.use.use_type = static_cast<uint8_t>(a.use_type);
        slot.use.instigator = static_cast<uint8_t>(a.instigator);
        slot.use.ability_id = a.ability_id;
        slot.use.skill_rank_id = a.skill_rank_id;
        slot.use.pet_command_id = a.pet_command_id;
        slot.use.item_record_id = a.item_record_id;
        if (const auto w = character_guid_words(a.item_guid)) {
            slot.use.item_guid = repl::GuidValue{(*w)[0], (*w)[1], (*w)[2], (*w)[3]};
        }
        slot.use.item_rarity = static_cast<uint8_t>(a.item_rarity);
        slot.use.item_quality = a.item_quality;
        slot.use.combo_button = static_cast<uint8_t>(a.combo_button);
        std::string why;
        if (!out.action_bar.set(slot, &why)) out.notes.push_back("a stored action bar slot is left out: " + why);
    }
}

bool setup_character_talents(const svc::Character& ch, const content::ContentDb& content, const StatContext& stat_context, PlayerSetup& s,
                             CharacterSetupResult& out) {
    const items::Inventory& inventory = *out.inventory;
    if (!stat_context.talents) {
        add_item_records(s.records_to_load, carried_item_abilities(stat_context.mounts, inventory));
        return true;
    }
    talents::Book book(*stat_context.talents, s.class_record_guid, out.progress.level);
    if (out.weapon.valid()) book.set_line_levels(out.weapon.levels());
    size_t dropped = 0;
    for (const svc::TreeRanks& t : ch.skill_ranks) book.load(t.tree_guid, t.rank_guids, &dropped);
    if (dropped > 0) {
        out.notes.push_back(std::to_string(dropped) + " stored rank(s) are no rank of a tree the character can have (content.db changed?): left out");
    }
    s.progress.trees.clear();
    for (const talents::Tree* t : book.listed_trees()) s.progress.trees.push_back({t->guid, book.list(*t)});
    std::string weapon_error;
    const auto primary = worn_weapon(content, inventory, items::equip::Primary, &weapon_error);
    if (!weapon_error.empty()) {
        out.inventory.reset();
        out.error = weapon_error;
        return false;
    }
    if (primary) {
        s.weapon_skill_line.guid = primary->line;
        s.weapon_skill_line.name = primary->line_name;
        out.wielded_line = primary->line;
    }
    const std::vector<int64_t> item_abilities = carried_item_abilities(stat_context.mounts, inventory, &content);
    const talents::AbilityLists lists = talents::join_abilities(book, general_unstuck(), primary ? primary->combo : 0,
                                                                carried_combos(content, inventory), static_known_tail(), item_abilities);
    s.known_abilities = lists.known;
    s.records_to_load.clear();
    for (const int64_t g : lists.records) s.records_to_load.push_back({g, content::kTypeAbilityRecord});
    setup_character_action_bar(ch, book, out);
    out.book = std::move(book);
    return true;
}

bool setup_character_stats(const svc::Character& ch, const StatContext& stat_context, std::span<const content::AutoGrant* const> granted,
                           PlayerSetup& s, CharacterSetupResult& out) {
    stats::CharacterStats character_stats(*stat_context.bundle, *stat_context.gear, ch.class_id, ch.level, stat_context.rounding);
    if (out.book) {
        character_stats.set_passives(out.book->passives(out.wielded_line));
        character_stats.set_ranks(out.book->held_ranks(out.wielded_line));
    } else {
        character_stats.set_passives(rank_effects_of(granted));
    }
    const std::vector<stats::WornItem> worn = worn_items_of(*out.inventory);
    std::string stats_error;
    if (!character_stats.start(worn, stats_error)) {
        out.inventory.reset();
        out.error = "the character's stats cannot be computed: " + stats_error;
        return false;
    }
    if (character_stats.faults() != 0) {
        out.inventory.reset();
        out.error = "the stat program faulted " + std::to_string(character_stats.faults()) +
                    " time(s) for this character (a division by zero or a bad curve read)";
        return false;
    }
    auto layout = stats_setup_for(character_stats.values(), captured_bard_level1_stats(), stats_error);
    if (!layout) {
        out.inventory.reset();
        out.error = "the character's stats cannot be laid out: " + stats_error;
        return false;
    }
    s.stats = std::move(*layout);
    if (character_stats.unknown_items() != 0) {
        out.notes.push_back(std::to_string(character_stats.unknown_items()) + " worn item(s) are not in content.db: they add nothing to the stats");
    }
    if (character_stats.unknown_sets() != 0) {
        out.notes.push_back(std::to_string(character_stats.unknown_sets()) + " set bonus(es) of the worn items are not in content.db: they add nothing");
    }
    out.character_stats = std::move(character_stats);
    return true;
}

}  // namespace

std::optional<std::array<uint32_t, 4>> character_guid_words(std::string_view hex32) {
    if (hex32.size() != 32) return std::nullopt;
    std::array<uint32_t, 4> w{};
    for (size_t i = 0; i < 32; ++i) {
        const int d = hex_digit(hex32[i]);
        if (d < 0) return std::nullopt;
        w[i / 8] = (w[i / 8] << 4) | static_cast<uint32_t>(d);
    }
    return w;
}

int64_t general_unstuck() { return captured::kKnownAbilities[0]; }

std::span<const int64_t> static_known_tail() {
    constexpr size_t kTail = 45;
    constexpr size_t n = sizeof(captured::kKnownAbilities) / sizeof(captured::kKnownAbilities[0]);
    static_assert(n > kTail, "the captured list holds the tail");
    return std::span<const int64_t>(captured::kKnownAbilities + (n - kTail), kTail);
}

std::optional<WeaponInfo> worn_weapon(const content::ContentDb& content, const items::Inventory& inventory, uint8_t slot, std::string* error) {
    const svc::InventoryItem* it = inventory.at(svc::ItemContainer::Equipment, slot);
    if (!it) return std::nullopt;
    std::string e;
    const auto w = content.item_weapon(it->item_record_id, e);
    if (!w) {
        if (!e.empty() && error) *error = e;
        return std::nullopt;
    }
    if (w->skill_line == 0 || w->attack_ability == 0) return std::nullopt;
    WeaponInfo out;
    out.combo = w->attack_ability;
    out.line = w->skill_line;
    out.line_name = w->skill_line_name;
    return out;
}

std::vector<int64_t> carried_combos(const content::ContentDb& content, const items::Inventory& inventory) {
    std::vector<int64_t> out;
    const std::vector<svc::InventoryItem> all = inventory.items();
    for (const svc::ItemContainer c : {svc::ItemContainer::Equipment, svc::ItemContainer::Backpack}) {
        std::vector<const svc::InventoryItem*> held;
        for (const svc::InventoryItem& it : all) {
            if (it.container == c) held.push_back(&it);
        }
        std::sort(held.begin(), held.end(), [](const svc::InventoryItem* a, const svc::InventoryItem* b) { return a->slot < b->slot; });
        for (const svc::InventoryItem* it : held) {
            std::string e;
            const auto w = content.item_weapon(it->item_record_id, e);
            if (!w || w->skill_line == 0 || w->attack_ability == 0) continue;
            if (std::find(out.begin(), out.end(), w->attack_ability) == out.end()) out.push_back(w->attack_ability);
        }
    }
    return out;
}

std::vector<int64_t> carried_item_abilities(const content::MountData* mounts, const items::Inventory& inventory, const content::ContentDb* content) {
    std::vector<int64_t> out;
    if (!mounts && !content) return out;
    std::vector<int64_t> seen;
    for (const int64_t record : inventory.records()) {
        if (std::find(seen.begin(), seen.end(), record) != seen.end()) continue;
        seen.push_back(record);
        int64_t ability = 0;
        if (mounts) {
            const content::MountRow* row = mounts->find(record);
            if (row) ability = row->cast_ability;
        }
        if (ability == 0 && content) {
            std::string error;
            ability = content->consumable_ability(record, error);
        }
        if (ability != 0 && std::find(out.begin(), out.end(), ability) == out.end()) out.push_back(ability);
    }
    return out;
}

std::vector<stats::ActiveEffect> rank_effects_of(std::span<const content::AutoGrant* const> granted) {
    std::vector<stats::ActiveEffect> out;
    for (const content::AutoGrant* g : granted) {
        if (!g || g->mods.empty()) continue;
        stats::ActiveEffect e;
        e.effect = {stats::kRefTypeEffect, g->effect_guid};
        e.stacks = 1;
        for (const content::RankStatMod& m : g->mods) e.mods.push_back({m.stat_guid, m.value});
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<stats::WornItem> worn_items_of(const items::Inventory& inventory) {
    std::vector<stats::WornItem> out;
    for (const svc::InventoryItem& it : inventory.items()) {
        if (it.container == svc::ItemContainer::Equipment) out.push_back({it.item_record_id, it.quality, it.slot});
    }
    std::sort(out.begin(), out.end(), [](const stats::WornItem& a, const stats::WornItem& b) { return a.slot < b.slot; });
    return out;
}

std::optional<StatsSetup> stats_setup_for(const std::vector<stats::StatValue>& values, const StatsSetup& layout, std::string& error) {
    std::map<int64_t, const stats::StatValue*> by_guid;
    for (const stats::StatValue& v : values) {
        if (!by_guid.emplace(v.guid, &v).second) {
            error = "the stat " + std::to_string(v.guid) + " is in the character's stats twice";
            return std::nullopt;
        }
        if (!std::isfinite(v.value) || !std::isfinite(v.base) || !std::isfinite(v.equip)) {
            error = "the stat " + std::to_string(v.guid) + " is not a finite number";
            return std::nullopt;
        }
    }
    StatsSetup out;
    fill_stat_array(layout.owner, 1, values, by_guid, out.owner);
    fill_stat_array(layout.owner_proxy, 5, values, by_guid, out.owner_proxy);
    fill_stat_array(layout.everyone_proxy, 7, values, by_guid, out.everyone_proxy);
    int32_t max_float_id = 0;
    for (const FloatStatItem& it : layout.float_everyone_proxy) max_float_id = std::max(max_float_id, it.id);
    for (const FloatStatItem& it : layout.float_everyone_proxy) {
        const auto found = by_guid.find(it.record);
        if (found != by_guid.end() && found->second->float_array() && found->second->replication == 7) out.float_everyone_proxy.push_back(it);
    }
    for (const stats::StatValue& v : values) {
        if (!v.float_array() || v.replication != 7) continue;
        const bool have = std::any_of(out.float_everyone_proxy.begin(), out.float_everyone_proxy.end(),
                                      [&](const FloatStatItem& f) { return f.record == v.guid; });
        if (!have) out.float_everyone_proxy.push_back({++max_float_id, v.guid});
    }
    out.float_array_key = layout.float_array_key;
    return out;
}

ItemStorage item_storage_of(svc::ItemContainer container) {
    switch (container) {
        case svc::ItemContainer::Backpack: return ItemStorage::Backpack;
        case svc::ItemContainer::Equipment: return ItemStorage::Equipment;
        case svc::ItemContainer::Quest: return ItemStorage::Quest;
        case svc::ItemContainer::Resource0: return ItemStorage::Resource0;
        case svc::ItemContainer::Resource1: return ItemStorage::Resource1;
        case svc::ItemContainer::Resource2: return ItemStorage::Resource2;
    }
    return ItemStorage::Backpack;
}

std::vector<HeldItem> held_items_for(const items::Inventory& inventory) {
    const std::array<int64_t, 23> records = items::held_records(inventory);
    std::vector<HeldItem> out;
    out.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        HeldItem h;
        h.id = static_cast<int32_t>(i + 1);
        h.slot = items::kHeldSlots[i];
        h.item_guid = records[i];
        h.enchantment = 0;
        out.push_back(h);
    }
    return out;
}

CharacterSetupResult build_player_setup(const svc::Character& ch, const content::ContentDb& content, const LoginIdentity& login,
                                        const SessionValues& session, const StatContext& stat_context) {
    CharacterSetupResult out;
    PlayerSetup s;
    if (!validate_character_identity(ch, content, s, out)) return out;
    if (!stat_context.bundle || !stat_context.gear) {
        out.error = "world has no stat program: the character's stats cannot be computed";
        return out;
    }
    s.known_abilities = captured_bard_known_abilities();
    s.records_to_load = captured_bard_records_to_load();

    if (!setup_character_inventory(ch, content, s, out)) return out;
    const auto granted = setup_character_progress(ch, stat_context, s, out);
    if (!setup_character_talents(ch, content, stat_context, s, out)) return out;
    if (!setup_character_stats(ch, stat_context, granted, s, out)) return out;

    const CustomizationFromJson cust = customization_from_json(ch.appearance_json, static_cast<ClientRace>(s.race), static_cast<ClientGender>(s.gender));
    if (!cust.value) {
        out.error = cust.error;
        return out;
    }
    s.customization = *cust.value;
    for (const std::string& key : cust.ignored_keys) out.notes.push_back("the appearance has a member '" + key + "' this server does not read");

    if (ch.has_position) {
        const svc::Position& p = ch.position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.yaw)) {
            out.error = "the character's saved position is not finite";
            return out;
        }
        s.x = p.x;
        s.y = p.y;
        s.z = p.z;
        s.yaw_degrees = p.yaw;
    } else {
        s.x = kDefaultSpawn.x;
        s.y = kDefaultSpawn.y;
        s.z = kDefaultSpawn.z;
        s.yaw_degrees = kDefaultSpawn.yaw_degrees;
        out.notes.push_back("the character has no saved position: it spawns at the default spawn point");
    }

    s.login = login;
    s.server_id = session.server_id;
    s.world_time = session.world_time;
    s.player_id = session.player_id;
    s.guids = session.guids;
    out.setup = std::move(s);
    return out;
}

}  // namespace aoc::worldd::bootstrap
