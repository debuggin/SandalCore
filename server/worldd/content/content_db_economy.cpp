// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "content/content_db_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace aoc::worldd::content {

namespace {

constexpr const char* kPercentStatsSql = "SELECT guid FROM stat_def WHERE display_pct = 1";
constexpr const char* kEquippedEffectSql = "SELECT effect_guid, min_rarity, max_rarity, stacks FROM item_equipped_effect WHERE item_guid = ?1 ORDER BY ord";

std::vector<int64_t> int_list(const nlohmann::json& j, const char* key) {
    std::vector<int64_t> out;
    const auto it = j.find(key);
    if (it != j.end() && it->is_array()) {
        for (const auto& v : *it) {
            out.push_back(v.is_number() ? v.get<int64_t>() : 0);
        }
    }
    return out;
}

std::string lower_case(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

TagQuery parse_tag_query(const std::string& text) {
    TagQuery q;
    if (text.empty()) {
        return q;
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return q;
    }
    q.tok = int_list(j, "tok");
    const auto it = j.find("tags");
    if (it != j.end() && it->is_array()) {
        for (const auto& v : *it) {
            q.tags.push_back(v.is_string() ? lower_case(v.get<std::string>()) : std::string());
        }
    }
    q.present = q.tok.size() >= 2 && q.tok[1] != 0;
    return q;
}

std::vector<int64_t> int_array(const std::string& text) {
    std::vector<int64_t> out;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        return out;
    }
    for (const auto& v : j) {
        out.push_back(v.is_number() ? v.get<int64_t>() : 0);
    }
    return out;
}

bool parse_warehouse_cost_item(const nlohmann::json& c, WarehouseCostRow& r) {
    if (!c.is_object()) {
        return false;
    }
    if (const auto it = c.find("c"); it != c.end() && it->is_number_integer()) {
        r.currency = it->get<int64_t>();
    }
    if (const auto it = c.find("ty"); it != c.end() && it->is_number_integer()) {
        r.type_id = it->get<int64_t>();
    }
    if (const auto it = c.find("v"); it != c.end() && it->is_string()) {
        r.value = it->get<std::string>();
    }
    if (const auto it = c.find("m"); it != c.end() && it->is_string()) {
        r.multiplier = it->get<std::string>();
    }
    return true;
}

bool parse_warehouse_costs(const std::string& text, std::vector<WarehouseCostRow>& out) {
    out.clear();
    if (text.empty()) {
        return true;
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        return false;
    }
    for (const auto& c : j) {
        WarehouseCostRow r;
        if (!parse_warehouse_cost_item(c, r)) {
            return false;
        }
        out.push_back(std::move(r));
    }
    return true;
}

bool warehouse_failed(sqlite3* db, const char* table, std::string& error) {
    error = std::string("content.db ") + table + " query failed: " + db_message(db) +
            " (the warehouse tables are contentc version 25's: rebuild the file with this checkout's contentc)";
    return false;
}

bool warehouse_unread(const char* table, int64_t guid, std::string& error) {
    error = std::string("content.db ") + table + " row " + std::to_string(guid) + " does not read";
    return false;
}

bool cols_are_int(sqlite3_stmt* s, int count) {
    for (int c = 0; c < count; ++c) {
        if (!column_is_int(s, c)) {
            return false;
        }
    }
    return true;
}

bool load_warehouse_nodes_and_defs(sqlite3* db, WarehouseData& out, std::string& error) {
    {
        Stmt q(db, "SELECT node_guid, resource_def, crate_def, chunks_def FROM warehouse_node");
        if (!q.ok()) {
            return warehouse_failed(db, "warehouse_node", error);
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!cols_are_int(s, 4)) {
                return warehouse_unread("warehouse_node", 0, error);
            }
            out.nodes[sqlite3_column_int64(s, 0)] = {sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2), sqlite3_column_int64(s, 3)};
        }
        if (step != SQLITE_DONE) {
            return warehouse_failed(db, "warehouse_node", error);
        }
    }
    Stmt q(db, "SELECT guid, name, columns, rows FROM warehouse_def");
    if (!q.ok()) {
        return warehouse_failed(db, "warehouse_def", error);
    }
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 2) || !column_is_int(s, 3)) {
            return warehouse_unread("warehouse_def", 0, error);
        }
        WarehouseDefRow d{sqlite3_column_int64(s, 0), column_text(s, 1), static_cast<int>(sqlite3_column_int64(s, 2)), static_cast<int>(sqlite3_column_int64(s, 3)), {}};
        out.defs[d.guid] = std::move(d);
    }
    return step == SQLITE_DONE ? true : warehouse_failed(db, "warehouse_def", error);
}

