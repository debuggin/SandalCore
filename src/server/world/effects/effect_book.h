// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// effects/effect_book.h: actor effect book (OverrideAlways, StackInPlace, StackSeperate, OverrideDuration, ExtendDuration, NeverRefresh).
#pragma once

#include "session/ability_catalog.h"
#include "stats/stat_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace aoc::worldd::effects {

inline constexpr double kForever = std::numeric_limits<double>::infinity();

struct Applied {
    const abilities::EffectDef*   def = nullptr;
    int                           stacks = 1;            // 1 or more
    double                        duration = kForever;   // seconds; kForever: it does not end by itself
    int                           in_place_limit = 1;    // the most stacks one instance holds (StackInPlace)
    std::vector<stats::EffectMod> mods;                  // one stack's stat mod values
    double                        extension = -1;        // seconds one more application adds under ExtendDuration; negative: refreshed in full
    double                        extended_max = 0;      // the most seconds an extended instance has left; 0: no limit
};

struct Instance {
    int64_t                       key = 0;               // what the replicated item is known by (EffectId)
    const abilities::EffectDef*   def = nullptr;
    int                           stacks = 1;
    double                        start = 0, end = kForever;
    double                        next_tick = kForever;  // kForever: it does not tick
    std::vector<stats::EffectMod> mods;
    uint64_t                      instigator = 0;        // the actor that applied it (the one it is shown to be from); 0: none
    uint64_t                      pet = 0;               // the pet whose hit put it on (its owner is the instigator); 0: the instigator's own.  It ends with the pet
};

class Book {
public:
    static constexpr size_t kMaxInstances = 256;

    struct Changes {
        std::vector<int64_t>  removed;
        std::vector<Instance> added;
        std::vector<Instance> changed;
        bool empty() const { return removed.empty() && added.empty() && changed.empty(); }
    };

    struct Due {
        enum class Kind : uint8_t { None, End, Tick };
        Kind    kind = Kind::None;
        int64_t key = 0;
        double  at = 0;
    };

    explicit Book(int64_t first_key = 1) : next_key_(first_key) {}

