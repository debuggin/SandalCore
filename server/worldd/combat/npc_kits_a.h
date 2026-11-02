// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include <cstddef>

namespace aoc::worldd::combat::npc_kits {

struct KitAbility {
    const char* name;
    int         uses;
    int         windup_ms;
    int         range_cm;
};
struct Kit {
    const char*       blueprint;   // the Blueprint class without "_C"
    const KitAbility* abilities;
    size_t            count;
    int               interval_ms;
};

inline constexpr KitAbility kAbilities0[] = {
    {"WitheredCombatant_Exhausted", 47, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 47, 500, 250},
    {"WitheredCombatant_WitheredMaw", 47, 500, 250},
    {"Bandit_PhysicalPotion", 44, 500, 250},
    {"Bandit_Raider_BrutalCleave", 44, 500, 250},
    {"Bandit_Raider_HeavySlash", 44, 500, 250},
    {"Bandit_Raider_LeapStrike", 44, 1532, 250},
    {"Bandit_Raider_Strike", 44, 500, 250},
    {"Bandit_Raider_Whirlwind", 44, 500, 250},
    {"WitheredCombatant_Breath", 44, 500, 250},
    {"Ooze_Dropin", 40, 500, 250},
    {"Ooze_Duplicate", 40, 500, 250},
    {"Ooze_Duplicate_Spawned", 40, 500, 250},
    {"Ooze_OminousSkull", 40, 2376, 250},
    {"Ooze_Swipe", 40, 500, 250},
    {"LivingGeode_BrightmoundShriek", 29, 500, 250},
    {"LivingGeode_CrystalShard", 29, 3477, 250},
    {"LivingGeode_RendingBite", 29, 502, 250},
    {"LivingGeode_Resonance", 29, 1203, 250},
    {"LivingGeode_Shriek", 29, 668, 250},
    {"LivingGeode_SlashingSwirl", 29, 500, 250},
    {"BloodCultist_CorruptSacrifice", 16, 500, 250},
    {"BloodCultist_RottenEruption", 16, 500, 250},
    {"BloodCultist_SanguineBolt", 16, 500, 250},
    {"Crab_Giant_Guillotine", 15, 500, 250},
    {"Crab_Pinch", 15, 500, 250},
    {"Crab_Retract", 15, 500, 250},
    {"Crab_WaterSpray", 15, 500, 250},
    {"Goblin_Rally", 14, 500, 250},
    {"BogBrute_BogBash", 13, 500, 250},
    {"BogBrute_FistStrike", 13, 500, 250},
    {"BogBrute_LeapingBogBash", 13, 500, 250},
    {"BogBrute_MudSink", 13, 500, 250},
    {"BogBrute_MudSummon", 13, 500, 250},
    {"BogBrute_Unbury", 13, 500, 250},
    {"BogBrute_UnburyBogBash", 13, 500, 250},
    {"Goblin_Headbutt", 9, 500, 250},
    {"Goblin_LeftSwing", 9, 369, 250},
    {"Goblin_Pummel", 9, 500, 250},
    {"Goblin_RightSwing", 9, 369, 250},
    {"Goblin_RockThrow", 9, 500, 250},
    {"Goblin_RockThrowHarass", 9, 500, 250},
    {"Goblin_StationaryHeadbutt", 9, 500, 250},
    {"GoblinSeer_EnergyBolt", 5, 500, 250},
    {"GoblinSeer_EnergyShard", 5, 500, 250},
    {"GoblinShaman_Bludgeon", 5, 500, 250},
    {"GoblinShaman_Flee", 5, 500, 250},
    {"AshenHaunt_BlazingVisage", 4, 500, 250},
    {"BanditCaptain_Bash", 4, 500, 250},
    {"BanditCaptain_BasicSwordAttack", 4, 500, 250},
    {"BanditCaptain_BasicSwordAttack02", 4, 500, 250},
    {"BanditCaptain_BasicSwordAttack03", 4, 500, 250},
    {"BanditCaptain_Slash", 4, 500, 250},
    {"BanditPirateCaptain_CutlassBarrage", 4, 500, 250},
    {"BanditPirateCaptain_Firepot", 4, 500, 250},
    {"BanditPirateCaptain_SeaShanty", 4, 500, 250},
    {"BanditPirateCaptain_SwigOfRum", 4, 500, 250},
    {"BanditPirateCaptain_Taunt", 4, 500, 250},
    {"Bandit_Healer_DivineProtection", 4, 500, 250},
    {"Bandit_Healer_DivineShot", 4, 500, 250},
    {"Bandit_Healer_ReliefTouch", 4, 500, 250},
    {"Gryphon_Bite", 4, 500, 250},
    {"Gryphon_Rake", 4, 500, 250},
    {"AlphaWolf_Growl", 3, 902, 250},
    {"AshenHaunt_EtherealShift", 3, 500, 250},
    {"AshenHaunt_KingStrike", 3, 2273, 250},
    {"AshenHaunt_PokeStrike", 3, 502, 250},
    {"AshenHaunt_SweepingStrike", 3, 871, 250},
    {"AshenHaunt_UpStrike", 3, 500, 250},
    {"AshenHaunt_WhirlingThrow", 3, 500, 250},
    {"Bandit_Healer_WaveBreak", 3, 500, 250},
    {"Minotaur_Summoner_Wolf_SacrificeHowl", 3, 500, 250},
    {"WitheredCombatant_DevotedInvigoration", 3, 500, 250},
    {"WitheredCombatant_Shift_Bwd", 3, 500, 250},
    {"Wolf_ArrowDash", 3, 500, 250},
    {"Wolf_Bite", 3, 500, 250},
    {"Wolf_Howl", 3, 500, 250},
    {"Wolf_Pounce", 3, 500, 250},
    {"Wolf_RageHowl", 3, 500, 250},
    {"AshenHaunt_Deaden", 1, 500, 250},
    {"AshenHaunt_Fireball", 1, 500, 250},
    {"AshenHaunt_LeftSwing", 1, 980, 250},
    {"AshenHaunt_MageArmor", 1, 500, 250},
    {"AshenHaunt_RightSwing", 1, 1135, 250},
    {"AshenHaunt_Silence", 1, 500, 250},
    {"AshenHaunt_SpectralBolt", 1, 732, 250},
    {"AshenHaunt_SummonWeapon", 1, 500, 250},
    {"Bandit_Aerialist_Airstrike", 1, 500, 250},
    {"Bandit_Aerialist_KillShot", 1, 500, 250},
    {"Bandit_Aerialist_LongbowShot", 1, 500, 250},
    {"Bandit_Arsonist_Firebolt", 1, 500, 250},
    {"Bandit_Arsonist_FireboltCombo", 1, 500, 250},
    {"Bandit_Arsonist_FlameBurst", 1, 500, 250},
    {"Bandit_Arsonist_FlameRing", 1, 500, 250},
    {"Bandit_Arsonist_FlameThrow", 1, 500, 250},
    {"Bandit_Healer_BlessWeapon2", 1, 500, 250},
    {"Bandit_Healer_Mending", 1, 500, 250},
    {"Bandit_MagicalPotion", 1, 500, 250},
    {"Bandit_Musician_Discordance", 1, 500, 250},
    {"Bandit_Musician_ForebodingMelody", 1, 500, 250},
    {"Bandit_Musician_GleefulMelody", 1, 500, 250},
    {"Bandit_Musician_LustfulSerenade", 1, 500, 250},
    {"Bandit_Musician_MagicShot", 1, 500, 250},
    {"Bandit_Musician_SinisterLullaby", 1, 500, 250},
    {"Bandit_Pirate_Archer_AnchoringShot", 1, 500, 250},
    {"Bandit_Pirate_Archer_BrinyShot", 1, 500, 250},
    {"Bandit_Pirate_Archer_Grog", 1, 500, 250},
    {"Bandit_Rogue_DaggerStrike", 1, 500, 250},
    {"Bandit_Rogue_ShadowCascade", 1, 500, 250},
    {"Bandit_Rogue_ShadowStep", 1, 500, 250},
    {"Bandit_Rogue_SmokeBomb", 1, 500, 250},
    {"Bandit_Rogue_Stab", 1, 500, 250},
    {"ClayDunzenkell_Mage_ChainShock", 1, 267, 250},
    {"ClayDunzenkell_Mage_LightningOrb", 1, 500, 250},
    {"ClayDunzenkell_Mage_ThunderClap", 1, 500, 250},
    {"ClayDunzenkell_Ranger_AutoShot", 1, 1092, 250},
    {"ClayDunzenkell_Ranger_ExplosiveShot", 1, 500, 250},
    {"ClayDunzenkell_Ranger_KillShot", 1, 2072, 250},
    {"ClayDunzenkell_Ranger_Reconstitution", 1, 500, 250},
    {"Dunir_Mage_Counterspell", 1, 500, 250},
    {"Dunir_Mage_Displacement", 1, 500, 250},
    {"Dunir_Mage_ForgeBurst", 1, 500, 250},
    {"FlameFanatic_UnendurableFrenzy", 1, 500, 250},
    {"GoblinFlayer_Cleave", 1, 500, 250},
    {"GoblinFlayer_Enrage", 1, 500, 250},
    {"GoblinFlayer_ManicSneer", 1, 500, 250},
    {"GoblinFlayer_OverheadSlash", 1, 500, 250},
    {"GoblinFlayer_OverheadSlash2", 1, 500, 250},
    {"GoblinFlayer_SeveringSpin", 1, 500, 250},
    {"GoblinFlayer_WeaponGuard", 1, 500, 250},
    {"Minotaur_BattlePriest_BattlePrayer_Daze", 1, 500, 250},
    {"Minotaur_BattlePriest_BattlePrayer_Resonance", 1, 500, 250},
    {"Minotaur_BattlePriest_LightSpear", 1, 500, 250},
    {"Minotaur_BattlePriest_ShieldBash", 1, 500, 250},
    {"Minotaur_BattlePriest_SpearStrike", 1, 500, 250},
    {"Minotaur_BattlePriest_SpearStrike_Radiant", 1, 500, 250},
    {"Minotaur_Streamweaver_EmpoweredShot_Frost", 1, 500, 250},
    {"Minotaur_Streamweaver_EmpoweredShot_Water", 1, 500, 250},
    {"Minotaur_Streamweaver_Shell_Frost", 1, 500, 250},
    {"Minotaur_Streamweaver_Shell_Water", 1, 500, 250},
    {"Minotaur_Streamweaver_Shot_Frost", 1, 500, 250},
    {"Minotaur_Streamweaver_Shot_Water", 1, 500, 250},
    {"Minotaur_Summoner_RavenFeast", 1, 500, 250},
    {"Minotaur_Summoner_SpiritAid", 1, 500, 250},
    {"Minotaur_Summoner_SpiritAura", 1, 500, 250},
    {"Minotaur_Summoner_SpiritInstability", 1, 500, 250},
    {"Minotaur_Summoner_SpiritShot", 1, 500, 250},
    {"Minotaur_Summoner_WolfSpirit", 1, 500, 250},
    {"MountAbility_Sprint", 1, 500, 250},
};
inline constexpr KitAbility kAbilities1[] = {
    {"AlphaWolf_Growl", 3, 902, 2705},
    {"Minotaur_Summoner_Wolf_SacrificeHowl", 3, 500, 250},
    {"Wolf_ArrowDash", 3, 500, 250},
    {"Wolf_Bite", 3, 500, 250},
    {"Wolf_Howl", 3, 500, 250},
    {"Wolf_Pounce", 3, 500, 250},
    {"Wolf_RageHowl", 3, 500, 250},
};
inline constexpr KitAbility kAbilities2[] = {
    {"Ancient_Caster_Brand", 5, 500, 250},
    {"Ancient_Caster_DescendingVoid", 5, 500, 250},
    {"Ancient_Caster_Destruction", 5, 500, 250},
    {"Ancient_Caster_Dominion", 5, 500, 250},
    {"Ancient_Caster_GreaterVoidBolt", 5, 500, 250},
    {"Ancient_Caster_Repulsion", 5, 500, 250},
    {"Ancient_Caster_StaffSweep", 5, 500, 250},
    {"Ancient_Caster_VoidBolt", 5, 500, 250},
    {"Ancient_FootSoldier_SigilofCommand", 5, 500, 250},
};
inline constexpr KitAbility kAbilities3[] = {
    {"Ancient_FootSoldier_Camisado", 1, 500, 250},
    {"Ancient_FootSoldier_FetteringStrike", 1, 500, 250},
    {"Ancient_FootSoldier_Foresight", 1, 500, 250},
    {"Ancient_FootSoldier_SigilofCommand", 1, 500, 250},
    {"Ancient_FootSoldier_VoidGlaive", 1, 500, 250},
    {"Ancient_FootSoldier_WoundStrike", 1, 500, 250},
};
inline constexpr KitAbility kAbilities4[] = {
    {"Ancient_FootSoldier_Camisado", 8, 500, 250},
    {"Ancient_FootSoldier_FetteringStrike", 8, 500, 250},
    {"Ancient_FootSoldier_Foresight", 8, 500, 250},
    {"Ancient_FootSoldier_SigilofCommand", 8, 500, 250},
    {"Ancient_FootSoldier_VoidGlaive", 8, 500, 250},
    {"Ancient_FootSoldier_WoundStrike", 8, 500, 250},
};
inline constexpr KitAbility kAbilities5[] = {
    {"Bandit_Aerialist_Airstrike", 74, 500, 250},
    {"Bandit_Aerialist_CelerityPotion", 74, 500, 250},
    {"Bandit_Aerialist_KillShot", 74, 500, 250},
    {"Bandit_Aerialist_LightningShot", 74, 500, 250},
    {"Bandit_Aerialist_LongbowShot", 74, 500, 250},
};
inline constexpr KitAbility kAbilities6[] = {
    {"Bandit_Aerialist_Airstrike", 34, 500, 250},
    {"Bandit_Aerialist_CelerityPotion", 34, 500, 250},
    {"Bandit_Aerialist_KillShot", 34, 500, 250},
    {"Bandit_Aerialist_LightningShot", 34, 500, 250},
    {"Bandit_Aerialist_LongbowShot", 34, 500, 250},
};
inline constexpr KitAbility kAbilities7[] = {
    {"Bandit_Aerialist_Airstrike", 5, 500, 250},
    {"Bandit_Aerialist_CelerityPotion", 5, 500, 250},
    {"Bandit_Aerialist_KillShot", 5, 500, 250},
    {"Bandit_Aerialist_LightningShot", 5, 500, 250},
    {"Bandit_Aerialist_LongbowShot", 5, 500, 250},
};
inline constexpr KitAbility kAbilities8[] = {
    {"Bandit_Aerialist_Airstrike", 5, 500, 250},
    {"Bandit_Aerialist_CelerityPotion", 5, 500, 250},
    {"Bandit_Aerialist_KillShot", 5, 500, 250},
    {"Bandit_Aerialist_LightningShot", 5, 500, 250},
    {"Bandit_Aerialist_LongbowShot", 5, 500, 250},
};
inline constexpr KitAbility kAbilities9[] = {
    {"Bandit_Aerialist_Airstrike", 203, 500, 250},
    {"Bandit_Aerialist_CelerityPotion", 203, 500, 250},
    {"Bandit_Aerialist_KillShot", 203, 500, 250},
    {"Bandit_Aerialist_LightningShot", 203, 500, 250},
    {"Bandit_Aerialist_LongbowShot", 203, 500, 250},
};
inline constexpr KitAbility kAbilities10[] = {
    {"Bandit_Arsonist_Firebolt", 2, 500, 250},
    {"Bandit_Arsonist_FireboltCombo", 2, 500, 250},
    {"Bandit_Arsonist_FlameBurst", 2, 500, 250},
    {"Bandit_Arsonist_FlameRing", 2, 500, 250},
    {"Bandit_Arsonist_FlameThrow", 2, 500, 250},
    {"Bandit_MagicalPotion", 2, 500, 250},
    {"FlameFanatic_UnendurableFrenzy", 2, 500, 250},
};
inline constexpr KitAbility kAbilities11[] = {
    {"Bandit_Arsonist_Firebolt", 49, 500, 250},
    {"Bandit_Arsonist_FireboltCombo", 49, 500, 250},
    {"Bandit_Arsonist_FlameBurst", 49, 500, 250},
    {"Bandit_Arsonist_FlameRing", 49, 500, 250},
    {"Bandit_Arsonist_FlameThrow", 49, 500, 250},
    {"Bandit_MagicalPotion", 49, 500, 250},
    {"FlameFanatic_UnendurableFrenzy", 49, 500, 250},
};
inline constexpr KitAbility kAbilities12[] = {
    {"Bandit_Arsonist_Firebolt", 6, 500, 250},
    {"Bandit_Arsonist_FireboltCombo", 6, 500, 250},
    {"Bandit_Arsonist_FlameBurst", 6, 500, 250},
    {"Bandit_Arsonist_FlameRing", 6, 500, 250},
    {"Bandit_Arsonist_FlameThrow", 6, 500, 250},
    {"Bandit_MagicalPotion", 6, 500, 250},
    {"FlameFanatic_UnendurableFrenzy", 6, 500, 250},
};
inline constexpr KitAbility kAbilities13[] = {
    {"Bandit_Arsonist_Firebolt", 3, 500, 250},
    {"Bandit_Arsonist_FireboltCombo", 3, 500, 250},
    {"Bandit_Arsonist_FlameBurst", 3, 500, 250},
    {"Bandit_Arsonist_FlameRing", 3, 500, 250},
    {"Bandit_Arsonist_FlameThrow", 3, 500, 250},
    {"Bandit_MagicalPotion", 3, 500, 250},
    {"FlameFanatic_UnendurableFrenzy", 3, 500, 250},
};
inline constexpr KitAbility kAbilities14[] = {
    {"Bandit_Arsonist_Firebolt", 122, 500, 250},
    {"Bandit_Arsonist_FireboltCombo", 122, 500, 250},
    {"Bandit_Arsonist_FlameBurst", 122, 500, 250},
    {"Bandit_Arsonist_FlameRing", 122, 500, 250},
    {"Bandit_Arsonist_FlameThrow", 122, 500, 250},
    {"Bandit_MagicalPotion", 122, 500, 250},
    {"FlameFanatic_UnendurableFrenzy", 122, 500, 250},
};
inline constexpr KitAbility kAbilities15[] = {
    {"BanditCaptain_Bash", 1, 500, 250},
    {"BanditCaptain_BasicSwordAttack", 1, 500, 250},
    {"BanditCaptain_BasicSwordAttack02", 1, 500, 250},
    {"BanditCaptain_BasicSwordAttack03", 1, 500, 250},
    {"BanditCaptain_Slash", 1, 500, 250},
    {"BanditPirateCaptain_CutlassBarrage", 1, 500, 250},
    {"BanditPirateCaptain_Firepot", 1, 500, 250},
    {"BanditPirateCaptain_SeaShanty", 1, 500, 250},
    {"BanditPirateCaptain_SwigOfRum", 1, 500, 250},
    {"BanditPirateCaptain_Taunt", 1, 500, 250},
};
inline constexpr KitAbility kAbilities16[] = {
    {"BanditCaptain_Bash", 8, 500, 250},
    {"BanditCaptain_BasicBowAttack", 8, 500, 250},
    {"BanditCaptain_BasicSwordAttack", 8, 500, 250},
    {"BanditCaptain_BasicSwordAttack02", 8, 500, 250},
    {"BanditCaptain_BasicSwordAttack03", 8, 500, 250},
    {"BanditCaptain_MoraleBoost", 8, 500, 250},
    {"BanditCaptain_ShieldRaise", 8, 500, 250},
    {"BanditCaptain_Slash", 8, 500, 250},
    {"BanditCaptain_Taunt", 8, 500, 250},
    {"BanditCaptain_Tomahawk", 8, 500, 250},
};
inline constexpr KitAbility kAbilities17[] = {
    {"BanditCaptain_Bash", 3, 500, 250},
    {"BanditCaptain_BasicBowAttack", 3, 500, 250},
    {"BanditCaptain_BasicSwordAttack", 3, 500, 250},
    {"BanditCaptain_BasicSwordAttack02", 3, 500, 250},
    {"BanditCaptain_BasicSwordAttack03", 3, 500, 250},
    {"BanditCaptain_MoraleBoost", 3, 500, 250},
    {"BanditCaptain_ShieldRaise", 3, 500, 250},
    {"BanditCaptain_Slash", 3, 500, 250},
    {"BanditCaptain_Taunt", 3, 500, 250},
    {"BanditCaptain_Tomahawk", 3, 500, 250},
};
inline constexpr KitAbility kAbilities18[] = {
    {"BanditCaptain_Bash", 22, 500, 250},
    {"BanditCaptain_BasicBowAttack", 22, 500, 250},
    {"BanditCaptain_BasicSwordAttack", 22, 500, 250},
    {"BanditCaptain_BasicSwordAttack02", 22, 500, 250},
    {"BanditCaptain_BasicSwordAttack03", 22, 500, 250},
    {"BanditCaptain_MoraleBoost", 22, 500, 250},
    {"BanditCaptain_ShieldRaise", 22, 500, 250},
    {"BanditCaptain_Slash", 22, 500, 250},
    {"BanditCaptain_Taunt", 22, 500, 250},
    {"BanditCaptain_Tomahawk", 22, 500, 250},
};
inline constexpr KitAbility kAbilities19[] = {
    {"Bandit_Healer_DivineProtection", 14, 500, 250},
    {"Bandit_Healer_DivineShot", 14, 500, 250},
    {"Bandit_Healer_ReliefTouch", 14, 500, 250},
    {"Bandit_Healer_WaveBreak", 14, 500, 250},
    {"WitheredCombatant_DevotedInvigoration", 14, 500, 250},
    {"WitheredCombatant_Exhausted", 14, 500, 250},
    {"WitheredCombatant_Shift_Bwd", 14, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 14, 500, 250},
    {"WitheredCombatant_WitheredMaw", 14, 500, 250},
};
inline constexpr KitAbility kAbilities20[] = {
    {"Bandit_Healer_DivineProtection", 3, 500, 250},
    {"Bandit_Healer_DivineShot", 3, 500, 250},
    {"Bandit_Healer_ReliefTouch", 3, 500, 250},
    {"Bandit_Healer_WaveBreak", 3, 500, 250},
    {"WitheredCombatant_DevotedInvigoration", 3, 500, 250},
    {"WitheredCombatant_Exhausted", 3, 500, 250},
    {"WitheredCombatant_Shift_Bwd", 3, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 3, 500, 250},
    {"WitheredCombatant_WitheredMaw", 3, 500, 250},
};
inline constexpr KitAbility kAbilities21[] = {
    {"Bandit_Healer_DivineProtection", 51, 500, 250},
    {"Bandit_Healer_DivineShot", 51, 500, 250},
    {"Bandit_Healer_ReliefTouch", 51, 500, 250},
    {"Bandit_Healer_WaveBreak", 51, 500, 250},
    {"WitheredCombatant_DevotedInvigoration", 51, 500, 250},
    {"WitheredCombatant_Exhausted", 51, 500, 250},
    {"WitheredCombatant_Shift_Bwd", 51, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 51, 500, 250},
    {"WitheredCombatant_WitheredMaw", 51, 500, 250},
};
inline constexpr KitAbility kAbilities22[] = {
    {"Bandit_Healer_DivineProtection", 2, 500, 250},
    {"Bandit_Healer_DivineShot", 2, 500, 250},
    {"Bandit_Healer_ReliefTouch", 2, 500, 250},
    {"Bandit_Healer_WaveBreak", 2, 500, 250},
    {"WitheredCombatant_DevotedInvigoration", 2, 500, 250},
    {"WitheredCombatant_Exhausted", 2, 500, 250},
    {"WitheredCombatant_Shift_Bwd", 2, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 2, 500, 250},
    {"WitheredCombatant_WitheredMaw", 2, 500, 250},
};
inline constexpr KitAbility kAbilities23[] = {
    {"Bandit_Musician_Discordance", 22, 500, 250},
    {"Bandit_Musician_ForebodingMelody", 22, 500, 250},
    {"Bandit_Musician_GleefulMelody", 22, 500, 250},
    {"Bandit_Musician_LustfulSerenade", 22, 500, 250},
    {"Bandit_Musician_MagicShot", 22, 500, 250},
    {"Bandit_Musician_SinisterLullaby", 22, 500, 250},
};
inline constexpr KitAbility kAbilities24[] = {
    {"Bandit_Musician_Discordance", 28, 500, 250},
    {"Bandit_Musician_ForebodingMelody", 28, 500, 250},
    {"Bandit_Musician_GleefulMelody", 28, 500, 250},
    {"Bandit_Musician_LustfulSerenade", 28, 500, 250},
    {"Bandit_Musician_MagicShot", 28, 500, 250},
    {"Bandit_Musician_SinisterLullaby", 28, 500, 250},
};
inline constexpr KitAbility kAbilities25[] = {
    {"Bandit_PhysicalPotion", 4, 500, 250},
    {"Bandit_Raider_BrutalCleave", 4, 500, 250},
    {"Bandit_Raider_HeavySlash", 4, 500, 250},
    {"Bandit_Raider_LeapStrike", 4, 1532, 250},
    {"Bandit_Raider_Strike", 4, 500, 250},
    {"Bandit_Raider_Whirlwind", 4, 500, 250},
    {"WitheredCombatant_Breath", 4, 500, 250},
    {"WitheredCombatant_Exhausted", 4, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 4, 500, 250},
    {"WitheredCombatant_WitheredMaw", 4, 500, 250},
};
inline constexpr KitAbility kAbilities26[] = {
    {"Bandit_PhysicalPotion", 22, 500, 250},
    {"Bandit_Raider_BrutalCleave", 22, 500, 250},
    {"Bandit_Raider_HeavySlash", 22, 500, 250},
    {"Bandit_Raider_LeapStrike", 22, 1532, 250},
    {"Bandit_Raider_Strike", 22, 500, 250},
    {"Bandit_Raider_Whirlwind", 22, 500, 250},
    {"WitheredCombatant_Breath", 22, 500, 250},
    {"WitheredCombatant_Exhausted", 22, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 22, 500, 250},
    {"WitheredCombatant_WitheredMaw", 22, 500, 250},
};
inline constexpr KitAbility kAbilities27[] = {
    {"Bandit_PhysicalPotion", 396, 500, 250},
    {"Bandit_Raider_BrutalCleave", 396, 500, 250},
    {"Bandit_Raider_HeavySlash", 396, 500, 250},
    {"Bandit_Raider_LeapStrike", 396, 1532, 250},
    {"Bandit_Raider_Strike", 396, 500, 250},
    {"Bandit_Raider_Whirlwind", 396, 500, 250},
    {"WitheredCombatant_Breath", 396, 500, 250},
    {"WitheredCombatant_Exhausted", 396, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 396, 500, 250},
    {"WitheredCombatant_WitheredMaw", 396, 500, 250},
};
inline constexpr KitAbility kAbilities28[] = {
    {"Bandit_PhysicalPotion", 10, 500, 250},
    {"Bandit_Raider_BrutalCleave", 10, 500, 250},
    {"Bandit_Raider_HeavySlash", 10, 500, 250},
    {"Bandit_Raider_LeapStrike", 10, 1532, 250},
    {"Bandit_Raider_Strike", 10, 500, 250},
    {"Bandit_Raider_Whirlwind", 10, 500, 250},
    {"WitheredCombatant_Breath", 10, 500, 250},
    {"WitheredCombatant_Exhausted", 10, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 10, 500, 250},
    {"WitheredCombatant_WitheredMaw", 10, 500, 250},
};
inline constexpr KitAbility kAbilities29[] = {
    {"Bandit_PhysicalPotion", 1, 500, 250},
    {"Bandit_Raider_BrutalCleave", 1, 500, 250},
    {"Bandit_Raider_HeavySlash", 1, 500, 250},
    {"Bandit_Raider_LeapStrike", 1, 1532, 250},
    {"Bandit_Raider_Strike", 1, 500, 250},
    {"Bandit_Raider_Whirlwind", 1, 500, 250},
    {"WitheredCombatant_Breath", 1, 500, 250},
    {"WitheredCombatant_Exhausted", 1, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 1, 500, 250},
    {"WitheredCombatant_WitheredMaw", 1, 500, 250},
};
inline constexpr KitAbility kAbilities30[] = {
    {"Bandit_Rogue_DaggerStrike", 53, 500, 250},
    {"Bandit_Rogue_ShadowCascade", 53, 500, 250},
    {"Bandit_Rogue_ShadowStep", 53, 500, 250},
    {"Bandit_Rogue_SmokeBomb", 53, 500, 250},
    {"Bandit_Rogue_Stab", 53, 500, 250},
};
inline constexpr KitAbility kAbilities31[] = {
    {"Basilisk_Bite", 3, 500, 250},
    {"Basilisk_Enrage", 3, 500, 250},
    {"Basilisk_EnragedTailSplit", 3, 500, 250},
    {"Basilisk_HeavyBite", 3, 500, 250},
    {"Basilisk_PetrifyingGaze", 3, 500, 250},
    {"Basilisk_TailSplit", 3, 500, 250},
    {"Basilisk_TailSwipe", 3, 500, 250},
};
inline constexpr KitAbility kAbilities32[] = {
    {"Bear_AlertedRearUp", 18, 500, 250},
    {"Bear_Bite", 18, 500, 250},
    {"Bear_Enraged_Roar", 18, 500, 250},
    {"Bear_LeftSwing", 18, 500, 250},
    {"Bear_Pound", 18, 500, 250},
    {"Bear_Ravage", 18, 500, 250},
    {"Bear_RearUp", 18, 500, 250},
    {"Bear_RightSwing", 18, 500, 250},
};
inline constexpr KitAbility kAbilities33[] = {
    {"Beetle_FlameSpray", 2, 500, 250},
    {"Beetle_FlyingHorn", 2, 500, 250},
    {"Beetle_HornAttack", 2, 500, 250},
    {"Beetle_RearWing", 2, 500, 250},
    {"Beetle_RhinoCharge", 2, 500, 250},
    {"Beetle_Subsonics", 2, 500, 250},
    {"Beetle_WingBlast", 2, 500, 250},
};
inline constexpr KitAbility kAbilities34[] = {
    {"BroodTyrant_FireSilkShot", 3, 500, 250},
    {"BroodTyrant_FireSilkVolley", 3, 500, 250},
    {"BroodTyrant_FlameSpray", 3, 500, 250},
    {"BroodTyrant_IgnitingBite", 3, 500, 250},
    {"BroodTyrant_ScorchingRay", 3, 500, 250},
    {"BroodTyrant_ScorchingWarp_Backward", 3, 500, 250},
    {"BroodTyrant_ScorchingWarp_Forward", 3, 500, 250},
    {"BroodTyrant_Swipe", 3, 500, 250},
};
inline constexpr KitAbility kAbilities35[] = {
    {"Bandit_Arsonist_Firebolt", 6, 500, 250},
    {"Bandit_Arsonist_FireboltCombo", 6, 500, 250},
    {"Bandit_Arsonist_FlameBurst", 6, 500, 250},
    {"Bandit_Arsonist_FlameRing", 6, 500, 250},
    {"Bandit_Arsonist_FlameThrow", 6, 500, 250},
    {"Bandit_MagicalPotion", 6, 500, 250},
    {"FlameFanatic_UnendurableFrenzy", 6, 500, 250},
};
inline constexpr KitAbility kAbilities36[] = {
    {"Bandit_Healer_DivineProtection", 3, 500, 250},
    {"Bandit_Healer_DivineShot", 3, 500, 250},
    {"Bandit_Healer_ReliefTouch", 3, 500, 250},
    {"Bandit_Healer_WaveBreak", 3, 500, 250},
    {"WitheredCombatant_DevotedInvigoration", 3, 500, 250},
    {"WitheredCombatant_Exhausted", 3, 500, 250},
    {"WitheredCombatant_Shift_Bwd", 3, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 3, 500, 250},
    {"WitheredCombatant_WitheredMaw", 3, 500, 250},
};
inline constexpr KitAbility kAbilities37[] = {
    {"Bandit_PhysicalPotion", 36, 500, 250},
    {"Bandit_Raider_BrutalCleave", 36, 500, 250},
    {"Bandit_Raider_HeavySlash", 36, 500, 250},
    {"Bandit_Raider_LeapStrike", 36, 1532, 250},
    {"Bandit_Raider_Strike", 36, 500, 250},
    {"Bandit_Raider_Whirlwind", 36, 500, 250},
    {"WitheredCombatant_Breath", 36, 500, 250},
    {"WitheredCombatant_Exhausted", 36, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 36, 500, 250},
    {"WitheredCombatant_WitheredMaw", 36, 500, 250},
};
inline constexpr KitAbility kAbilities38[] = {
    {"Bear_AlertedRearUp", 147, 500, 250},
    {"Bear_Bite", 147, 500, 250},
    {"Bear_Enraged_Roar", 147, 500, 250},
    {"Bear_LeftSwing", 147, 500, 250},
    {"Bear_Pound", 147, 500, 250},
    {"Bear_Ravage", 147, 500, 250},
    {"Bear_RearUp", 147, 500, 250},
    {"Bear_RightSwing", 147, 500, 250},
};
inline constexpr KitAbility kAbilities39[] = {
    {"Bandit_Healer_DivineProtection", 2, 500, 250},
    {"Bandit_Healer_DivineShot", 2, 500, 250},
    {"Bandit_Healer_ReliefTouch", 2, 500, 250},
    {"Bandit_Healer_WaveBreak", 2, 500, 250},
    {"WitheredCombatant_DevotedInvigoration", 2, 500, 250},
    {"WitheredCombatant_Exhausted", 2, 500, 250},
    {"WitheredCombatant_Shift_Bwd", 2, 500, 250},
    {"WitheredCombatant_Shift_Fwd", 2, 500, 250},
    {"WitheredCombatant_WitheredMaw", 2, 500, 250},
};
inline constexpr KitAbility kAbilities40[] = {
    {"Caravan_CollectCargo", 2, 500, 250},
    {"Caravan_ConvertToGround", 2, 500, 250},
    {"Caravan_ConvertToRaft", 2, 500, 250},
    {"Caravan_SpeedBoost", 2, 500, 250},
    {"Caravan_Unstuck", 2, 500, 250},
    {"Vehicle_TurnIn", 2, 500, 250},
};
inline constexpr KitAbility kAbilities41[] = {
    {"Cow_FrontKick", 6, 500, 250},
    {"Cow_Headbutt", 6, 500, 250},
};
inline constexpr KitAbility kAbilities42[] = {
    {"Crab_Giant_Guillotine", 129, 500, 250},
    {"Crab_Pinch", 129, 500, 250},
    {"Crab_Retract", 129, 500, 250},
    {"Crab_WaterSpray", 129, 500, 250},
};
inline constexpr KitAbility kAbilities43[] = {
    {"Crab_Giant_Guillotine", 18, 500, 250},
    {"Crab_Pinch", 18, 500, 1566},
    {"Crab_Retract", 18, 500, 250},
    {"Crab_WaterSpray", 18, 500, 250},
};
inline constexpr KitAbility kAbilities44[] = {
    {"CrystalGolem_AntiMagicShield", 6, 500, 250},
    {"CrystalGolem_ArcaneDispersion", 6, 500, 250},
    {"CrystalGolem_Disintegrate", 6, 500, 250},
    {"CrystalGolem_EnergyBulwark", 6, 500, 250},
    {"CrystalGolem_GroundSlam", 6, 500, 250},
    {"CrystalGolem_Shard_MeleeSwing", 6, 500, 250},
};
inline constexpr KitAbility kAbilities45[] = {
    {"BloodCultist_CorruptSacrifice", 2, 500, 250},
    {"BloodCultist_RottenEruption", 2, 500, 250},
    {"BloodCultist_SanguineBolt", 2, 500, 250},
};
inline constexpr KitAbility kAbilities46[] = {
    {"Deer_FrontKick", 145, 470, 214},
    {"Deer_HeadSwipe", 145, 334, 232},
    {"Deer_Headbutt", 145, 500, 250},
};
inline constexpr KitAbility kAbilities47[] = {
    {"GiantIguana_Poison_Bite", 11, 500, 250},
    {"GiantIguana_Poison_TailSplit", 11, 500, 250},
    {"GiantIguana_Poison_TailSwipe", 11, 500, 250},
    {"GiantIguana_Poison_Vomit", 11, 500, 250},
};
inline constexpr KitAbility kAbilities48[] = {
    {"Giant_BackSwipe", 3, 500, 250},
    {"Giant_BoulderToss", 3, 500, 250},
    {"Giant_CleavingSwipe", 3, 500, 250},
    {"Giant_Hammerfist", 3, 500, 250},
    {"Giant_Kick", 3, 500, 250},
    {"Giant_LeapSlam", 3, 500, 250},
    {"Giant_PunchCombo", 3, 500, 250},
};
inline constexpr KitAbility kAbilities49[] = {
    {"GoblinAssassin_Backstab", 3, 500, 250},
    {"GoblinAssassin_BasicMelee", 3, 500, 250},
    {"GoblinFlayer_ManicSneer", 3, 500, 250},
    {"Goblin_Kickflip", 3, 500, 250},
};
inline constexpr KitAbility kAbilities50[] = {
    {"Goblin_Headbutt", 13, 500, 250},
    {"Goblin_LeftSwing", 13, 369, 260},
    {"Goblin_Pummel", 13, 500, 250},
    {"Goblin_Rally", 13, 500, 250},
    {"Goblin_RightSwing", 13, 369, 260},
    {"Goblin_RockThrow", 13, 500, 250},
    {"Goblin_RockThrowHarass", 13, 500, 250},
    {"Goblin_StationaryHeadbutt", 13, 500, 250},
};
inline constexpr KitAbility kAbilities51[] = {
    {"Goblin_Headbutt", 7, 500, 250},
    {"Goblin_LeftSwing", 7, 369, 260},
    {"Goblin_Pummel", 7, 500, 250},
    {"Goblin_Rally", 7, 500, 250},
    {"Goblin_RightSwing", 7, 369, 260},
    {"Goblin_RockThrow", 7, 500, 250},
    {"Goblin_RockThrowHarass", 7, 500, 250},
    {"Goblin_StationaryHeadbutt", 7, 500, 250},
};
inline constexpr KitAbility kAbilities52[] = {
    {"BanditCaptain_BasicSwordAttack", 20, 500, 250},
    {"BanditCaptain_BasicSwordAttack02", 20, 500, 250},
    {"BanditCaptain_BasicSwordAttack03", 20, 500, 250},
    {"BanditCaptain_Tomahawk", 20, 500, 250},
};

}  // namespace aoc::worldd::combat::npc_kits