bool load_warehouse_def_tabs_and_tabs(sqlite3* db, WarehouseData& out, std::string& error) {
    {
        Stmt q(db, "SELECT def_guid, tab_guid FROM warehouse_def_tab ORDER BY def_guid, ord");
        if (!q.ok()) {
            return warehouse_failed(db, "warehouse_def_tab", error);
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!cols_are_int(s, 2)) {
                return warehouse_unread("warehouse_def_tab", 0, error);
            }
            const auto it = out.defs.find(sqlite3_column_int64(s, 0));
            if (it != out.defs.end()) {
                it->second.tabs.push_back(sqlite3_column_int64(s, 1));
            }
        }
        if (step != SQLITE_DONE) {
            return warehouse_failed(db, "warehouse_def_tab", error);
        }
    }
    Stmt q(db, "SELECT guid, name, storage_type, member, specializable, requirement, costs FROM warehouse_tab");
    if (!q.ok()) {
        return warehouse_failed(db, "warehouse_tab", error);
    }
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 4)) {
            return warehouse_unread("warehouse_tab", 0, error);
        }
        WarehouseTabRow t{sqlite3_column_int64(s, 0), column_text(s, 1), static_cast<int>(sqlite3_column_int64(s, 2)),
                          sqlite3_column_int64(s, 3) != 0, sqlite3_column_int64(s, 4) != 0, column_text(s, 5), {}};
        if (!parse_warehouse_costs(column_text(s, 6), t.costs)) {
            return warehouse_unread("warehouse_tab", t.guid, error);
        }
        out.tabs[t.guid] = std::move(t);
    }
    return step == SQLITE_DONE ? true : warehouse_failed(db, "warehouse_tab", error);
}

bool load_warehouse_chunks(sqlite3* db, WarehouseData& out, std::string& error) {
    {
        Stmt q(db, "SELECT def_guid, chunk_guid FROM warehouse_chunks_def_chunk ORDER BY def_guid, ord");
        if (!q.ok()) {
            return warehouse_failed(db, "warehouse_chunks_def_chunk", error);
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!cols_are_int(s, 2)) {
                return warehouse_unread("warehouse_chunks_def_chunk", 0, error);
            }
            out.chunk_defs[sqlite3_column_int64(s, 0)].push_back(sqlite3_column_int64(s, 1));
        }
        if (step != SQLITE_DONE) {
            return warehouse_failed(db, "warehouse_chunks_def_chunk", error);
        }
    }
    Stmt q(db, "SELECT guid, name, size, level, requirement, costs FROM warehouse_chunk");
    if (!q.ok()) {
        return warehouse_failed(db, "warehouse_chunk", error);
    }
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 2) || !column_is_int(s, 3)) {
            return warehouse_unread("warehouse_chunk", 0, error);
        }
        WarehouseChunkRow c{sqlite3_column_int64(s, 0), column_text(s, 1), static_cast<int>(sqlite3_column_int64(s, 2)),
                            static_cast<int>(sqlite3_column_int64(s, 3)), column_text(s, 4), {}};
        if (!parse_warehouse_costs(column_text(s, 5), c.costs)) {
            return warehouse_unread("warehouse_chunk", c.guid, error);
        }
        out.chunks[c.guid] = std::move(c);
    }
    return step == SQLITE_DONE ? true : warehouse_failed(db, "warehouse_chunk", error);
}

bool vendor_data_failed(sqlite3* db, const char* table, std::string& error) {
    error = std::string("content.db ") + table + " query failed: " + db_message(db) +
            " (the vendor tables are contentc version 18's: rebuild the file with this checkout's contentc)";
    return false;
}

