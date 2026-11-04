#include "test_worldd_ability_session_common.h"

#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "session/player_session.h"

using namespace aoc::worldd;

static BunchWriter hit_request_bunch(const bs::PlayerSetup &setup, int64_t ability, int64_t targeting, int64_t hit, bool with_component = true) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("RequestHitVerification_Server");
    repl::HitRequest req;
    if (with_component) {
        repl::HitComponentRef c;
        c.object = setup.guids.collision_cylinder;
        c.server_timestamp = 1.0f;
        req.components.push_back(c);
    }
    req.ability_guid = ability;
    req.targeting_guid = targeting;
    req.hit_guid = hit;
    const auto params = rpc ? repl::hit_request_params(*rpc, req) : std::nullopt;
    if (!params) {
        std::printf("  [FAIL] the hit request's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "RequestHitVerification_Server", *params);
}

static BunchWriter trigger_aimed(const bs::PlayerSetup &setup, int64_t guid, int32_t id, bool target) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) {
        ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok && target) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = repl::Value(setup.guids.collision_cylinder);
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the aimed trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}

static int32_t trigger_aimed_now(Run &r, int64_t guid, bool target) {
    const int32_t id = r.next_id++;
    r.send_reliable(trigger_aimed(r.h.s->bootstrap().setup(), guid, id, target));
    return id;
}

static size_t cooldown_items(const Run &r) {
    std::set<int64_t> abilities;
    for (const auto &[id, guid] : r.m.cooldowns) {
        abilities.insert(guid);
    }
    return abilities.size();
}

static void test_d2_glee_heals() {
    std::printf("D2: Quick Wit (Glee) heals the caster\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    CHECK(r.h.s->pipeline() != nullptr && r.h.s->pipeline()->handles(kQuickWit) && r.h.s->pipeline()->known(kQuickWit),
          "the generic runtime runs Quick Wit, which the level-25 Bard knows");
    const float max_health = r.m.stats[kHealthStat];
    CHECK(r.h.s->set_current_value(kHealthStat, 300.0f), "(a hit took the Bard down to 300 Health)");
    r.tick(false);
    CHECK(r.m.stats[kHealthStat] >= 300.0f && r.m.stats[kHealthStat] < 320.0f, "(the client has it, with at most a regeneration tick on top)");
    const float mana_before = r.m.stats[kManaStat];
    const float health_before = r.m.stats[kHealthStat];
    const size_t ends_before = r.m.ends.size();
    trigger_aimed_now(r, kQuickWit, true);
    CHECK(r.m.ends.size() == ends_before && !r.m.has_ability(kQuickWit),
          "the trigger is accepted: no ClientEndAbility, and nothing at the trigger itself (the montage's first notify is ~0.3 s on)");
    r.play(0.5, false);
    CHECK(r.m.has_ability(kQuickWit), "0.5 s later the ability item is there");
    bool cooldown = false;
    for (const auto &[id, guid] : r.m.cooldowns) {
        cooldown = cooldown || guid == kQuickWit;
    }
    CHECK(cooldown && r.m.has_effect(kResonanceEffect), "...with its Cooldowns item and the Bard's Resonance");
    CHECK(near(mana_before - r.m.stats[kManaStat], 56.875, 8.0), "Mana fell by 10 x ManaCostMult(25) = 56.875 (a regeneration tick may have given some back)");
    r.play(0.5, false);
    CHECK(r.m.has_effect(kGleeEffect), "Glee is on the caster");
    CHECK(r.m.stats[kHealthStat] > health_before + 20.0f, "and the Health has risen: the projectile's heal landed (the retail Glee heals ~0.34 s after the cost)");
    CHECK(r.m.stats[kMitigationStat] < 1.0f && r.m.stats[kMitigationStat] > 0.9f, "Mitigation is x (1 - Glee's value): a little under 1");
    const float after_cast = r.m.stats[kHealthStat];
    r.play(4.0, false);
    CHECK(r.m.stats[kHealthStat] > after_cast + 20.0f, "every 2 s Glee's tick heals again, over and above the regeneration");
    r.play(14.0, false);
    CHECK(!r.m.has_effect(kGleeEffect) && r.m.stats[kMitigationStat] == 1.0f, "after its 15 s Glee ends and Mitigation is back");
    CHECK(r.m.stats[kHealthStat] <= max_health && !r.h.s->abilities_failed(), "(never above the maximum; the session is fine)");
    r.play(100.0, false);
    CHECK(r.m.stats[kHealthStat] == max_health, "Health ends at the maximum");
}

