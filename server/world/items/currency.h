// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/currency.h: the kinds of a currency that are carried as items (the Glint certificates)
//
//  A currency tier is a row of kinds that make one another up: ten Dull Glint are worth one Dim Glint, ten Dim one Glowing, and so on up to Radiant (content.db currency_tier and
//  currency_def; the Money tier is copper, silver and gold, which are no items).  Each kind has a rate, how many of it make one of the next kind, and so a value in units of the
//  tier's smallest kind.  Two things use it:
//    * a full stack of one kind, used in a material bag, turns into one of the next kind (Inventory::tier_up);
//    * a reward of Glint is paid in kinds, the biggest first, each under its rate: the total of the tier's lines is split again (compress), the way the game's own reward step does it, so
//      a kill that rolls 27 Dull Glint gives 2 Dim and 7 Dull.
//
//  Pure data and arithmetic, header only: no sockets, no database but the CurrencyData it is made from.
// ============================================================================
#pragma once

#include "content/content_db.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aoc::worldd::items {

/// One kind of a tier.
struct CurrencyKind {
    int64_t def = 0;
    int64_t item = 0;     // the item record the kind is carried as (0: it is no item, a coin)
    int     ord = 0;      // 0 is the smallest kind
    int64_t rate = 0;     // how many of this kind make one of the next (the top kind's is not used)
    int64_t value = 1;    // what one is worth in units of the tier's smallest kind: 1, then the product of the rates below
};

struct CurrencyTier {
    int64_t                   guid = 0;
    std::string               name;
    int64_t                   max_base_quantity = 0;
    std::vector<CurrencyKind> kinds;     // by ord
    bool                      carried = false;   // every kind is an item: the tier can be split into the items
};

/// A line of a reward: an item and how many.
struct CurrencyLine {
    int64_t item = 0;
    int64_t quantity = 0;
};

/// A reward's lines after the currency lines were split again.
struct CompressedLines {
    struct Line {
        int64_t item = 0;
        int64_t quantity = 0;
        int     from = -1;    // the index of the line it was before, kept as it was; -1: a line the split made
    };
    std::vector<Line> lines;
    bool              changed = false;   // the lines differ from the ones given
};

class CurrencyTable {
public:
    /// The table the rows make.  nullopt, with `error` saying why, for a tier whose kinds do not run 0, 1, 2 ... without a gap, a kind below the top with a rate under 1, or a value that
    /// does not fit 64 bits.
    static std::optional<CurrencyTable> from(const content::CurrencyData& data, std::string& error) {
        CurrencyTable t;
        std::map<int64_t, size_t> at;
        for (const content::CurrencyTierRow& r : data.tiers) {
            CurrencyTier tier;
            tier.guid = r.guid;
            tier.name = r.name;
            tier.max_base_quantity = r.max_base_quantity;
            at[r.guid] = t.tiers_.size();
            t.tiers_.push_back(std::move(tier));
        }
        for (const content::CurrencyDefRow& d : data.defs) {
            if (d.tier == 0) continue;   // no tier lists the kind
            const auto it = at.find(d.tier);
            if (it == at.end()) {
                error = "the currency " + d.name + " is in the tier " + std::to_string(d.tier) + ", which content.db does not have";
                return std::nullopt;
            }
            CurrencyTier& tier = t.tiers_[it->second];
            if (d.ord != static_cast<int>(tier.kinds.size())) {
                error = "the tier " + tier.name + " has the currency " + d.name + " in place " + std::to_string(d.ord) + ", after " + std::to_string(tier.kinds.size()) + " kind(s)";
                return std::nullopt;
            }
            CurrencyKind k;
            k.def = d.guid;
            k.item = d.item;
            k.ord = d.ord;
            k.rate = d.rate;
            tier.kinds.push_back(k);
        }
        for (CurrencyTier& tier : t.tiers_) {
            for (size_t i = 0; i < tier.kinds.size(); ++i) {
                if (i == 0) {
                    tier.kinds[i].value = 1;
                    continue;
                }
                const CurrencyKind& below = tier.kinds[i - 1];
                if (below.rate < 1) {
                    error = "the tier " + tier.name + ": " + std::to_string(below.rate) + " of the kind in place " + std::to_string(i - 1) + " make one of the next";
                    return std::nullopt;
                }
                if (below.value > std::numeric_limits<int64_t>::max() / below.rate) {
                    error = "the tier " + tier.name + ": the kind in place " + std::to_string(i) + " is worth more than 64 bits hold";
                    return std::nullopt;
                }
                tier.kinds[i].value = below.value * below.rate;
            }
            tier.carried = !tier.kinds.empty() && std::all_of(tier.kinds.begin(), tier.kinds.end(), [](const CurrencyKind& k) { return k.item != 0; });
        }
        for (size_t i = 0; i < t.tiers_.size(); ++i) {
            for (size_t k = 0; k < t.tiers_[i].kinds.size(); ++k) {
                if (t.tiers_[i].kinds[k].item != 0) t.by_item_.emplace(t.tiers_[i].kinds[k].item, std::make_pair(i, k));
            }
        }
        return t;
    }

