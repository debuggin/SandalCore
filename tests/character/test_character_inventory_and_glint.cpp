// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "content/content_types_economy.h"
#include "items/currency.h"
#include "items/inventory.h"
#include "items/rarity.h"
#include "items/wallet.h"
#include "protocol/repl/vendor_codec.h"
#include "svc/services.h"
#include "vendor/sell_rules.h"
#include "vendor/town_shops.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace aoc::worldd;

constexpr int64_t kItemOre = 9001001;
constexpr int64_t kItemIngot = 9001002;
constexpr int64_t kItemBag = 9002001;
constexpr int64_t kItemSword = 9003001;
constexpr int64_t kItemPotion = 9004001;
constexpr int64_t kShopMarket = 7001001;
constexpr int64_t kShopInv = 7002001;

constexpr const char* kGuidOreA = "00000000000000000000000000000101";
constexpr const char* kGuidOreB = "00000000000000000000000000000102";
constexpr const char* kGuidBag = "00000000000000000000000000000201";
constexpr const char* kGuidSword = "00000000000000000000000000000301";
constexpr const char* kGuidIngot = "00000000000000000000000000000401";
constexpr const char* kGuidPotionA = "00000000000000000000000000000501";
constexpr const char* kGuidPotionB = "00000000000000000000000000000502";

class TestCatalog final : public items::ItemCatalog {
public:
    std::unordered_map<int64_t, items::ItemInfo> entries;
    std::optional<items::ItemInfo> info(int64_t record) const override {
        const auto it = entries.find(record);
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }
};

TestCatalog build_catalog() {
    TestCatalog cat;
    items::ItemInfo ore;
    ore.resource = true;
    ore.max_stack = 20;
    ore.inv_w = 1;
    ore.inv_h = 1;
    cat.entries[kItemOre] = ore;

    items::ItemInfo ingot;
    ingot.resource = true;
    ingot.max_stack = 20;
    ingot.inv_w = 1;
    ingot.inv_h = 1;
    cat.entries[kItemIngot] = ingot;

    items::ItemInfo bag;
    bag.equip_slots = {items::equip::ResourceBag0};
    bag.bag = items::ResourceGrid(4, 3, false);
    cat.entries[kItemBag] = bag;

    items::ItemInfo sword;
    sword.equip_slots = {items::equip::Primary};
    sword.max_stack = 1;
    cat.entries[kItemSword] = sword;

    items::ItemInfo potion;
    potion.max_stack = 10;
    cat.entries[kItemPotion] = potion;
    return cat;
}

items::ItemRarityFacts build_rarity_facts() {
    items::ItemRarityFacts rf;
    rf.has_curve = true;
    rf.rarity_min = 0;
    rf.rarity_max = items::kRarityCount - 1;
    for (int i = 0; i < items::kRarityCount; ++i) {
        rf.bands[static_cast<size_t>(i)] = {
            static_cast<double>(items::first_quality_of(i)),
            static_cast<double>(items::last_quality_of(i))};
    }
    return rf;
}

svc::InventoryItem make_item(
    svc::ItemContainer c, int32_t slot, int64_t rec, const char* guid, int32_t qty) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = rec;
    it.instance_guid = guid;
    it.quantity = qty;
    it.quality = 30;
    it.durability = 100.0f;
    return it;
}

bool test_bag_equip_and_stack_move(items::Inventory& inv, const TestCatalog& cat) {
    const auto eq = inv.use(svc::ItemContainer::Backpack, 0, kGuidBag, 0xFFFFFFFFu, cat);
    if (!eq.ok || !eq.equipment_changed || inv.grid(svc::ItemContainer::Resource0).cells() != 12) {
        return false;
    }

    const auto gr_ore = inv.grant(
        {make_item(svc::ItemContainer::Backpack, 0, kItemOre, kGuidOreA, 15),
         make_item(svc::ItemContainer::Backpack, 0, kItemOre, kGuidOreB, 8)},
        cat);
    if (!gr_ore.ok) return false;

    const auto* r0 = inv.at(svc::ItemContainer::Resource0, 0);
    const auto* r1 = inv.at(svc::ItemContainer::Resource0, 1);
    if (!r0 || r0->quantity != 20 || !r1 || r1->quantity != 3) return false;

    const auto mv_bad = inv.move(
        svc::ItemContainer::Backpack, 1, 1, kGuidSword, svc::ItemContainer::Resource0, 3, &cat);
    if (mv_bad.ok) return false;

    const auto mv_res = inv.move(
        svc::ItemContainer::Resource0, 1, 3, kGuidOreB, svc::ItemContainer::Resource0, 2, &cat);
    if (!mv_res.ok || !inv.at(svc::ItemContainer::Resource0, 2)) return false;

    const auto uneq_refused = inv.use(
        svc::ItemContainer::Equipment, items::equip::ResourceBag0, kGuidBag, 0xFFFFFFFFu, cat);
    return !uneq_refused.ok && uneq_refused.holds_items;
}