static void test_d2_flourish_dash() {
    std::printf("D2: Flourish's slide against the move checker\n");
    const svc::Character rec = bard25();
    Run control(0, true, &rec);
    control.play(0.5, true);
    for (int i = 0; i < 6; ++i) {
        control.tick_at(3600);
    }
    CHECK(control.h.s->moves_refused() > 0, "(without the cast, 3,600 cm/s is over the limit of 2,135: refused)");
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.trigger(kFlourish);
    for (int i = 0; i < 5; ++i) {
        r.tick_at(3600);
    }
    CHECK(r.h.s->moves_refused() == 0 && r.h.s->corrections_sent() == 0, "after Flourish the slide is accepted: no move refused, no correction");
    CHECK(r.m.has_ability(kFlourish), "(the cast went through: its item is on the client)");
    r.play(1.5, true);
    for (int i = 0; i < 6; ++i) {
        r.tick_at(3600);
    }
    CHECK(r.h.s->moves_refused() > 0, "once the window is over the speed is refused again");
    Run too_fast(0, true, &rec);
    too_fast.play(0.5, true);
    too_fast.trigger(kFlourish);
    for (int i = 0; i < 6; ++i) {
        too_fast.tick_at(9000);
    }
    CHECK(too_fast.h.s->moves_refused() > 0, "and not even Flourish allows 9,000 cm/s");
}