bool vendor_data_unread(const char* table, std::string& error) {
    error = std::string("content.db ") + table + " has a column of the wrong storage class";
    return false;
}

bool load_vendor_defs_and_links(sqlite3* db, VendorData& out, std::string& error) {
    {
        Stmt q(db, "SELECT d.guid, d.name, d.operations, COALESCE(x.tax_tag, ''), COALESCE(x.blacklisted, 0) FROM vendor_def d LEFT JOIN vendor_def_data x ON x.vendor_guid = d.guid");
        if (!q.ok()) {
            return vendor_data_failed(db, "vendor_def", error);
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 2)) {
                return vendor_data_unread("vendor_def", error);
            }
            VendorData::Def d;
            d.name = column_text(s, 1);
            d.operations = sqlite3_column_int64(s, 2);
            d.tax_tag = column_text(s, 3);
            d.blacklisted = sqlite3_column_int64(s, 4) != 0;
            out.defs[sqlite3_column_int64(s, 0)] = std::move(d);
        }
        if (step != SQLITE_DONE) {
            return vendor_data_failed(db, "vendor_def", error);
        }
    }
    Stmt q(db, "SELECT source_kind, source_guid, vendor_guid FROM npc_vendor_link");
    if (!q.ok()) {
        return vendor_data_failed(db, "npc_vendor_link", error);
    }
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 1) || !column_is_int(s, 2)) {
            return vendor_data_unread("npc_vendor_link", error);
        }
        (column_text(s, 0) == "instance" ? out.instance_links : out.asset_links)[sqlite3_column_int64(s, 1)] = sqlite3_column_int64(s, 2);
    }
    return step == SQLITE_DONE ? true : vendor_data_failed(db, "npc_vendor_link", error);
}

bool load_vendor_prices(sqlite3* db, VendorData& out, std::string& error) {
    Stmt q(db, "SELECT vendor_guid, inventory_guid, ord, item_guid, variation_guid, offer_guid, currency_guid, price "
               "FROM vendor_price ORDER BY vendor_guid, inventory_guid, ord, offer_guid, currency_guid");
    if (!q.ok()) {
        return vendor_data_failed(db, "vendor_price", error);
    }
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!cols_are_int(s, 8)) {
            return vendor_data_unread("vendor_price", error);
        }
        VendorPriceRow r{sqlite3_column_int64(s, 1), static_cast<int>(sqlite3_column_int64(s, 2)), sqlite3_column_int64(s, 3),
                         sqlite3_column_int64(s, 4), sqlite3_column_int64(s, 5), sqlite3_column_int64(s, 6), sqlite3_column_int64(s, 7)};
        out.prices[sqlite3_column_int64(s, 0)][r.item].push_back(r);
    }
    return step == SQLITE_DONE ? true : vendor_data_failed(db, "vendor_price", error);
}

bool load_vendor_inventories_and_avail(sqlite3* db, VendorData& out, std::string& error) {
    {
        Stmt q(db, "SELECT vendor_guid, inventory_guid FROM vendor_def_inventory ORDER BY vendor_guid, ord");
        if (!q.ok()) {
            return vendor_data_failed(db, "vendor_def_inventory", error);
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!cols_are_int(s, 2)) {
                return vendor_data_unread("vendor_def_inventory", error);
            }
            const auto def = out.defs.find(sqlite3_column_int64(s, 0));
            if (def != out.defs.end()) {
                def->second.inventories.push_back(sqlite3_column_int64(s, 1));
            }
        }
        if (step != SQLITE_DONE) {
            return vendor_data_failed(db, "vendor_def_inventory", error);
        }
    }
    Stmt q(db, "SELECT inventory_guid, listing_ord, expression, failure, hidden FROM vendor_availability ORDER BY inventory_guid, listing_ord, ord");
    if (!q.ok()) {
        return vendor_data_failed(db, "vendor_availability", error);
    }
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!cols_are_int(s, 2)) {
            return vendor_data_unread("vendor_availability", error);
        }
        VendorPredicateRow p{column_text(s, 2), column_text(s, 3), sqlite3_column_int64(s, 4) != 0};
        out.predicates[{sqlite3_column_int64(s, 0), static_cast<int>(sqlite3_column_int64(s, 1))}].push_back(std::move(p));
    }
    return step == SQLITE_DONE ? true : vendor_data_failed(db, "vendor_availability", error);
}