    const std::vector<Instance>& instances() const { return items_; }
    bool   empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }

    bool apply(const Applied& a, uint64_t instigator, double t, Changes& out, Instance* created = nullptr, uint64_t via_pet = 0) {
        if (!a.def || a.stacks < 1) return false;
        const content::EffectRow& row = a.def->row;
        const bool never_refresh = row.stack_duration_refresh.find("NeverRefresh") != std::string::npos;
        const bool in_place = row.stack_type.find("StackInPlace") != std::string::npos;
        const bool separate = row.stack_type.find("StackSeperate") != std::string::npos;
        const bool override_duration = row.stack_type.find("OverrideDuration") != std::string::npos;
        const Source src{instigator, via_pet};
        Instance* existing = nullptr;
        int same = 0;
        for (Instance& e : items_) {
            if (e.def != a.def) continue;
            ++same;
            if (!existing && (!row.compare_source || e.instigator == instigator)) existing = &e;
        }
        const auto fresh = [&](int stacks) {
            if (!add_fresh_instance(a, stacks, src, t, out)) return false;
            if (created) *created = items_.back();
            return true;
        };
        if (!existing) {
            // compare_source effects already present from other instigators are bounded by global_stack_limit (0: no limit)
            if (row.compare_source && same > 0 && row.global_stack_limit > 0 && same >= row.global_stack_limit) return false;
            return fresh(in_place ? std::min(a.stacks, std::max(1, a.in_place_limit)) : 1);
        }
        if (in_place) {
            existing->stacks = std::min(std::max(1, a.in_place_limit), existing->stacks + a.stacks);
            refresh_instance(*existing, a, src, t);
            note_changed(*existing, out);
        } else if (separate) {
            if (same < std::max(1, row.global_stack_limit)) return fresh(1);
        } else if (override_duration) {
            refresh_instance(*existing, a, src, t);
            note_changed(*existing, out);
        } else {   // OverrideAlways: one instance, replaced (a record that never refreshes keeps what it has)
            if (never_refresh) return false;
            const int64_t old_key = existing->key;
            remove_key(old_key, out);
            return fresh(1);
        }
        return false;
    }

    Due next_due(double now) const {
        Due best;
        for (const Instance& e : items_) {
            if (e.end <= now && (best.kind == Due::Kind::None || e.end < best.at || (e.end == best.at && best.kind == Due::Kind::Tick))) best = {Due::Kind::End, e.key, e.end};
            if (e.next_tick <= now && (best.kind == Due::Kind::None || e.next_tick < best.at)) best = {Due::Kind::Tick, e.key, e.next_tick};
        }
        return best;
    }

    bool end_of(int64_t key, Instance& gone, Changes& out) {
        for (const Instance& e : items_) {
            if (e.key != key) continue;
            gone = e;
            remove_key(key, out);
            return true;
        }
        return false;
    }

    bool ticked(int64_t key, double at, Instance& copy) {
        for (Instance& e : items_) {
            if (e.key != key) continue;
            const double period = e.def->row.tick_timer;
            e.next_tick = period > 0 ? at + period : kForever;
            copy = e;
            return true;
        }
        return false;
    }

    bool remove(int64_t key, Changes& out) { return remove_key(key, out); }

    size_t remove_from(uint64_t instigator, Changes& out) {
        size_t n = 0;
        for (size_t i = 0; i < items_.size();) {
            if (items_[i].instigator == instigator) {
                out.removed.push_back(items_[i].key);
                items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
                ++n;
            } else {
                ++i;
            }
        }
        return n;
    }

    size_t remove_from_pet(uint64_t pet, Changes& out) {
        size_t n = 0;
        if (pet == 0) return 0;
        for (size_t i = 0; i < items_.size();) {
            if (items_[i].pet == pet) {
                remove_key(items_[i].key, out);
                ++n;
            } else {
                ++i;
            }
        }
        return n;
    }

    void clear(Changes& out) {
        for (const Instance& e : items_) out.removed.push_back(e.key);
        items_.clear();
    }

    double remaining(int64_t effect_guid, double now) const {
        double best = 0;
        for (const Instance& e : items_) {
            if (e.def->row.guid != effect_guid) continue;
            best = std::max(best, std::isinf(e.end) ? 1.0e9 : std::max(0.0, e.end - now));
        }
        return best;
    }

    int stacks_of(int64_t effect_guid) const {
        int n = 0;
        for (const Instance& e : items_) {
            if (e.def->row.guid == effect_guid) n += e.stacks;
        }
        return n;
    }

    std::vector<stats::ActiveEffect> engine_effects() const {
        std::vector<stats::ActiveEffect> out;
        out.reserve(items_.size());
        for (const Instance& e : items_) {
            stats::ActiveEffect a;
            a.effect = {stats::kRefTypeEffect, e.def->row.guid};
            a.stacks = e.stacks;
            a.tags = e.def->row.tags;
            a.tags.insert(a.tags.end(), e.def->row.effect_tags.begin(), e.def->row.effect_tags.end());
            a.mods = e.mods;
            out.push_back(std::move(a));
        }
        return out;
    }

    struct Crowd {
        int64_t flags = 0;
        double  snare = 0;
        bool    override_move = false;
        int     anim = content::kCcAnimNone;
        int64_t anim_record = 0;
        double  anim_start = 0;
        double  anim_seconds = 0;
        bool operator==(const Crowd&) const = default;
    };
    Crowd cc() const {
        Crowd out;
        const Instance* shown = nullptr;
        for (const Instance& e : items_) {
            if (!e.def || !e.def->cc) continue;
            const content::CrowdControlRow& r = *e.def->cc;
            out.flags |= r.applied_status;
            if (r.applied_status & content::kCcSnare) out.snare = std::max(out.snare, r.snare_strength);
            out.override_move = out.override_move || r.override_client_move;
            if (animation_of(r) == content::kCcAnimNone) continue;
            if (!shown || r.priority > shown->def->cc->priority ||
                (r.priority == shown->def->cc->priority && (e.end > shown->end || (e.end == shown->end && e.start > shown->start)))) {
                shown = &e;
            }
        }
        if (shown) {
            out.anim = animation_of(*shown->def->cc);
            out.anim_record = shown->def->cc->guid;
            out.anim_start = shown->start;
            out.anim_seconds = std::isinf(shown->end) ? static_cast<double>(std::numeric_limits<float>::max()) : static_cast<double>(static_cast<float>(shown->end - shown->start));
        }
        return out;
    }
    static int animation_of(const content::CrowdControlRow& r) {
        if (r.animation_request.find("Stunned") != std::string::npos) return content::kCcAnimStunned;
        if (r.animation_request.find("Tripped") != std::string::npos) return content::kCcAnimTripped;
        return content::kCcAnimNone;
    }