    bool empty() const { return by_item_.empty(); }
    const std::vector<CurrencyTier>& tiers() const { return tiers_; }

    /// The kind an item record is, nullptr for an item that is no currency kind (and for a coin).
    const CurrencyKind* kind_of(int64_t item) const {
        const auto it = by_item_.find(item);
        return it == by_item_.end() ? nullptr : &tiers_[it->second.first].kinds[it->second.second];
    }
    /// The tier the item's kind belongs to, nullptr when it has none.
    const CurrencyTier* tier_of(int64_t item) const {
        const auto it = by_item_.find(item);
        return it == by_item_.end() ? nullptr : &tiers_[it->second.first];
    }
    /// The next kind up from the item's kind, as an item: nullptr for the top kind, for an item that is no kind, and for a kind whose next is no item.
    const CurrencyKind* next_of(int64_t item) const {
        const auto it = by_item_.find(item);
        if (it == by_item_.end()) return nullptr;
        const CurrencyTier& tier = tiers_[it->second.first];
        const size_t next = it->second.second + 1;
        return next < tier.kinds.size() && tier.kinds[next].item != 0 ? &tier.kinds[next] : nullptr;
    }

    /// `total` units of the tier's smallest kind in kinds, the biggest kind first: each kind takes as many as it holds of what is left, a kind below the top never more than its rate (the top
    /// kind has no limit) and the rest goes down.  Only the kinds that get any are listed.
    static std::vector<std::pair<const CurrencyKind*, int64_t>> break_down(const CurrencyTier& tier, int64_t total) {
        std::vector<std::pair<const CurrencyKind*, int64_t>> out;
        int64_t left = total;
        for (size_t k = tier.kinds.size(); k-- > 0 && left > 0;) {
            const CurrencyKind& kind = tier.kinds[k];
            int64_t n = left / kind.value;
            if (k + 1 != tier.kinds.size() && kind.rate > 0 && n > kind.rate) n = kind.rate;
            if (n <= 0) continue;
            out.emplace_back(&kind, n);
            left -= n * kind.value;
        }
        return out;
    }

    /// The lines of a reward with the currency lines of each tier made one: the lines of the tier's kinds (a tier whose kinds are all items) are summed in units of its smallest kind and
    /// split again by break_down, and the result takes the place of the tier's first line; the other lines stay where they are.  A line with no quantity, one that is no currency kind
    /// and one whose sum does not fit 64 bits stay as they are.
    CompressedLines compress(const std::vector<CurrencyLine>& lines) const {
        CompressedLines out;
        std::map<size_t, int64_t> total;       // tier -> units of its smallest kind
        std::map<size_t, size_t> first;        // tier -> index of its first line
        std::vector<int> tier_of_line(lines.size(), -1);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].quantity <= 0) continue;
            const auto it = by_item_.find(lines[i].item);
            if (it == by_item_.end() || !tiers_[it->second.first].carried) continue;
            const CurrencyKind& kind = tiers_[it->second.first].kinds[it->second.second];
            int64_t& sum = total[it->second.first];
            if (lines[i].quantity > std::numeric_limits<int64_t>::max() / kind.value || sum > std::numeric_limits<int64_t>::max() - lines[i].quantity * kind.value) continue;
            sum += lines[i].quantity * kind.value;
            first.emplace(it->second.first, i);
            tier_of_line[i] = static_cast<int>(it->second.first);
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            if (tier_of_line[i] < 0) {
                out.lines.push_back({lines[i].item, lines[i].quantity, static_cast<int>(i)});
                continue;
            }
            const size_t tier = static_cast<size_t>(tier_of_line[i]);
            if (first[tier] != i) continue;   // the tier's later lines are in the first one's split
            for (const auto& [kind, n] : break_down(tiers_[tier], total[tier])) out.lines.push_back({kind->item, n, -1});
        }
        out.changed = out.lines.size() != lines.size();
        for (size_t i = 0; !out.changed && i < lines.size(); ++i) {
            out.changed = out.lines[i].item != lines[i].item || out.lines[i].quantity != lines[i].quantity;
        }
        return out;
    }

private:
    std::vector<CurrencyTier>                                   tiers_;
    std::unordered_map<int64_t, std::pair<size_t, size_t>>     by_item_;   // item record -> (tier, kind)
};

[[nodiscard]] inline constexpr bool is_valid_currency_id(int64_t currency_id) noexcept {
    return currency_id > 0;
}

[[nodiscard]] inline std::optional<int64_t> glint_tier_value(
    const CurrencyTable& table, int64_t item_id, int64_t quantity = 1) noexcept {
    if (quantity < 0) return std::nullopt;
    const CurrencyTier* tier = table.tier_of(item_id);
    const CurrencyKind* kind = table.kind_of(item_id);
    if (!tier || !tier->carried || !kind || kind->value <= 0) return std::nullopt;
    if (quantity == 0) return 0;
    if (quantity > std::numeric_limits<int64_t>::max() / kind->value) return std::nullopt;
    return quantity * kind->value;
}