void parse_vendor_modifier_curve(const std::string& text, std::vector<VendorModifier::Key>& out) {
    const nlohmann::json curve = nlohmann::json::parse(text, nullptr, false);
    if (!curve.is_array()) {
        return;
    }
    for (const auto& k : curve) {
        if (k.is_array() && k.size() >= 3 && k[0].is_number() && k[1].is_number() && k[2].is_string()) {
            out.push_back({k[0].get<double>(), k[1].get<double>(), k[2].get<std::string>()});
        }
    }
}

bool load_vendor_modifiers_and_filters(sqlite3* db, VendorData& out, std::string& error) {
    {
        Stmt q(db, "SELECT guid, name, contexts, operation, source, factor, expression, curve, filter, currencies, children FROM vendor_modifier");
        if (!q.ok()) {
            return vendor_data_failed(db, "vendor_modifier", error);
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 2)) {
                return vendor_data_unread("vendor_modifier", error);
            }
            VendorModifier m;
            m.guid = sqlite3_column_int64(s, 0);
            m.name = column_text(s, 1);
            m.contexts = sqlite3_column_int64(s, 2);
            m.operation = column_text(s, 3);
            m.source = column_text(s, 4);
            m.factor = column_text(s, 5);
            m.expression = column_text(s, 6);
            parse_vendor_modifier_curve(column_text(s, 7), m.curve);
            m.filter = parse_tag_query(column_text(s, 8));
            m.currencies = int_array(column_text(s, 9));
            m.children = int_array(column_text(s, 10));
            out.modifiers[m.guid] = std::move(m);
        }
        if (step != SQLITE_DONE) {
            return vendor_data_failed(db, "vendor_modifier", error);
        }
    }
    Stmt q(db, "SELECT guid, filter, whitelist FROM vendor_sell_filter");
    if (!q.ok()) {
        return vendor_data_failed(db, "vendor_sell_filter", error);
    }
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0)) {
            return vendor_data_unread("vendor_sell_filter", error);
        }
        VendorSellFilter f;
        f.filter = parse_tag_query(column_text(s, 1));
        for (const int64_t item : int_array(column_text(s, 2))) {
            f.whitelist.insert(item);
        }
        out.sell_filters[sqlite3_column_int64(s, 0)] = std::move(f);
    }
    return step == SQLITE_DONE ? true : vendor_data_failed(db, "vendor_sell_filter", error);
}

bool load_vendor_chains(sqlite3* db, VendorData& out, std::string& error) {
    struct Chain {
        const char* table;
        const char* sql;
        bool modifiers;
    };
    const Chain chains[] = {
        {"vendor_def_modifier", "SELECT vendor_guid, modifier_guid FROM vendor_def_modifier ORDER BY vendor_guid, ord", true},
        {"vendor_def_sell_filter", "SELECT vendor_guid, filter_guid FROM vendor_def_sell_filter ORDER BY vendor_guid, ord", false},
    };
    for (const Chain& chain : chains) {
        Stmt q(db, chain.sql);
        if (!q.ok()) {
            return vendor_data_failed(db, chain.table, error);
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!cols_are_int(s, 2)) {
                return vendor_data_unread(chain.table, error);
            }
            const auto def = out.defs.find(sqlite3_column_int64(s, 0));
            if (def != out.defs.end()) {
                (chain.modifiers ? def->second.modifiers : def->second.sell_filters).push_back(sqlite3_column_int64(s, 1));
            }
        }
        if (step != SQLITE_DONE) {
            return vendor_data_failed(db, chain.table, error);
        }
    }
    return true;
}