static void test_d2_dodge_keeps_cooldowns() {
    std::printf("D2: a Dodge after other casts leaves their cooldowns\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.trigger(kFlourish);
    r.play(0.8, false);
    trigger_aimed_now(r, kQuickWit, true);
    r.play(1.2, false);
    CHECK(cooldown_items(r) == 2 && r.h.s->bootstrap().listed_cooldowns() == 2, "Flourish and Quick Wit are on cooldown: the server's array and the client's both list them");
    r.trigger(kDodge);
    r.play(0.4, true);
    CHECK(r.m.has_ability(kDodge) && cooldown_items(r) == 3 && r.h.s->bootstrap().listed_cooldowns() == 3,
          "the Dodge adds its own item: all three are listed (the client's predicted cooldowns are not dropped)");
    bool still = false;
    for (const auto &[id, guid] : r.m.cooldowns) {
        still = still || guid == kFlourish;
    }
    CHECK(still, "Flourish's cooldown is still there after the Dodge's update");
    r.play(1.0, false);
    CHECK(cooldown_items(r) == 2, "when the Dodge's cooldown ends only its item goes");
    r.play(9.0, false);
    CHECK(cooldown_items(r) == 1, "Quick Wit's goes at the end of its 8 s");
    r.play(12.0, false);
    CHECK(cooldown_items(r) == 0 && !r.h.s->abilities_failed(), "and Flourish's at the end of its 20 s");
}

static void test_d2_floating_numbers_missing_health(Run &r, int64_t heal_inst) {
    CHECK(r.h.s->set_current_value(kHealthStat, r.m.stats[kHealthStat] - 20.0f), "(Health is 20 short)");
    r.play(9.0, false);
    const size_t before = r.m.hits.size();
    trigger_aimed_now(r, kQuickWit, true);
    r.play(1.0, false);
    const repl::HitInstance *second = nullptr;
    for (size_t i = before; i < r.m.hits.size(); ++i) {
        if (r.m.hits[i].source_record_type == 979581256175327151ll && !r.m.hits[i].targets.empty() && !r.m.hits[i].targets[0].stat_mods.empty()) {
            second = &r.m.hits[i];
        }
    }
    CHECK(second && second->targets[0].stat_mods[0].effective_value <= second->targets[0].stat_mods[0].value && second->inst_guid != heal_inst,
          "the next one has its own InstGuid");
}

static void test_d2_floating_numbers() {
    std::printf("D2: a heal at full Health sends its amount in a hit confirmation\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    CHECK(r.m.hits.empty() && r.m.hit_calls == 0, "(nothing confirmed before a cast)");
    trigger_aimed_now(r, kQuickWit, true);
    r.play(1.5, false);
    CHECK(r.m.hit_calls >= 1 && !r.m.hits.empty(), "the cast's projectile hit is confirmed with a ClientHitConfirmationsReliable");
    const repl::HitInstance *heal = nullptr;
    for (const repl::HitInstance &h : r.m.hits) {
        if (h.source_record_type == 979581256175327151ll && !h.targets.empty() && !h.targets[0].stat_mods.empty()) {
            heal = &h;
        }
    }
    CHECK(heal != nullptr, "one hit is a projectile's, with a stat mod on its target");
    if (!heal) {
        return;
    }
    const bs::SessionGuids &g = r.h.s->bootstrap().setup().guids;
    CHECK(heal->targets.size() == 1 && heal->targets[0].hit_comp == g.pawn && heal->instigator == g.pawn && heal->predicted_on_autonomous && heal->predicted_on_simulated,
          "it names the Bard's pawn as the target and the instigator, as retail's (HitComp = Instigator)");
    CHECK(heal->source == emit::FIntrepidNetworkGUID{} && heal->source_record_guid != 0, "a projectile's hit has no Source actor, and the projectile record's guid");
    const auto &mod = heal->targets[0].stat_mods[0];
    CHECK(mod.value > 10.0f && mod.effective_value == 0.0f, "the stat mod has the heal's amount and an effective value of 0: the client draws the number although Health is full");
    CHECK(r.h.s->hit_confirmations_sent() == r.m.hits.size() && r.h.s->hit_confirmations_pending() == 0, "(every confirmation went out, none waits)");
    const int64_t heal_inst = heal->inst_guid;
    test_d2_floating_numbers_missing_health(r, heal_inst);
}

static void test_d2_shielding_damage_and_expiry(Run &r, float shield) {
    const float health = r.m.stats[kHealthStat];
    const double through = r.h.s->take_damage(r.h.conn, shield * 0.5, r.h.t);
    r.h.flush();
    r.absorb();
    r.tick(false);
    CHECK(near(through, 0.0, 1e-9) && r.m.shields.size() == 1 && near(r.m.shields[0].second, shield * 0.5, 0.05) && r.m.shields[0].first == shield &&
              r.m.stats[kHealthStat] >= health,
          "half the shield's amount in damage: the client's array shows the half left, Health is not touched");
    r.play(9.0, false);
    CHECK(r.m.shields.empty() && !r.m.has_effect(6064631234954461268ll), "after its 8 s the effect and the shield are gone");
    CHECK(!r.h.s->abilities_failed(), "(the session is fine)");
}

static void test_d2_shielding() {
    std::printf("D2: Shielding Dance's shield and the client's LingerTick request\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.h.s->teach_ability(kShielding);
    r.trigger(kShielding);
    r.play(0.7, false);
    CHECK(r.m.shields.size() == 1 && r.m.shields[0].first > 30.0f && r.m.shields[0].first == r.m.shields[0].second,
          "the TempHealth array holds one shield: Base = Value = its amount");
    const float shield = r.m.shields.empty() ? 0.0f : r.m.shields[0].second;
    CHECK(r.m.has_effect(6064631234954461268ll), "(its TempHP effect is on the Bard)");
    const bs::PlayerSetup &setup = r.h.s->bootstrap().setup();
    r.send_reliable(hit_request_bunch(setup, kShielding, kAoeTargeting, kLingerTick));
    r.play(0.4, false);
    const repl::HitInstance *tick = nullptr;
    for (const repl::HitInstance &h : r.m.hits) {
        if (h.record_guid == kLingerTick) {
            tick = &h;
        }
    }
    CHECK(tick && tick->source_record_type == 1841048597116038327ll && tick->source_record_guid == kShielding && tick->source == setup.guids.pawn && tick->targets.size() == 1 &&
              tick->targets[0].stat_mods.size() == 1,
          "the request is answered with a confirmation of the LingerTick hit: the ability as its source, the pawn as its Source, one stat mod");
    CHECK(tick && tick->targets[0].stat_mods[0].value > 0 && tick->predicted_on_autonomous && !tick->predicted_on_simulated,
          "(the heal's amount, predicted on the autonomous client only)");
    CHECK(r.m.shields.size() == 1 && r.m.shields[0].second == shield, "no second shield: the internal cooldown is on");
    test_d2_shielding_damage_and_expiry(r, shield);
}

static void test_d2_hit_hostile() {
    std::printf("D2: forged hit requests and cancel spam\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    const bs::PlayerSetup &setup = r.h.s->bootstrap().setup();
    r.send_reliable(hit_request_bunch(setup, kShielding, kAoeTargeting, kLingerTick));
    r.play(0.5, false);
    CHECK(r.m.hits.empty() && r.h.s->hit_requests() == 1, "a request for an ability that was not cast is read and dropped: nothing confirmed");
    r.send_reliable(hit_request_bunch(setup, 12345, 678, 910));
    r.send_reliable(hit_request_bunch(setup, kQuickWit, kTargetingSelf, kLingerTick));
    r.play(0.3, false);
    CHECK(r.m.hits.empty(), "a request for an unknown ability, and for a hit that is none of the ability's, are dropped");
    r.h.s->teach_ability(kShielding);
    r.trigger(kShielding);
    r.play(0.4, false);
    for (int i = 0; i < 300; ++i) {
        r.send_reliable(hit_request_bunch(setup, kShielding, kAoeTargeting, kLingerTick, i % 2 == 0));
    }
    r.play(1.0, false);
    CHECK(r.h.s->hit_requests_dropped() > 100, "300 requests at once: the budget drops most of them");
    CHECK(r.m.hits.size() <= 12, "and a cast answers at most a handful of the ones it takes");
    CHECK(!r.h.s->abilities_failed(), "(the session is fine after the flood)");
    Run c(0, true, &rec);
    c.play(0.5, true);
    for (int i = 0; i < 200; ++i) {
        c.cancel(i + 1);
    }
    c.play(0.5, false);
    CHECK(c.h.s->held_cancels_dropped() > 100 && c.h.s->held_cancels() <= 216, "200 held-ability cancels at once: the budget drops most of them");
    CHECK(!c.h.s->abilities_failed(), "(the session is fine)");
}

static void test_d2_sprint_speed() {
    std::printf("D2: the Sprint's speed from its formula\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.trigger(kSprint);
    CHECK(r.m.has_effect(kSprintEffect) && near(r.m.stats[kMoveSpeedMult], 1.3, 1e-6), "a level-25 Bard sprints at MoveSpeedMult 1.3: 780 cm/s (retail's)");
    r.play(0.5, true);
    r.cancel(0);
    r.play(0.5, true);
    CHECK(r.m.stats[kMoveSpeedMult] == 1.0f, "and it is 1.0 again when the Sprint ends");
    const svc::Character one = cyb_record();
    Run low(0, true, &one);
    low.play(0.5, true);
    low.trigger(kSprint);
    CHECK(near(low.m.stats[kMoveSpeedMult], 1.2, 1e-6), "a level-1 Bard (no Sprint Training yet) sprints at 1.2");
}

static void test_d2_hostile_mana_and_spam(const svc::Character &rec) {
    Run poor(0, true, &rec);
    poor.play(0.5, true);
    poor.h.s->set_current_value(kManaStat, 10.0f);
    poor.tick(false);
    const int32_t broke = poor.trigger(kFlourish);
    poor.play(0.6, false);
    CHECK(!poor.m.has_ability(kFlourish) && poor.m.end_ids() == std::vector<int32_t>{broke} && poor.m.stats[kManaStat] < 30.0f,
          "an ability that costs more Mana than the character has is refused: nothing is spent");
    Run spam(0, true, &rec);
    spam.play(0.5, true);
    for (int i = 0; i < 40; ++i) {
        spam.trigger(kFlourish);
    }
    spam.play(1.0, false);
    CHECK(!spam.h.s->abilities_failed() && spam.h.s->ability_triggers_dropped() > 0 && cooldown_items(spam) == 1 && spam.h.s->pipeline()->charges_left(kFlourish) == 0,
          "40 triggers in one frame: the budget drops the surplus, the casts that wait take Flourish's two charges and the rest end at their cost step, the session is fine");
    CHECK(spam.m.unreadable == 0, "(every chain the server sent reads)");
}

static void test_d2_hostile() {
    std::printf("D2: hostile triggers\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const size_t items = r.m.abilities.size();
    const int32_t unknown = r.next_id++;
    r.send_reliable(trigger_aimed(setup, 4242424242ll, unknown, false));
    r.play(0.5, false);
    CHECK(r.m.end_ids() == std::vector<int32_t>{unknown} && r.m.abilities.size() == items && !r.h.s->abilities_failed(),
          "an unknown ability is answered with ClientEndAbility, nothing changes");
    const int32_t not_known = r.next_id++;
    r.send_reliable(trigger_aimed(setup, kMaddening, not_known, false));
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == 2 && r.m.end_ids()[1] == not_known && !r.m.has_ability(kMaddening), "a real ability the character does not know is refused the same way");
    const int32_t no_target = r.next_id++;
    r.send_reliable(trigger_aimed(setup, kQuickWit, no_target, false));
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == 3 && r.m.end_ids()[2] == no_target && !r.m.has_ability(kQuickWit), "Quick Wit with no target is refused (it needs one)");
    const int32_t first = r.trigger(kFlourish);
    r.play(0.1, false);
    const int32_t during = r.trigger(kFortissimo);
    r.play(0.6, false);
    CHECK(r.m.has_ability(kFlourish) && r.m.end_ids().size() == 4 && r.m.end_ids()[3] == during,
          "a trigger while another ability's cast has not finished is refused (the first goes on)");
    const size_t ends = r.m.end_ids().size();
    r.send_trigger(kFlourish, first);
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == ends && !r.h.s->abilities_failed(), "an id the session used before is dropped with no answer (ClientEndAbility would end the real instance)");
    r.play(1.0, false);
    r.trigger(kFlourish);
    r.play(1.0, false);
    const int32_t third = r.trigger(kFlourish);
    r.play(1.0, false);
    CHECK(r.m.end_ids().back() == third, "with both of Flourish's charges spent a third use is refused");
    test_d2_hostile_mana_and_spam(rec);
}

static constexpr int64_t kCheerful = 6064630831284420608ll, kCheerfulSelf = 6064630831382003720ll;
static constexpr int64_t kLightningStrike = 6064628894041374720ll;
static constexpr int64_t kFocus = 6064633175093543338ll;

static BunchWriter end_bunch(const emit::FIntrepidNetworkGUID &ability, int64_t server_instance) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerEndAbility");
    repl::RpcParams p;
    const bool ok = rpc && repl::set_rpc_param(*rpc, p, "ServerInstanceId", repl::Value::i64(server_instance));
    if (!ok) {
        std::printf("  [FAIL] the end's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(ability, "ServerEndAbility", p);
}

static void test_d2_song() {
    std::printf("D2: a song plays, ticks and ends with a second press\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.h.s->teach_ability(kCheerful);
    const int32_t first = trigger_aimed_now(r, kCheerful, false);
    r.play(1.0, false);
    CHECK(r.m.has_ability(kCheerful) && !r.m.has_effect(kCheerfulSelf), "the press plays the song's montage; its effects are not on yet");
    r.play(2.0, false);
    CHECK(r.m.has_effect(kCheerfulSelf), "2.3 s after the press the song's own effect is on (it has no end)");
    const size_t at_3 = r.m.hits.size();
    r.play(6.0, false);
    CHECK(r.m.has_effect(kCheerfulSelf) && r.m.hits.size() > at_3, "the song goes on ticking: its hits are confirmed as it plays");
    CHECK(r.h.s->hit_confirmations_pending() == 0 && !r.h.s->abilities_failed(), "(nothing waits, nothing failed)");
    const size_t ends = r.m.end_ids().size();
    trigger_aimed_now(r, kCheerful, false);
    r.play(1.0, false);
    CHECK(!r.m.has_effect(kCheerfulSelf) && !r.h.s->abilities_failed() && r.m.end_ids().size() >= ends, "a second press ends the song: its effect is gone");
    const size_t after_end = r.m.hits.size();
    r.play(5.0, false);
    CHECK(r.m.hits.size() == after_end && r.m.unreadable == 0, "...and nothing ticks any more (every chain the server sent reads)");
    (void)first;
}

static void test_d2_held_cancel() {
    std::printf("D2: a held ability is ended by the client's cancel, quietly\n");
    const svc::Character rec = class25(8, "Mage", 17750);
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.h.s->teach_ability(kLightningStrike);
    const int32_t id = trigger_aimed_now(r, kLightningStrike, false);
    r.play(0.6, false);
    const bool held = r.m.has_ability(kLightningStrike);
    CHECK(held, "the Mage's held ability is cast: its item is on the pawn");
    const size_t ends = r.m.end_ids().size();
    r.cancel(id);
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == ends && !r.h.s->abilities_failed(), "the cancel is quiet: the server sends no ClientEndAbility for it");
    r.cancel(id);
    r.cancel(id);
    r.cancel(12345);
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == ends && !r.h.s->abilities_failed() && r.m.unreadable == 0, "a cancel again and a cancel of an unknown id change nothing");
}

static void test_d2_end_ability() {
    std::printf("D2: ServerEndAbility is answered with ClientEndAbility\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    const int32_t id = trigger_aimed_now(r, kQuickWit, true);
    r.play(0.4, false);
    int64_t server_id = -1;
    for (const auto &[k, a] : r.m.abilities) {
        if (a.ability == kQuickWit) {
            server_id = a.instance;
        }
    }
    CHECK(server_id >= 0, "the cast's item names its ServerInstanceId");
    const size_t ends = r.m.end_ids().size();
    r.send_reliable(end_bunch(r.h.ability_guid(), server_id));
    r.play(0.1, false);
    CHECK(r.m.end_ids().size() == ends, "(the answer is not at once)");
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == ends + 1 && r.m.end_ids().back() == id, "259 ms on the client's own instance id is ended");
    r.send_reliable(end_bunch(r.h.ability_guid(), server_id));
    r.send_reliable(end_bunch(r.h.ability_guid(), 987654321));
    r.play(1.0, false);
    CHECK(r.m.end_ids().size() == ends + 1 && !r.h.s->abilities_failed() && r.m.unreadable == 0, "an end of the same instance again, or of one that is not ours, is ignored");
}

static void test_d2_class_resource() {
    std::printf("D2: a Ranger's Focus ticks\n");
    const svc::Character rec = class25(1, "Ranger", 17751);
    Run r(0, true, &rec);
    r.play(0.5, true);
    CHECK(r.m.stats.count(kFocus) == 1, "Focus is one of the Ranger's Owner stats");
    CHECK(r.h.s->set_current_value(kFocus, 10.0f), "(Focus is set to 10)");
    r.tick(false);
    const float start = r.m.stats[kFocus];
    r.play(5.0, false);
    const float later = r.m.stats[kFocus];
    CHECK(near(start, 10.0, 1.0) && later > start + 8.0f && later <= 100.0f, "Focus rises with its regeneration (+3 a second, 10 -> about 25 in 5 s)");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

static void test_d2_sprint_drain() {
    std::printf("D2: a Sprint drains Stamina, and the client's StaminaRegeneration is 0 while it does\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    CHECK(r.m.stats.count(kStaminaRegeneration) != 0 && near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6), "joined: the client's StaminaRegeneration is 5");
    const float full = r.m.stamina_now();
    const size_t before = r.m.stamina.size();
    const int32_t id = r.trigger(kSprint);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the Sprint's first packet sets it to 0 (retail: with the first charge, while the player moves)");
    r.play(5.0, true);
    CHECK(near(full - r.m.stamina_now(), 26.25, 2.6), "5 s of Sprint while moving: Stamina fell by about 25 (5 a second)");
    bool only_down = r.m.stamina.size() > before + 15;
    for (size_t i = before + 1; i < r.m.stamina.size(); ++i) {
        only_down = only_down && near(r.m.stamina[i - 1].second - r.m.stamina[i].second, 1.25, 1e-6);
    }
    CHECK(only_down, "...in steps of 1.25 and never a step up: nothing regenerated meanwhile");
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the client's StaminaRegeneration stayed 0 the whole time");
    r.play(1.0, false);
    CHECK(near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6), "the player stands in a held Sprint: StaminaRegeneration is 5 again");
    r.play(1.0, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "...and 0 when it moves on");
    const float low = r.m.stamina_now();
    r.cancel(id);
    r.play(0.5, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f && r.m.stamina_now() == low, "the Sprint is released: the 1 s pause keeps StaminaRegeneration at 0 and the bar still");
    r.play(1.2, true);
    CHECK(near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6) && r.m.stamina_now() > low, "once the pause is over it is 5 again and the bar rises");
    r.play(8.0, false);
    r.trigger(kDodge);
    r.play(0.2, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "a Dodge sets it to 0 with its first packet [R: the retail capture]");
    r.play(2.0, false);
    CHECK(near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6) && !r.h.s->abilities_failed(), "and it is 5 once the Evasion and the pause are over");
}

int main() {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log);
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    if (ability_rig::real_db_present()) {
        test_d2_glee_heals();
        test_d2_flourish_dash();
        test_d2_dodge_keeps_cooldowns();
        test_d2_floating_numbers();
        test_d2_shielding();
        test_d2_hit_hostile();
        test_d2_sprint_speed();
        test_d2_hostile();
        test_d2_song();
        test_d2_held_cancel();
        test_d2_end_ability();
        test_d2_class_resource();
        test_d2_sprint_drain();
    } else {
        std::printf("D2 ability session tests: skipped (no content.db)\n");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