[[nodiscard]] inline constexpr std::optional<int64_t> glint_tier_value(
    int tier_ord, int64_t quantity = 1, int64_t rate = 10) noexcept {
    if (tier_ord < 0 || tier_ord > 18 || quantity < 0 || rate < 1) return std::nullopt;
    if (quantity == 0) return 0;
    int64_t unit = 1;
    for (int i = 0; i < tier_ord; ++i) {
        if (unit > std::numeric_limits<int64_t>::max() / rate) return std::nullopt;
        unit *= rate;
    }
    if (quantity > std::numeric_limits<int64_t>::max() / unit) return std::nullopt;
    return quantity * unit;
}

[[nodiscard]] inline constexpr bool try_credit_currency(
    int64_t currency_id, int64_t& balance, int64_t amount,
    int64_t max_cap = std::numeric_limits<int64_t>::max()) noexcept {
    if (!is_valid_currency_id(currency_id) || balance < 0 || amount < 0 || max_cap < 0) return false;
    if (balance > max_cap || amount > std::numeric_limits<int64_t>::max() - balance) return false;
    const int64_t next = balance + amount;
    if (next > max_cap) return false;
    balance = next;
    return true;
}

[[nodiscard]] inline constexpr bool try_credit_currency(
    int64_t currency_id, uint64_t& balance, uint64_t amount,
    uint64_t max_cap = std::numeric_limits<uint64_t>::max()) noexcept {
    if (!is_valid_currency_id(currency_id) || balance > max_cap) return false;
    if (amount > std::numeric_limits<uint64_t>::max() - balance) return false;
    const uint64_t next = balance + amount;
    if (next > max_cap) return false;
    balance = next;
    return true;
}

[[nodiscard]] inline bool try_credit_currency(
    std::map<int64_t, int64_t>& ledger, int64_t currency_id, int64_t amount,
    int64_t max_cap = std::numeric_limits<int64_t>::max()) {
    const auto it = ledger.find(currency_id);
    int64_t current = (it == ledger.end()) ? 0 : it->second;
    if (!try_credit_currency(currency_id, current, amount, max_cap)) return false;
    ledger[currency_id] = current;
    return true;
}

[[nodiscard]] inline constexpr bool try_debit_currency(
    int64_t currency_id, int64_t& balance, int64_t amount) noexcept {
    if (!is_valid_currency_id(currency_id) || balance < 0 || amount < 0 || amount > balance) {
        return false;
    }
    balance -= amount;
    return true;
}

[[nodiscard]] inline constexpr bool try_debit_currency(
    int64_t currency_id, uint64_t& balance, uint64_t amount) noexcept {
    if (!is_valid_currency_id(currency_id) || amount > balance) return false;
    balance -= amount;
    return true;
}

[[nodiscard]] inline bool try_debit_currency(
    std::map<int64_t, int64_t>& ledger, int64_t currency_id, int64_t amount) {
    const auto it = ledger.find(currency_id);
    int64_t current = (it == ledger.end()) ? 0 : it->second;
    if (!try_debit_currency(currency_id, current, amount)) return false;
    if (current == 0 && it != ledger.end()) {
        it->second = 0;
    } else if (current > 0) {
        ledger[currency_id] = current;
    }
    return true;
}

[[nodiscard]] inline constexpr bool try_transfer_currency(
    int64_t currency_id, int64_t& from_balance, int64_t& to_balance, int64_t amount,
    int64_t max_cap = std::numeric_limits<int64_t>::max()) noexcept {
    if (&from_balance == &to_balance) {
        return is_valid_currency_id(currency_id) && from_balance >= 0 && amount >= 0 &&
               amount <= from_balance && from_balance <= max_cap;
    }
    int64_t next_from = from_balance;
    int64_t next_to = to_balance;
    if (!try_debit_currency(currency_id, next_from, amount)) return false;
    if (!try_credit_currency(currency_id, next_to, amount, max_cap)) return false;
    from_balance = next_from;
    to_balance = next_to;
    return true;
}

[[nodiscard]] inline constexpr bool try_transfer_currency(
    int64_t currency_id, uint64_t& from_balance, uint64_t& to_balance, uint64_t amount,
    uint64_t max_cap = std::numeric_limits<uint64_t>::max()) noexcept {
    if (&from_balance == &to_balance) {
        return is_valid_currency_id(currency_id) && amount <= from_balance &&
               from_balance <= max_cap;
    }
    uint64_t next_from = from_balance;
    uint64_t next_to = to_balance;
    if (!try_debit_currency(currency_id, next_from, amount)) return false;
    if (!try_credit_currency(currency_id, next_to, amount, max_cap)) return false;
    from_balance = next_from;
    to_balance = next_to;
    return true;
}

}  // namespace aoc::worldd::items