int load_item_vendor_row(sqlite3* db, int64_t item, ItemSaleFacts& out, int64_t& curve, std::string& error) {
    const std::string rebuild = " (the sale tables are contentc version 19's: rebuild the file with this checkout's contentc)";
    Stmt q(db, "SELECT v.vendorable, v.blacklisted, v.rarity_min, v.rarity_max, i.level, i.quality_curve_guid FROM item_vendor v JOIN item i ON i.guid = v.item_guid WHERE v.item_guid = ?1");
    if (!q.ok()) {
        error = "content.db item_vendor query failed: " + db_message(db) + rebuild;
        return -1;
    }
    sqlite3_bind_int64(q.get(), 1, item);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) {
        return 0;
    }
    if (step != SQLITE_ROW) {
        error = "content.db item_vendor query failed: " + db_message(db);
        return -1;
    }
    if (!cols_are_int(q.get(), 6)) {
        error = "content.db item_vendor of item " + std::to_string(item) + " has a column of the wrong storage class";
        return -1;
    }
    out.known = true;
    out.vendorable = sqlite3_column_int64(q.get(), 0) != 0;
    out.blacklisted = sqlite3_column_int64(q.get(), 1) != 0;
    out.rarity_min = static_cast<int>(std::clamp<int64_t>(sqlite3_column_int64(q.get(), 2), 0, 6));
    out.rarity_max = static_cast<int>(std::clamp<int64_t>(sqlite3_column_int64(q.get(), 3), 0, 6));
    out.level = static_cast<int>(sqlite3_column_int64(q.get(), 4));
    curve = sqlite3_column_int64(q.get(), 5);
    return 1;
}

void insert_tag_ancestors(std::string tag, std::set<std::string>& tags) {
    while (!tag.empty()) {
        tags.insert(tag);
        const size_t dot = tag.rfind('.');
        if (dot == std::string::npos) {
            break;
        }
        tag.resize(dot);
    }
}

bool load_item_sale_tags(sqlite3* db, int64_t item, ItemSaleFacts& out, std::string& error) {
    Stmt q(db, "SELECT tag FROM item_tag WHERE item_guid = ?1");
    if (!q.ok()) {
        error = "content.db item_tag query failed: " + db_message(db);
        return false;
    }
    sqlite3_bind_int64(q.get(), 1, item);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        insert_tag_ancestors(lower_case(column_text(q.get(), 0)), out.tags);
    }
    if (step != SQLITE_DONE) {
        error = "content.db item_tag query failed: " + db_message(db);
        return false;
    }
    return true;
}

bool load_item_sale_curve_and_values(sqlite3* db, int64_t item, int64_t curve, ItemSaleFacts& out, std::string& error) {
    if (curve != 0) {
        Stmt q(db, "SELECT x, y FROM quality_curve WHERE guid = ?1 ORDER BY ord");
        if (!q.ok()) {
            error = "content.db quality_curve query failed: " + db_message(db);
            return false;
        }
        sqlite3_bind_int64(q.get(), 1, curve);
        int step = SQLITE_OK;
        int rows = 0;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (rows < items::kRarityCount) {
                out.bands[static_cast<size_t>(rows)] = {sqlite3_column_double(q.get(), 0), sqlite3_column_double(q.get(), 1)};
            }
            ++rows;
        }
        if (step != SQLITE_DONE) {
            error = "content.db quality_curve query failed: " + db_message(db);
            return false;
        }
        out.has_curve = rows == items::kRarityCount;
    }
    const std::string rebuild = " (the sale tables are contentc version 19's: rebuild the file with this checkout's contentc)";
    Stmt q(db, "SELECT rarity, currency_guid, price FROM item_sell_value WHERE item_guid = ?1");
    if (!q.ok()) {
        error = "content.db item_sell_value query failed: " + db_message(db) + rebuild;
        return false;
    }
    sqlite3_bind_int64(q.get(), 1, item);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        if (!cols_are_int(q.get(), 3)) {
            error = "content.db item_sell_value of item " + std::to_string(item) + " has a column of the wrong storage class";
            return false;
        }
        out.values[{static_cast<int>(sqlite3_column_int64(q.get(), 0)), sqlite3_column_int64(q.get(), 1)}] = sqlite3_column_int64(q.get(), 2);
    }
    if (step != SQLITE_DONE) {
        error = "content.db item_sell_value query failed: " + db_message(db);
        return false;
    }
    return true;
}

}  // namespace

std::optional<WarehouseData> ContentDb::warehouse_data(std::string& error) const {
    WarehouseData out;
    if (!load_warehouse_nodes_and_defs(db_, out, error) || !load_warehouse_def_tabs_and_tabs(db_, out, error) || !load_warehouse_chunks(db_, out, error)) {
        return std::nullopt;
    }
    return out;
}