private:
    struct Source {
        uint64_t instigator = 0;
        uint64_t via_pet = 0;
    };

    static void refresh_instance(Instance& e, const Applied& a, Source src, double t) {
        const content::EffectRow& row = a.def->row;
        e.mods = a.mods;
        e.instigator = src.instigator;   // compare_source effects only reach an instance of this same instigator
        if (src.via_pet == 0 || e.pet != 0) e.pet = src.via_pet;
        if (row.stack_duration_refresh.find("NeverRefresh") != std::string::npos) return;
        const bool extend = row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
        if (extend && a.extension >= 0 && !std::isinf(e.end) && !std::isinf(a.duration)) {
            double end = e.end + a.extension;
            if (a.extended_max > 0) end = std::min(end, t + a.extended_max);
            e.start = t;
            e.end = std::max(end, e.end);
            return;
        }
        e.start = t;
        e.end = std::isinf(a.duration) ? kForever : t + a.duration;
    }

    bool add_fresh_instance(const Applied& a, int stacks, Source src, double t, Changes& out) {
        if (items_.size() >= kMaxInstances) return false;
        const content::EffectRow& row = a.def->row;
        Instance e;
        e.key = next_key_++;
        e.def = a.def;
        e.stacks = stacks;
        e.start = t;
        e.end = std::isinf(a.duration) ? kForever : t + a.duration;
        e.mods = a.mods;
        e.next_tick = row.tick_timer > 0 ? (row.ignore_initial_tick ? t + row.tick_timer : t) : kForever;
        e.instigator = src.instigator;
        e.pet = src.via_pet;
        items_.push_back(e);
        out.added.push_back(e);
        return true;
    }

    bool remove_key(int64_t key, Changes& out) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].key != key) continue;
            // an instance this same change added never reached the replicated array: it is dropped from the change, not removed
            bool unsent = false;
            for (auto it = out.added.begin(); it != out.added.end(); ++it) {
                if (it->key == key) {
                    out.added.erase(it);
                    unsent = true;
                    break;
                }
            }
            for (auto it = out.changed.begin(); it != out.changed.end();) it = it->key == key ? out.changed.erase(it) : it + 1;
            if (!unsent) out.removed.push_back(key);
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    }

    // an instance changed in place; one that is new in this same change stays an addition with its latest values
    static void note_changed(const Instance& e, Changes& out) {
        for (Instance& a : out.added) {
            if (a.key == e.key) {
                a = e;
                return;
            }
        }
        for (Instance& c : out.changed) {
            if (c.key == e.key) {
                c = e;
                return;
            }
        }
        out.changed.push_back(e);
    }

    std::vector<Instance> items_;
    int64_t               next_key_ = 1;
};

}  // namespace aoc::worldd::effects