bool test_tier_up_and_grants(items::Inventory& inv, const TestCatalog& cat) {
    const auto rf = build_rarity_facts();
    if (rf.rarity_of(30.0) != 2) return false;
    if (inv.count_materials(kItemOre, rf, 0, 6) != 23) return false;

    svc::InventoryItem prod = make_item(svc::ItemContainer::Resource0, 0, kItemIngot, kGuidIngot, 1);
    prod.quality = 60;
    const auto tup = inv.tier_up(svc::ItemContainer::Resource0, 0, kGuidOreA, 20, prod, rf, cat);
    if (!tup.ok || inv.count_of(kItemIngot) != 1) return false;

    const auto cons = inv.consume(svc::ItemContainer::Resource0, 2, kGuidOreB, 2);
    if (!cons.ok || inv.at(svc::ItemContainer::Resource0, 2)->quantity != 1) return false;

    std::vector<svc::InventoryItem> gift = {
        make_item(svc::ItemContainer::Backpack, 0, kItemPotion, kGuidPotionA, 10),
        make_item(svc::ItemContainer::Backpack, 0, kItemPotion, kGuidPotionB, 5)};
    const auto gr = inv.grant_what_fits(gift, cat);
    if (!gr.outcome.ok || !gr.rest.empty() || inv.count_of(kItemPotion) != 15) return false;

    const auto tk = inv.take(kItemPotion, 12);
    if (!tk.ok || inv.count_of(kItemPotion) != 3) return false;

    const auto tmat = inv.take_materials(kItemOre, rf, 0, 6, 1, true);
    return tmat.ok && inv.count_materials(kItemOre, rf, 0, 6) == 0;
}

int run_inventory_bag_slot_stacking() {
    const TestCatalog cat = build_catalog();
    std::vector<svc::InventoryItem> init = {
        make_item(svc::ItemContainer::Backpack, 0, kItemBag, kGuidBag, 1),
        make_item(svc::ItemContainer::Backpack, 1, kItemSword, kGuidSword, 1)};
    std::string err;
    auto inv = items::Inventory::from_items(init, err, &cat);
    if (!inv || !err.empty()) return 1;
    if (!test_bag_equip_and_stack_move(*inv, cat)) return 1;
    if (!test_tier_up_and_grants(*inv, cat)) return 1;
    return 0;
}

content::CurrencyData build_glint_currency_data() {
    content::CurrencyData cd;
    cd.tiers.push_back({items::kMoneyTier, "Money", 999999999});
    cd.tiers.push_back({items::kGlintTier, "Glint", 999999999});
    cd.defs.push_back({101, "Copper", items::kMoneyTier, 0, 0, 100});
    cd.defs.push_back({201, "Glint_T1", items::kGlintTier, 0, items::kGlintT1Item, 10});
    cd.defs.push_back({202, "Glint_T2", items::kGlintTier, 1, items::kGlintT1Item + 1, 10});
    cd.defs.push_back({203, "Glint_T3", items::kGlintTier, 2, items::kGlintT1Item + 2, 10});
    return cd;
}

