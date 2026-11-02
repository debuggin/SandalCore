// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "content/content_types_ability.h"
#include "content/content_types_economy.h"
#include "content/content_types_quest.h"
#include "content/content_types_world.h"
#include "stats/gear.h"

#include <memory>
#include <mutex>

struct sqlite3;

namespace aoc::worldd::content {

// Opened with SQLITE_OPEN_READONLY in serialized mode so a single connection can be shared across threads.
class ContentDb {
public:
    static std::optional<ContentDb> open(const std::string& path, std::string& error);

    ContentDb(const ContentDb&) = delete;
    ContentDb& operator=(const ContentDb&) = delete;
    ContentDb(ContentDb&& other) noexcept;
    ContentDb& operator=(ContentDb&& other) noexcept;
    ~ContentDb();

    const ContentMeta& meta() const { return meta_; }
    const std::string& path() const { return path_; }

    std::optional<ClassRecord> class_for_archetype(int64_t archetype_id, std::string& error) const;
    std::optional<ClassRecord> class_for_name(const std::string& name, std::string& error) const;
    std::optional<std::vector<PrimaryAttribute>> primary_attributes(int64_t class_guid, int level, std::string& error) const;
    std::optional<AbilityRules> ability_rules(std::string& error) const;

    std::optional<ItemRecord> item(int64_t guid, std::string& error) const;
    std::vector<int64_t> items_named(const std::string& name, std::string& error) const;
    std::optional<ResourceBagRecord> resource_bag(int64_t item_guid, std::string& error) const;

    std::optional<StatProgramRow> stat_program(std::string& error) const;
    std::optional<stats::ItemGear> item_gear(int64_t guid, std::string& error) const;
    std::optional<stats::SetBonus> set_bonus(int64_t guid, std::string& error) const;
    std::optional<std::vector<EquippedEffect>> item_equipped_effects(int64_t item, std::string& error) const;
    std::optional<std::map<int, std::string>> equip_slot_names(std::string& error) const;

    std::optional<ItemUse> item_use(int64_t item_guid, std::string& error) const;
    std::optional<ActionResponse> action_response(int64_t guid, std::string& error) const;
    std::optional<RewardTable> reward_table(int64_t guid, std::string& error) const;
    std::optional<int32_t> variation_quality(int64_t variation, std::string& error) const;
    std::optional<LevelingType> leveling_type_named(const std::string& name, std::string& error) const;
    std::vector<std::string> leveling_type_names(const std::string& experience_type, const std::string& prefix, std::string& error) const;
    std::optional<CurveRow> stat_curve(int64_t guid, std::string& error) const;
    std::optional<ProfessionData> profession_data(std::string& error) const;
    std::optional<CurrencyData> currency_data(std::string& error) const;
    std::optional<GatherData> gather_data(std::string& error) const;
    std::optional<FoliageData> foliage_data(std::string& error) const;
    std::optional<std::vector<AutoGrant>> auto_grants(int64_t leveling_type_guid, std::string& error) const;

    std::optional<TalentData> talent_data(std::string& error) const;
    std::optional<ItemWeaponRow> item_weapon(int64_t item_guid, std::string& error) const;

    std::optional<CombatRows> combat_rows(std::string& error) const;
    std::optional<std::map<int64_t, LootTable>> loot_tables(const std::vector<int64_t>& roots, std::string& error) const;
    std::optional<int64_t> xp_rule(const std::string& key, std::string& error) const;

    std::optional<PopulationData> population_data(std::string& error) const;
    std::optional<DialogueData> dialogue_data(std::string& error) const;
    std::optional<QuestData> quest_data(std::string& error) const;
    std::optional<ObjectData> object_data(std::string& error) const;
    std::optional<CraftStationData> craft_station_data(std::string& error) const;
    std::optional<RespawnData> respawn_data(std::string& error) const;
    std::optional<CraftRecipeData> craft_recipe_data(std::string& error) const;
    std::optional<VendorData> vendor_data(std::string& error) const;
    std::optional<WarehouseData> warehouse_data(std::string& error) const;
    std::optional<ItemSaleFacts> item_sale(int64_t item, std::string& error) const;
    std::optional<VendorDefRow> vendor_def(int64_t guid, std::string& error) const;
    std::optional<VendorDefRow> vendor_def_named(const std::string& name, std::string& error) const;
    std::optional<std::vector<VendorDefRow>> vendor_defs_with_prefix(const std::string& prefix, std::string& error) const;
    std::optional<std::vector<GmItemRow>> gm_items(std::string& error) const;
    std::optional<MountData> mount_data(std::string& error) const;

private:
    ContentDb() = default;

    // Percentage stats (stat_def.display_pct) are preserved unrounded in item stat blocks.
    void load_percent_stats();

    sqlite3*          db_ = nullptr;
    std::string       path_;
    ContentMeta       meta_;
    std::set<int64_t> percent_stats_;
};

class ContentGearCatalog final : public stats::GearCatalog {
public:
    explicit ContentGearCatalog(const ContentDb& db);

    bool               ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    const stats::ItemGear* item(int64_t record) const override;
    const stats::SetBonus* set(int64_t guid) const override;
    std::string            slot_name(int slot) const override;

private:
    const ContentDb&                                                      db_;
    std::map<int, std::string>                                            slots_;
    std::string                                                           error_;
    mutable std::mutex                                                    mutex_;
    mutable std::unordered_map<int64_t, std::unique_ptr<stats::ItemGear>> items_;
    mutable std::unordered_map<int64_t, std::unique_ptr<stats::SetBonus>> sets_;
};

}  // namespace aoc::worldd::content
