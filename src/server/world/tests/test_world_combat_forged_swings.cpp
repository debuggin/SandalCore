// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat_test_fixture.h"

namespace sandal::worldd::tests {
namespace {

struct Swings {
    Forge                        g;
    const abilities::AbilityDef* def = nullptr;
    uint64_t                     wolf = 0;
    double                       t = 1.0;
    int32_t                      id = 100;
    std::set<int64_t>            arrow_hits;

    Swings(Fixture& f, const char* combo, double x = 800.0) : g(f, 1, {}), def(f.catalog->ability_named(combo)) {
        if (!g.ok() || !def) return;
        wolf = g.w.spawn(g.wolf_path, x, 0, 60, 100);
        g.rig.know({combo});
        g.rig.asks = false;
        for (const content::AbilityHitRow& h : def->hits) {
            if (h.source == "projectile") arrow_hits.insert(h.hit_guid);
        }
    }
    bool ok() const { return g.ok() && def && wolf != 0; }
    Refusal swing(int anim, double gap) {
        t += gap;
        g.rig.run_to(t - 0.001);
        TriggerInfo info;
        info.animation_index = anim;
        info.target_count = 1;
        info.target_actors = {wolf};
        const Refusal r = g.rig.pipe->trigger(def->row.guid, id++, t, info);
        g.rig.take();
        return r;
    }
    void play(double after) { g.rig.run_to(t + after); }
    bool matches_landing(const bootstrap::HitConfirm& c, int64_t source, const char* hit) const {
        if (c.targets.empty() || c.source_type != source) return false;
        if (!hit) return arrow_hits.count(c.record_guid) != 0;
        const abilities::HitDef* h = g.rig.catalog->hit(c.record_guid);
        return h && h->row.name == hit;
    }
    size_t landed(int64_t source, const char* hit = nullptr) const {
        size_t n = 0;
        for (const Seen& s : g.rig.log) {
            for (const bootstrap::HitConfirm& c : s.confirmations) {
                n += matches_landing(c, source, hit) ? 1 : 0;
            }
        }
        return n;
    }
    int64_t targeting_of(const abilities::HitDef& hit) const {
        int64_t targeting = content::kTargetingDefaultSelf;
        for (const content::AbilityHitRow& h : def->hits) {
            if (h.hit_guid != hit.row.guid || h.source != "hit") continue;
            for (const content::TargetingRow& tg : def->targetings) {
                if (tg.key == h.key) targeting = tg.guid;
            }
        }
        return targeting;
    }
    AbilityPipeline::HitVerdict ask(const abilities::HitDef& hit, double after, size_t own = 0) {
        g.rig.run_to(t + after);
        const auto v = g.rig.pipe->hit_request(def->row.guid, targeting_of(hit), hit.row.guid, 1, own, t + after, {wolf});
        g.rig.take();
        return v;
    }
    void put_wolf(double x) {
        const world::WorldActor* a = g.w.registry.find(wolf);
        if (a) g.w.registry.move(wolf, x, 0, a->z, a->yaw_degrees);
    }
};

std::vector<size_t> shortbow_arrows(TestHarness& h, const std::vector<int>& anims, bool* accepted) {
    std::vector<size_t> out;
    Swings s(h.f, "WeaponCombo_Ranged_Shortbow");
    CHECKF(s.ok(), "the world for the Shortbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return out;
    size_t before = 0;
    for (const int anim : anims) {
        if (s.swing(anim, 3.0) != Refusal::None) *accepted = false;
        s.play(2.5);
        const size_t now = s.landed(content::kSourceTypeProjectile);
        out.push_back(now - before);
        before = now;
    }
    return out;
}

void test_forged_swing_step(TestHarness& h) {
    std::printf("a ranged weapon swing plays the next step of its combo or starts over, whatever montage the client names\n");
    bool accepted = true;
    const std::vector<size_t> honest = shortbow_arrows(h, {0, 1, 2, 3, 5, 7}, &accepted);
    CHECK(honest == std::vector<size_t>({1, 2, 1, 3, 3, 5}), "the Shortbow's combo in order sends 1, 2, 1, 3, 3 and 5 arrows");
    const std::vector<size_t> restarted = shortbow_arrows(h, {0, 1, 2, 0, 1}, &accepted);
    CHECK(restarted == std::vector<size_t>({1, 2, 1, 1, 2}), "...and a swing that starts the combo over (montage 0) is taken wherever the combo was: 1, 2, 1, 1 and 2 arrows");
    const std::vector<size_t> forged = shortbow_arrows(h, {0, 7, 7, 7, 7, 7}, &accepted);
    CHECK(accepted, "(every swing is accepted)");
    size_t total = 0, most_early = 0;
    for (size_t i = 0; i < forged.size(); ++i) {
        total += forged[i];
        if (i + 1 < forged.size()) most_early = std::max(most_early, forged[i]);
    }
    std::string arrows;
    for (const size_t n : forged) arrows += (arrows.empty() ? "" : ", ") + std::to_string(n);
    CHECKF(total == 15 && most_early <= 3, "naming the five-arrow montage on every swing sends the arrows of the combo's own steps, 15 in six swings, none of the first five more than three (sent: %s)", arrows.c_str());
}

size_t run_shortbow_volley(TestHarness& h, bool stun) {
    size_t arrows = 0;
    Swings s(h.f, "WeaponCombo_Ranged_Shortbow");
    CHECKF(s.ok(), "the world for the Shortbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return arrows;
    for (const int anim : {0, 1, 2}) {
        s.swing(anim, 3.0);
        s.play(2.5);
    }
    const size_t before = s.landed(content::kSourceTypeProjectile);
    CHECK(s.swing(3, 3.0) == Refusal::None, "the three-arrow swing is accepted");
    s.play(0.30);
    const bootstrap::ActiveAbilityItem* item = s.g.rig.pawn.ability_item();
    CHECK(item != nullptr && item->ability_guid == s.def->row.guid, "the swing's item is the pawn's");
    if (stun && item) CHECK(s.g.rig.pipe->end_request(static_cast<int32_t>(item->server_instance_id), s.t + 0.30), "the client ends the swing's instance");
    s.play(3.0);
    arrows = s.landed(content::kSourceTypeProjectile) - before;
    CHECK(s.g.rig.pawn.broken.empty(), "(the pawn took every change)");
    return arrows;
}

void test_forged_swing_ended(TestHarness& h) {
    std::printf("a swing the client ends sends no more arrows\n");
    const size_t free_swing = run_shortbow_volley(h, false);
    CHECKF(free_swing == 3, "a swing nobody ends sends its three arrows (%zu landed)", free_swing);
    const size_t stunned = run_shortbow_volley(h, true);
    CHECKF(stunned == 1, "a swing the client ends 0.30 s in sends only the arrow that had left (%zu landed)", stunned);
}

void test_swing_replaced_by_next(TestHarness& h) {
    std::printf("a swing's single arrow does not leave once the next swing's item has taken its place\n");
    Swings s(h.f, "WeaponCombo_Ranged_Longbow");
    CHECKF(s.ok(), "the world for the Longbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return;
    CHECK(s.swing(0, 3.0) == Refusal::None && s.swing(1, 0.45) == Refusal::None, "two swings 0.45 s apart are accepted");
    s.play(3.0);
    const size_t first = s.landed(content::kSourceTypeProjectile, "WeaponCombo_Longbow_01");
    const size_t second = s.landed(content::kSourceTypeProjectile, "WeaponCombo_Longbow_02");
    CHECKF(first == 0, "the first swing's arrow never leaves: the second swing's item came before its montage sent it (%zu landed)", first);
    CHECKF(second == 1, "...and the second swing's arrow lands (%zu landed)", second);
}

void test_swing_far_arrow_flight(TestHarness& h) {
    std::printf("an arrow aimed at a target that ran far off is in the air for no more than the longest flight\n");
    Swings s(h.f, "WeaponCombo_Ranged_Shortbow");
    CHECKF(s.ok(), "the world for the Shortbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return;
    for (const int anim : {0, 1, 2}) {
        s.swing(anim, 3.0);
        s.play(2.5);
    }
    const size_t before = s.landed(content::kSourceTypeProjectile);
    CHECK(s.swing(3, 3.0) == Refusal::None, "the three-arrow swing is accepted with the Wolf 8 m ahead");
    s.g.rig.run_to(s.t + 0.05);
    s.put_wolf(90000.0);
    s.g.rig.run_to(s.t + 2.0);
    s.put_wolf(800.0);
    s.play(4.5);
    const size_t soon = s.landed(content::kSourceTypeProjectile) - before;
    CHECKF(soon == 3, "all three arrows have landed 4.5 s after the trigger: the longest flight is %.0f s (%zu landed)", AbilityPipeline::kMaxShotFlight, soon);
    s.play(25.0);
    const size_t late = s.landed(content::kSourceTypeProjectile) - before;
    CHECKF(late == 3, "(nothing lands twice, and nothing is lost: %zu landed at 25 s)", late);
}

struct BeamAsk {
    const char* hit;
    double      after;
};

Refusal wand_beam_swing(Swings& s, int beam) {
    for (const int anim : {0, 1, 2, 3, 4}) {
        if (anim == beam) break;
        s.swing(anim, 3.0);
        s.play(2.5);
    }
    return s.swing(beam, 3.0);
}

std::map<std::string, size_t> beam_taken(TestHarness& h, Swings& s, const std::vector<BeamAsk>& asks) {
    std::map<std::string, size_t> taken;
    for (const BeamAsk& a : asks) {
        const abilities::HitDef* hd = h.f.catalog->hit_named(a.hit);
        CHECKF(hd != nullptr, "%s is in the data", a.hit);
        if (!hd) continue;
        const auto v = s.ask(*hd, a.after, 1);
        if (v == AbilityPipeline::HitVerdict::Scheduled || v == AbilityPipeline::HitVerdict::Empty) ++taken[a.hit];
    }
    return taken;
}

size_t taken_count(const std::map<std::string, size_t>& m, const char* hit) {
    const auto it = m.find(hit);
    return it == m.end() ? size_t{0} : it->second;
}

void test_forged_swing_shortbow_carried_hit(TestHarness& h) {
    Swings s(h.f, "WeaponCombo_Ranged_Shortbow");
    CHECKF(s.ok(), "the world for the Shortbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return;
    const abilities::HitDef* carried = h.f.catalog->hit_named("WeaponCombo_Shortbow_Hit1");
    CHECK(carried != nullptr, "the hit the first arrow carries is in the data");
    if (!carried) return;
    CHECK(s.swing(0, 1.0) == Refusal::None, "the first swing is accepted");
    const auto verdict = s.ask(*carried, 0.5);
    s.play(3.0);
    CHECKF(verdict != AbilityPipeline::HitVerdict::Scheduled, "a request for the hit its arrow carries is not served (%s)", AbilityPipeline::verdict_name(verdict));
    CHECKF(s.landed(content::kSourceTypeAbility, "WeaponCombo_Shortbow_Hit1") == 0, "...and lands nothing on the Wolf (%zu landed)", s.landed(content::kSourceTypeAbility, "WeaponCombo_Shortbow_Hit1"));
    CHECKF(s.landed(content::kSourceTypeProjectile, "WeaponCombo_Shortbow_Hit1") == 1, "the arrow itself landed once (%zu)", s.landed(content::kSourceTypeProjectile, "WeaponCombo_Shortbow_Hit1"));
}

void test_forged_swing_wand_beam_vs_step_one(TestHarness& h) {
    Swings s(h.f, "WeaponCombo_Ranged_Wand");
    CHECKF(s.ok(), "the world for the Wand swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return;
    const abilities::HitDef* beam_end = h.f.catalog->hit_named("Weapon_Wand_Beam_ComboEnd_Hit");
    const abilities::HitDef* step_one = h.f.catalog->hit_named("Weapon_Wand_Hit_1");
    const abilities::HitDef* step_six = h.f.catalog->hit_named("Weapon_Wand_Hit_6+");
    CHECK(beam_end && step_one && step_six, "the beam's end, the first step's hit and the extended finisher's hit are in the data");
    if (!beam_end || !step_one || !step_six) return;
    for (const int anim : {0, 1, 2, 3}) {
        s.swing(anim, 3.0);
        s.play(2.5);
    }
    CHECK(s.swing(4, 3.0) == Refusal::None, "the beam step is accepted");
    const auto other = s.ask(*step_one, 0.4);
    const auto end = s.ask(*beam_end, 0.8);
    s.play(1.6);
    CHECKF(end == AbilityPipeline::HitVerdict::Scheduled, "the request for the beam's end is served (%s)", AbilityPipeline::verdict_name(end));
    CHECKF(s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit") == 1, "...and the beam's end lands on the Wolf once (%zu)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit"));
    CHECKF(other != AbilityPipeline::HitVerdict::Scheduled && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_1") == 0,
           "a request for the first step's hit during the beam is not served and lands nothing (%s, %zu landed)", AbilityPipeline::verdict_name(other), s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_1"));
    CHECK(s.swing(6, 3.0) == Refusal::None, "the extended finisher's beam step is accepted");
    const auto six = s.ask(*step_six, 0.8);
    s.play(1.6);
    CHECKF(six == AbilityPipeline::HitVerdict::Scheduled, "the request for the extended finisher's own hit is served (%s)", AbilityPipeline::verdict_name(six));
    CHECKF(s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+") == 1, "...and lands on the Wolf once (%zu)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+"));
    CHECK(s.g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

void test_forged_swing_wand_first_beam_caps(TestHarness& h) {
    Swings s(h.f, "WeaponCombo_Ranged_Wand");
    CHECKF(s.ok(), "the world for the Wand swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return;
    CHECK(wand_beam_swing(s, 4) == Refusal::None, "the first beam step is accepted");
    const std::map<std::string, size_t> honest = beam_taken(h, s, {{"Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY", 0.486}, {"Weapon_Wand_Hit_5+", 0.486}, {"Weapon_Wand_BeamFX_Hit_Lightning_AUDIOONLY", 0.499},
                                                                   {"Weapon_Wand_BeamFX_Hit_Fire_AUDIOONLY", 0.499}, {"Weapon_Wand_BeamFX_Hit_Ice_AUDIOONLY", 0.499}, {"Weapon_Wand_Hit_5+", 0.621},
                                                                   {"Weapon_Wand_Hit_5+", 0.728}, {"Weapon_Wand_Beam_ComboEnd_Hit", 0.808}});
    CHECKF(taken_count(honest, "Weapon_Wand_Hit_5+") == 3 && taken_count(honest, "Weapon_Wand_Beam_ComboEnd_Hit") == 1 && taken_count(honest, "Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY") == 1 &&
               taken_count(honest, "Weapon_Wand_BeamFX_Hit_Lightning_AUDIOONLY") == 1 && taken_count(honest, "Weapon_Wand_BeamFX_Hit_Fire_AUDIOONLY") == 1 && taken_count(honest, "Weapon_Wand_BeamFX_Hit_Ice_AUDIOONLY") == 1,
           "an honest first beam step's requests are all taken: the step's hit 3 of 3, the end 1 of 1, each beam effect's audio hit 1 of 1 (%zu, %zu, %zu, %zu, %zu, %zu)", taken_count(honest, "Weapon_Wand_Hit_5+"),
           taken_count(honest, "Weapon_Wand_Beam_ComboEnd_Hit"), taken_count(honest, "Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY"), taken_count(honest, "Weapon_Wand_BeamFX_Hit_Lightning_AUDIOONLY"),
           taken_count(honest, "Weapon_Wand_BeamFX_Hit_Fire_AUDIOONLY"), taken_count(honest, "Weapon_Wand_BeamFX_Hit_Ice_AUDIOONLY"));
    s.play(1.0);
    CHECKF(s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_5+") == 3 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit") == 1,
           "...and they land on the Wolf: the step's hit 3 times, the end once (%zu, %zu)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_5+"), s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit"));
    const std::map<std::string, size_t> forged = beam_taken(h, s, {{"Weapon_Wand_ProcHit", 1.05}, {"Weapon_Wand_ProcHit_2", 1.10}, {"Weapon_Wand_Hit_5+", 1.15}, {"Weapon_Wand_Hit_5+", 1.20},
                                                                   {"Weapon_Wand_Hit_5+", 1.25}, {"Weapon_Wand_Beam_ComboEnd_Hit", 1.30}, {"Weapon_Wand_Beam_ComboEnd_Hit", 1.35},
                                                                   {"Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY", 1.40}, {"Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY", 1.45}});
    s.play(3.0);
    CHECKF(taken_count(forged, "Weapon_Wand_ProcHit") == 0 && taken_count(forged, "Weapon_Wand_ProcHit_2") == 0 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit") == 0 &&
               s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit_2") == 0,
           "a request for a proc's hit is not taken and lands nothing (%zu and %zu taken, %zu and %zu landed)", taken_count(forged, "Weapon_Wand_ProcHit"), taken_count(forged, "Weapon_Wand_ProcHit_2"),
           s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit"), s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit_2"));
    CHECKF(taken_count(forged, "Weapon_Wand_Hit_5+") == 1 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_5+") == 4,
           "the step's hit is taken 4 times in all, one more than an honest swing asks for: the 5th and 6th request land nothing (%zu more taken, %zu landed)", taken_count(forged, "Weapon_Wand_Hit_5+"),
           s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_5+"));
    CHECKF(taken_count(forged, "Weapon_Wand_Beam_ComboEnd_Hit") == 1 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit") == 2,
           "the beam's end is taken twice in all, and a third request lands nothing (%zu more taken, %zu landed)", taken_count(forged, "Weapon_Wand_Beam_ComboEnd_Hit"),
           s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit"));
    CHECKF(taken_count(forged, "Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY") == 1, "a beam effect's audio hit is taken twice in all (%zu more taken)", taken_count(forged, "Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY"));
    CHECK(s.g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

void test_forged_swing_wand_second_beam_caps(TestHarness& h) {
    Swings s(h.f, "WeaponCombo_Ranged_Wand");
    CHECKF(s.ok(), "the world for the Wand swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return;
    CHECK(wand_beam_swing(s, 6) == Refusal::None, "the second beam step is accepted");
    std::vector<BeamAsk> asks = {{"Weapon_Wand_BeamFX_Hit_BeamAmp_AUDIOONLY", 0.122}};
    for (const double at : {0.122, 0.235, 0.330, 0.435, 0.527, 0.633, 0.726, 0.830, 0.917, 1.031, 1.115, 1.210}) asks.push_back({"Weapon_Wand_Hit_6+", at});
    asks.push_back({"Weapon_Wand_Beam_ComboEnd2_Hit", 1.210});
    const std::map<std::string, size_t> honest = beam_taken(h, s, asks);
    CHECKF(taken_count(honest, "Weapon_Wand_Hit_6+") == 12 && taken_count(honest, "Weapon_Wand_Beam_ComboEnd2_Hit") == 1 && taken_count(honest, "Weapon_Wand_BeamFX_Hit_BeamAmp_AUDIOONLY") == 1,
           "an honest second beam step's requests are all taken: the step's hit 12 of 12, the end and the audio hit 1 of 1 (%zu, %zu, %zu)", taken_count(honest, "Weapon_Wand_Hit_6+"),
           taken_count(honest, "Weapon_Wand_Beam_ComboEnd2_Hit"), taken_count(honest, "Weapon_Wand_BeamFX_Hit_BeamAmp_AUDIOONLY"));
    s.play(1.4);
    CHECKF(s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+") == 12, "...and the step's hit lands on the Wolf 12 times (%zu)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+"));
    const std::map<std::string, size_t> forged = beam_taken(h, s, {{"Weapon_Wand_Hit_6+", 1.45}, {"Weapon_Wand_Hit_6+", 1.55}, {"Weapon_Wand_Hit_6+", 1.65}, {"Weapon_Wand_Hit_6+", 1.75},
                                                                   {"Weapon_Wand_ProcHit", 1.85}, {"Weapon_Wand_ProcHit_2", 1.95}, {"Weapon_Wand_Beam_ComboEnd2_Hit", 2.05}, {"Weapon_Wand_Beam_ComboEnd2_Hit", 2.15}});
    s.play(4.0);
    CHECKF(taken_count(forged, "Weapon_Wand_Hit_6+") == 2 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+") == 14,
           "the step's hit is taken 14 times in all, two more than an honest swing asks for: the 15th and 16th request land nothing (%zu more taken, %zu landed)", taken_count(forged, "Weapon_Wand_Hit_6+"),
           s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+"));
    CHECKF(taken_count(forged, "Weapon_Wand_ProcHit") == 0 && taken_count(forged, "Weapon_Wand_ProcHit_2") == 0 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit") == 0 &&
               s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit_2") == 0,
           "...and a request for a proc's hit lands nothing (%zu and %zu landed)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit"), s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit_2"));
    CHECKF(taken_count(forged, "Weapon_Wand_Beam_ComboEnd2_Hit") == 1 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd2_Hit") == 2,
           "the second beam's end is taken twice in all, and a third request lands nothing (%zu more taken, %zu landed)", taken_count(forged, "Weapon_Wand_Beam_ComboEnd2_Hit"),
           s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd2_Hit"));
    CHECK(s.g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

void test_forged_swing_requests(TestHarness& h) {
    std::printf("the hits a ranged weapon swing's arrows carry take no request from the client\n");
    test_forged_swing_shortbow_carried_hit(h);
    test_forged_swing_wand_beam_vs_step_one(h);
    test_forged_swing_wand_first_beam_caps(h);
    test_forged_swing_wand_second_beam_caps(h);
}

}  // namespace

void run_combat_forged_swing_tests(TestHarness& h) {
    test_forged_swing_step(h);
    test_forged_swing_ended(h);
    test_swing_replaced_by_next(h);
    test_swing_far_arrow_flight(h);
    test_forged_swing_requests(h);
}

}  // namespace sandal::worldd::tests