bool test_glint_and_wallet_math() {
    std::string err;
    const auto table = items::CurrencyTable::from(build_glint_currency_data(), err);
    if (!table || !err.empty()) return false;
    const auto* g1 = table->kind_of(items::kGlintT1Item);
    const auto* g2 = table->kind_of(items::kGlintT1Item + 1);
    const auto* g3 = table->kind_of(items::kGlintT1Item + 2);
    const auto* tier = table->tier_of(items::kGlintT1Item);
    if (!g1 || !g2 || !g3 || !tier || tier->guid != items::kGlintTier) return false;
    if (table->next_of(items::kGlintT1Item) != g2) return false;

    const auto parts = items::CurrencyTable::break_down(*tier, 247);
    if (parts.size() != 3 || parts[0].second != 2 || parts[1].second != 4 || parts[2].second != 7) {
        return false;
    }
    const auto comp = table->compress({{items::kGlintT1Item, 25}, {items::kGlintT1Item + 1, 8}});
    if (!comp.changed || comp.lines.size() != 2) return false;

    items::Wallet wallet({{items::kCopperCurrency, 5000}});
    if (wallet.copper() != 5000 || !wallet.can_pay(3500) || wallet.can_pay(6000)) return false;
    if (!wallet.pay(1200) || wallet.copper() != 3800) return false;
    const int64_t added = wallet.give(items::kMoneyCap);
    return added == (items::kMoneyCap - 3800) && wallet.copper() == items::kMoneyCap;
}

bool test_vendor_quote_sale_and_repair() {
    content::VendorData vd;
    content::VendorData::Def def;
    def.name = "Marketplace_Vendor";
    def.operations = vendor::kBuyBit | vendor::kSellBit | vendor::kRepairBit;
    def.tax_tag = "NodeTax.GoodsTax.MarketplaceTax";
    def.inventories = {kShopInv};
    vd.defs[kShopMarket] = def;

    content::VendorPriceRow row;
    row.inventory = kShopInv;
    row.ord = 0;
    row.item = kItemPotion;
    row.currency = items::kMoneyTier;
    row.price = 250;
    vd.prices[kShopMarket][kItemPotion].push_back(row);

    const vendor::TownShops shops(vd);
    if (!shops.is_open(kShopMarket)) return false;
    const auto tax = vendor::default_tax_rate("NodeTax.GoodsTax.MarketplaceTax");
    if (!tax.found || vendor::taxed_unit(250, tax) <= 250) return false;

    aoc::protocol::repl::VendorPurchaseCart cart;
    cart.vendor = kShopMarket;
    cart.items.push_back({kItemPotion, 3, std::nullopt, std::nullopt});
    const auto q = shops.quote(
        kShopMarket, cart, tax, [](const content::VendorPredicateRow&) { return true; });
    if (!q.ok || q.lines.size() != 1 || q.total != vendor::taxed_unit(250, tax) * 3) return false;

    content::ItemSaleFacts sf;
    sf.known = true;
    sf.vendorable = true;
    sf.level = 10;
    sf.values[{0, items::kMoneyTier}] = 400;
    const auto sq = vendor::quote_sale(vd, kShopMarket, kItemSword, 2, 30, sf, items::kMoneyTier, 10);
    if (!sq.ok || sq.unit != 400 || sq.total != 800) return false;

    const auto rq = vendor::quote_repair(
        vd, kShopMarket, kItemSword, 30, 40.0, 100.0, sf, items::kMoneyTier, tax, 10);
    return rq.ok && rq.before_tax == 400 && rq.total == vendor::taxed_unit(400, tax);
}

int run_vendor_glint_denomination_change() {
    if (!test_glint_and_wallet_math()) return 1;
    if (!test_vendor_quote_sale_and_repair()) return 1;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* case_name = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            case_name = argv[++i];
        }
    }
    if (!case_name) {
        if (run_inventory_bag_slot_stacking() != 0) return 1;
        if (run_vendor_glint_denomination_change() != 0) return 1;
        return 0;
    }
    if (std::strcmp(case_name, "inventory_bag_slot_stacking") == 0) {
        return run_inventory_bag_slot_stacking();
    }
    if (std::strcmp(case_name, "vendor_glint_denomination_change") == 0) {
        return run_vendor_glint_denomination_change();
    }
    std::fprintf(stderr, "Unknown --case %s\n", case_name);
    return 2;
}