std::optional<VendorData> ContentDb::vendor_data(std::string& error) const {
    VendorData out;
    if (!load_vendor_defs_and_links(db_, out, error) || !load_vendor_prices(db_, out, error) ||
        !load_vendor_inventories_and_avail(db_, out, error) || !load_vendor_modifiers_and_filters(db_, out, error) ||
        !load_vendor_chains(db_, out, error)) {
        return std::nullopt;
    }
    return out;
}

void ContentDb::load_percent_stats() {
    percent_stats_.clear();
    Stmt q(db_, kPercentStatsSql);
    if (!q.ok()) {
        return;
    }
    while (sqlite3_step(q.get()) == SQLITE_ROW) {
        if (column_is_int(q.get(), 0)) {
            percent_stats_.insert(sqlite3_column_int64(q.get(), 0));
        }
    }
}

std::optional<std::vector<EquippedEffect>> ContentDb::item_equipped_effects(int64_t item, std::string& error) const {
    error.clear();
    std::vector<EquippedEffect> out;
    Stmt q(db_, kEquippedEffectSql);
    if (!q.ok()) {
        return out;
    }
    sqlite3_bind_int64(q.get(), 1, item);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        if (!cols_are_int(q.get(), 4)) {
            error = "content.db item_equipped_effect of item " + std::to_string(item) + " has a column of the wrong storage class";
            return std::nullopt;
        }
        EquippedEffect e;
        e.effect = sqlite3_column_int64(q.get(), 0);
        e.min_rarity = static_cast<int>(std::clamp<int64_t>(sqlite3_column_int64(q.get(), 1), 0, 6));
        e.max_rarity = static_cast<int>(std::clamp<int64_t>(sqlite3_column_int64(q.get(), 2), 0, 6));
        e.stacks = static_cast<int>(std::max<int64_t>(1, sqlite3_column_int64(q.get(), 3)));
        out.push_back(e);
    }
    if (step != SQLITE_DONE) {
        error = "content.db item_equipped_effect query failed: " + db_message(db_);
        return std::nullopt;
    }
    return out;
}

std::optional<ItemSaleFacts> ContentDb::item_sale(int64_t item, std::string& error) const {
    error.clear();
    ItemSaleFacts out;
    int64_t curve = 0;
    const int status = load_item_vendor_row(db_, item, out, curve, error);
    if (status < 0) {
        return std::nullopt;
    }
    if (status == 0) {
        return out;
    }
    if (!load_item_sale_tags(db_, item, out, error) || !load_item_sale_curve_and_values(db_, item, curve, out, error)) {
        return std::nullopt;
    }
    return out;
}

ContentGearCatalog::ContentGearCatalog(const ContentDb& db) : db_(db) {
    if (auto names = db.equip_slot_names(error_)) {
        slots_ = std::move(*names);
    }
}

const stats::ItemGear* ContentGearCatalog::item(int64_t record) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = items_.find(record); it != items_.end()) {
        return it->second.get();
    }
    std::string error;
    auto g = db_.item_gear(record, error);
    if (!g) {
        if (!error.empty()) {
            spdlog::error("[content] {}", error);
        }
        return nullptr;
    }
    auto& slot = items_[record];
    slot = std::make_unique<stats::ItemGear>(std::move(*g));
    return slot.get();
}

const stats::SetBonus* ContentGearCatalog::set(int64_t guid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = sets_.find(guid); it != sets_.end()) {
        return it->second.get();
    }
    std::string error;
    auto s = db_.set_bonus(guid, error);
    if (!s) {
        if (!error.empty()) {
            spdlog::error("[content] {}", error);
        }
        return nullptr;
    }
    auto& slot = sets_[guid];
    slot = std::make_unique<stats::SetBonus>(std::move(*s));
    return slot.get();
}

std::string ContentGearCatalog::slot_name(int slot) const {
    const auto it = slots_.find(slot);
    return it != slots_.end() ? it->second : "Slot" + std::to_string(slot);
}

}  // namespace aoc::worldd::content
