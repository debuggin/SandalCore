// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_mounts_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using aoc::protocol::emit::FIntrepidNetworkGUID;

namespace {

bool near_to(double got, double want, double tol = 0.0125) {
    return got >= 0 && std::fabs(got - want) <= tol;
}

struct Seen {
    double item = -1, bar = -1, callout = -1, callout_off = -1, item_end = -1, open = -1;
    double summoning_off = -1, flags = -1, run = -1, flags_off = -1, input_off = -1, gone = -1;
    std::vector<int32_t> ends;
    size_t items = 0, bars = 0, callouts = 0;
    size_t max_mounts = 0;
    double last_x = 0, last_y = 0, last_z = 0, speed_seen = 0;
    bool input_forward = false, invulnerable = false, untargetable = false;
};

struct Rig {
    world::ActorRegistry registry;
    const spawns::MountBook& book;
    std::unique_ptr<MountController> mc;
    std::vector<uint64_t> removed;
    uint32_t listener = 0;
    uint64_t seen_actor = 0;
    uint64_t next_id = 0x7000000;
    int64_t instance = 40;

    explicit Rig(const spawns::MountBook& b) : book(b) {
        MountController::Owner owner;
        owner.player_id = 5;
        owner.guid = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu};
        owner.name = "Rider";
        owner.pawn = mint();
        owner.server_id = 60;
        mc = std::make_unique<MountController>(registry, book, [this] {
            FIntrepidNetworkGUID g;
            g.ObjectId = next_id;
            next_id += 2;
            g.ServerId = 60;
            g.Randomizer = 9;
            return g;
        }, owner);
        mc->set_instance_source([this] { return instance++; });
        listener = registry.on_remove([this](const world::WorldActor& a) { removed.push_back(a.id); });
    }
    ~Rig() { registry.off_remove(listener); }

    MountController::Request request(const content::MountRow& row, double world_time = 500.0) const {
        MountController::Request r;
        r.item_record = row.item;
        r.item_guid = {1, 2, 3, 4};
        r.client_instance = 77;
        r.now_unix = 2'000'000'000;
        r.place = {-777000, 616000, 5000, 90};
        r.owner_level = 20;
        r.world_time = world_time;
        return r;
    }

    const world::WorldActor* mount() const { return mc->mount(); }

    static bool flag(const world::WorldActor& a, const char* cls, const char* path) {
        for (const repl::Subobject& s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != cls) continue;
            const repl::Value* v = s.obj.state.get(*s.obj.schema, path);
            return v && v->get_if<bool>() && *v->get_if<bool>();
        }
        return false;
    }

    static void record_effects(double t, const MountController::Effects& fx, Seen& seen) {
        if (!fx.ability.abilities_added.empty()) {
            if (seen.item < 0) seen.item = t;
            ++seen.items;
        }
        if (!fx.ability.abilities_changed.empty() && seen.item_end < 0) seen.item_end = t;
        if (fx.bar) {
            if (seen.bar < 0) seen.bar = t;
            ++seen.bars;
        }
        if (!fx.callouts.empty()) {
            if (seen.callout < 0) seen.callout = t;
            seen.callouts += fx.callouts.size();
        }
        if (!fx.callouts_removed.empty() && seen.callout_off < 0) seen.callout_off = t;
        for (const int32_t e : fx.ends) seen.ends.push_back(e);
    }

    void record_mount_state(double t, const world::WorldActor& m, Seen& seen) {
        seen_actor = m.id;
        if (seen.open < 0) seen.open = t;
        if (seen.summoning_off < 0 && !flag(m, "UMountComponent", "bIsSummoning")) seen.summoning_off = t;
        if (seen.flags < 0 && flag(m, "UMountComponent", "bIsDesummoning")) seen.flags = t;
        if (seen.flags >= 0 && seen.flags_off < 0 && !flag(m, "UMountComponent", "bIsDesummoning")) seen.flags_off = t;
        seen.invulnerable = seen.invulnerable || flag(m, "UNPCCombatInfo", "bInvulnerable");
        seen.untargetable = seen.untargetable || flag(m, "UNPCCombatInfo", "bNotTargetable");
        const repl::Value* in = m.actor.obj.state.get(*m.actor.obj.schema, "CurrentMovementInput");
        const bool forward = in && in->get_if<repl::PlayerMovementInput>() && in->get_if<repl::PlayerMovementInput>()->bits != 0;
        if (forward && seen.run < 0) seen.run = t;
        if (seen.input_forward && !forward && seen.input_off < 0) seen.input_off = t;
        seen.input_forward = forward;
        seen.last_x = m.x;
        seen.last_y = m.y;
        seen.last_z = m.z;
    }

    void run(double from, double to, Seen& seen, double step = 0.005) {
        for (double t = from; t <= to + 1e-9; t += step) {
            MountController::Effects fx = mc->advance(t);
            record_effects(t, fx, seen);
            seen.max_mounts = std::max(seen.max_mounts, registry.count(world::ActorKind::Mount));
            if (const world::WorldActor* m = mount()) {
                record_mount_state(t, *m, seen);
            } else if (seen_actor != 0 && seen.gone < 0) {
                seen.gone = t;
            }
        }
    }
};

}  // namespace

static void check_summon_ability_and_ground(const spawns::MountBook& book, const content::MountRow& row) {
    Rig again(book);
    again.mc->summon(again.request(row), 10.0);
    MountController::Effects item_fx;
    for (double t = 10.0; t <= 10.2; t += 0.005) {
        MountController::Effects fx = again.mc->advance(t);
        if (!fx.ability.abilities_added.empty()) item_fx = std::move(fx);
    }
    const bootstrap::ActiveAbilityItem* it = item_fx.ability.abilities_added.empty() ? nullptr : &item_fx.ability.abilities_added[0];
    CHECK(it && it->ability_guid == book.summon_ability() && it->server_instance_id == 40 && it->montage_path == MountController::kMontagePath &&
              it->montage_guid == MountController::kMontageGuid && !it->targets_self && item_fx.bar,
          "the item is Summon_Mount with a ServerInstanceId of the pawn's counter and the Summon_Mount montage; the bar goes with it");
    Rig two(book);
    two.mc->summon(two.request(row), 10.0);
    Seen s2;
    two.run(10.0, 12.0, s2);
    two.mc->desummon(12.0);
    two.run(12.0, 16.0, s2);
    two.mc->summon(two.request(row), 17.0);
    MountController::Effects second;
    for (double t = 17.0; t <= 17.2; t += 0.005) {
        MountController::Effects fx = two.mc->advance(t);
        if (!fx.ability.abilities_added.empty()) second = std::move(fx);
    }
    CHECK(second.ability.abilities_removed.size() == 1 && second.ability.abilities_removed[0] == 40 &&
              second.ability.abilities_added.size() == 1 && second.ability.abilities_added[0].server_instance_id == 41,
          "the next summon's item replaces the last one (removed in the same update, the new one has the next instance id)");
    Rig g(book);
    g.mc->set_ground([](double, double, double) -> std::optional<double> { return 1000.0; });
    MountController::Request gr = g.request(row);
    gr.place.z = 1098;
    g.mc->summon(gr, 5.0);
    Seen s3;
    g.run(5.0, 5.3, s3);
    CHECK(g.mount() && std::fabs(g.mount()->z - (1000.0 + MountController::kMountFootOffsetCm)) < 1e-6,
          "with a ground, the mount stands on it (its origin a capsule's half height above)");
}

static void test_summon_timeline(const spawns::MountBook& book) {
    std::printf("a summon at retail's times\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    Rig rig(book);
    const double t0 = 100.0;
    const MountController::Request r = rig.request(*row);
    CHECK(rig.mc->summon(r, t0) == MountController::Outcome::Started && rig.mc->phase() == MountController::Phase::Summoning, "the request starts a summon");
    CHECK(rig.mc->actor_id() == 0 && rig.registry.size() == 0, "nothing is in the world yet");
    Seen seen;
    rig.run(t0, t0 + 2.2, seen);
    CHECK(near_to(seen.item - t0, 0.080) && near_to(seen.bar - t0, 0.080) && seen.items == 1 && seen.bars == 1,
          "the ability item and the bank-18 bar come at +80 ms, once each");
    CHECK(near_to(seen.open - t0, 0.104), "the mount opens at +104 ms");
    CHECK(near_to(seen.summoning_off - t0, 0.104 + 0.470), "bIsSummoning goes off 0.47 s after the open");
    CHECK(near_to(seen.item_end - t0, 1.541), "the ability item ends (no montage) at +1.54 s");
    CHECK(seen.ends.empty() && seen.max_mounts == 1, "no ClientEndAbility, one mount");
    CHECK(rig.mc->phase() == MountController::Phase::Out && !rig.mc->busy() && rig.mount() && rig.mc->item_record() == row->item,
          "the mount is out, nothing is pending");
    if (const world::WorldActor* m = rig.mount()) {
        const double yaw = 90.0 * 3.14159265358979 / 180.0;
        CHECK(std::fabs(m->x - (r.place.x + 575.0 * std::cos(yaw))) < 1e-6 && std::fabs(m->y - (r.place.y + 575.0 * std::sin(yaw))) < 1e-6,
              "it stands 575 cm in front of the player along its yaw");
        CHECK(std::fabs(m->z - (r.place.z - 41.0)) < 1e-6 && m->yaw_degrees == 90.0,
              "at the height retail's was (41 cm under the player's origin, with no ground to read) and with the player's yaw");
        CHECK(m->owner == 5 && m->never_dormant, "owned by the player, never dormant");
    }
    check_summon_ability_and_ground(book, *row);
}

static void check_summon_flood_and_non_mount(const spawns::MountBook& book, const content::MountRow& row) {
    Rig rig(book);
    size_t started = 0;
    size_t desummoned = 0;
    size_t ignored = 0;
    Seen seen;
    double t = 200.0;
    for (int i = 0; i < 60; ++i, t += 0.01) {
        rig.run(t, t, seen);
        switch (rig.mc->summon(rig.request(row), t)) {
            case MountController::Outcome::Started: ++started; break;
            case MountController::Outcome::Desummoning: ++desummoned; break;
            case MountController::Outcome::Ignored: ++ignored; break;
            case MountController::Outcome::Refused: break;
        }
    }
    rig.run(t, t + 8.0, seen);
    CHECK(seen.max_mounts <= 1 && started + desummoned + ignored == 60, "60 summon requests in 0.6 s: never more than one mount in the world");
    CHECK(rig.registry.count(world::ActorKind::Mount) <= 1 && !rig.mc->busy(), "and the controller settles (nothing pending after the dissolves)");
    Rig bad_item(book);
    MountController::Request r = bad_item.request(row);
    r.item_record = 12345;
    CHECK(bad_item.mc->summon(r, 1.0) == MountController::Outcome::Ignored &&
              !bad_item.mc->is_mount_item(12345) && bad_item.mc->is_mount_item(row.item),
          "an item that is no mount is ignored");
    CHECK(!bad_item.mc->desummon(1.0), "a desummon with no mount (and no summon in progress) does nothing");
}

static void test_one_mount(const spawns::MountBook& book) {
    std::printf("one mount per player, summon spam\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    const content::MountRow* other = nullptr;
    for (const content::MountRow& r : book.data().mounts) {
        if (r.kind == "ground" && r.item != row->item) {
            other = &r;
            break;
        }
    }
    Rig rig(book);
    rig.mc->summon(rig.request(*row), 100.0);
    Seen seen;
    rig.run(100.0, 102.0, seen);
    const uint64_t first = rig.mc->actor_id();
    const auto outcome = rig.mc->summon(rig.request(*other), 102.5);
    CHECK(outcome == MountController::Outcome::Desummoning && rig.mc->phase() == MountController::Phase::Desummoning,
          "another mount item used while one is out is a desummon of the old one");
    Seen s2;
    rig.run(102.5, 107.0, s2);
    CHECK(s2.items == 0 && s2.max_mounts == 1 && rig.registry.count(world::ActorKind::Mount) == 0 &&
              rig.removed.size() == 1 && rig.removed[0] == first && rig.mc->phase() == MountController::Phase::Idle,
          "the old mount is gone after its dissolve and nothing else was summoned");
    CHECK(rig.mc->summon(rig.request(*other), 107.5) == MountController::Outcome::Started, "the next use summons");
    Seen s3;
    rig.run(107.5, 110.0, s3);
    CHECK(rig.mount() && rig.mc->item_record() == other->item && rig.registry.count(world::ActorKind::Mount) == 1, "the new mount is the other item's");
    rig.mc->desummon(110.0);
    CHECK(rig.mc->summon(rig.request(*row), 111.0) == MountController::Outcome::Ignored, "a summon while the old mount dissolves is ignored");
    check_summon_flood_and_non_mount(book, *row);
}

static void check_refusal_height_and_locks(const spawns::MountBook& book, const content::MountRow& row) {
    const auto outcome_with = [&](double ground_z) {
        Rig rig(book);
        rig.mc->set_ground([=](double, double, double) -> std::optional<double> { return ground_z; });
        MountController::Request r = rig.request(row);
        return rig.mc->summon(r, 1.0);
    };
    CHECK(outcome_with(4902.0) == MountController::Outcome::Started &&
              outcome_with(4902.0 + 249.0) == MountController::Outcome::Started &&
              outcome_with(4902.0 - 249.0) == MountController::Outcome::Started,
          "ground within 250 cm above or below the player's feet: summoned");
    CHECK(outcome_with(4902.0 + 251.0) == MountController::Outcome::Refused &&
              outcome_with(4902.0 - 251.0) == MountController::Outcome::Refused,
          "a ledge or a drop of more than 250 cm: refused");
    Rig rig(book);
    MountController::Request r = rig.request(row);
    r.lock_expiration = r.now_unix + 60;
    CHECK(rig.mc->summon(r, 1.0) == MountController::Outcome::Refused, "a mount item under lock cannot be summoned");
    Rig past(book);
    MountController::Request p = past.request(row);
    p.lock_expiration = p.now_unix - 60;
    CHECK(past.mc->summon(p, 1.0) == MountController::Outcome::Started, "a lock that ended does not");
    Rig dead(book);
    MountController::Request d = dead.request(row);
    d.dead = true;
    CHECK(dead.mc->summon(d, 1.0) == MountController::Outcome::Refused, "a dead player summons nothing");
}

static void test_refusals(const spawns::MountBook& book) {
    std::printf("a refused summon: no actor, the ability item and the callout\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    {
        Rig rig(book);
        rig.mc->set_ground([](double, double, double) -> std::optional<double> { return std::nullopt; });
        const double t0 = 50.0;
        CHECK(rig.mc->summon(rig.request(*row), t0) == MountController::Outcome::Refused && rig.mc->phase() == MountController::Phase::Idle,
              "no ground in front of the player: refused");
        Seen seen;
        rig.run(t0, t0 + 4.0, seen);
        CHECK(near_to(seen.item - t0, 0.105) && near_to(seen.callout - t0, 0.105) && seen.callouts == 1 && seen.bar < 0,
              "the ability item and the callout come together at +105 ms, with no bar");
        CHECK(near_to(seen.item_end - t0, 1.600), "the item ends at +1.6 s");
        CHECK(near_to(seen.callout_off - t0, 0.105 + 3.0, 0.02), "the callout is taken off 3 s after it was shown");
        CHECK(seen.open < 0 && rig.registry.size() == 0 && seen.ends.empty(), "no mount, no ClientEndAbility");
        CHECK(rig.mc->stats().refused == 1 && rig.mc->stats().summons == 0, "counted as a refusal");
    }
    {
        Rig rig(book);
        rig.mc->set_ground([](double, double, double) -> std::optional<double> { return std::nullopt; });
        rig.mc->summon(rig.request(*row), 1.0);
        std::vector<MountController::Callout> got;
        std::vector<uint64_t> off;
        for (double t = 1.0; t < 6.0; t += 0.01) {
            MountController::Effects fx = rig.mc->advance(t);
            for (auto& c : fx.callouts) got.push_back(c);
            for (const uint64_t k : fx.callouts_removed) off.push_back(k);
        }
        CHECK(got.size() == 1 && got[0].text == "Cannot summon mount here." && got[0].key != 0 && off.size() == 1 && off[0] == got[0].key,
              "the callout says Cannot summon mount here. and is taken off by its key");
    }
    {
        Rig rig(book);
        bool ground = false;
        rig.mc->set_ground([&](double, double, double) -> std::optional<double> { return ground ? std::optional<double>(4902.0) : std::nullopt; });
        rig.mc->summon(rig.request(*row), 1.0);
        Seen seen;
        rig.run(1.0, 1.5, seen);
        CHECK(rig.mc->summon(rig.request(*row), 1.5) == MountController::Outcome::Ignored, "a summon while the refusal plays is ignored");
        rig.run(1.5, 3.0, seen);
        ground = true;
        CHECK(rig.mc->summon(rig.request(*row), 3.0) == MountController::Outcome::Started,
              "once it is over (and the callout still on) the next summon is made");
    }
    check_refusal_height_and_locks(book, *row);
}

static void check_cancel_after_open_and_refusal(const spawns::MountBook& book, const content::MountRow& row, double t0) {
    {
        Rig rig(book);
        rig.mc->summon(rig.request(row), t0);
        Seen seen;
        rig.run(t0, t0 + 0.3, seen);
        CHECK(rig.mount() != nullptr, "(the mount is open)");
        rig.mc->on_damage(t0 + 0.3);
        rig.run(t0 + 0.3, t0 + 5.0, seen);
        CHECK(rig.mount() != nullptr && rig.registry.count(world::ActorKind::Mount) == 1 &&
                  rig.mc->phase() == MountController::Phase::Out && seen.flags < 0,
              "a hit after the open: the mount stays (the user's rule: mounts remain during fights), nothing dissolves");
        CHECK(rig.mc->stats().cancelled == 0 && rig.mc->stats().desummons == 0, "(not counted as a cancel or a desummon)");
        CHECK(seen.summoning_off >= 0, "(the cast runs on: bIsSummoning goes off as without the hit)");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(row), t0);
        Seen seen;
        rig.run(t0, t0 + 1.7, seen);
        rig.mc->on_damage(t0 + 1.7);
        rig.run(t0 + 1.7, t0 + 4.0, seen);
        CHECK(rig.mount() != nullptr && rig.mc->phase() == MountController::Phase::Out && rig.mc->stats().cancelled == 0,
              "a hit after the cast is over does not touch the mount");
        Rig idle(book);
        idle.mc->on_damage(1.0);
        CHECK(idle.mc->phase() == MountController::Phase::Idle && idle.mc->stats().cancelled == 0, "a hit with nothing summoned does nothing");
    }
    {
        Rig rig(book);
        rig.mc->set_ground([](double, double, double) -> std::optional<double> { return std::nullopt; });
        rig.mc->summon(rig.request(row), t0);
        Seen seen;
        rig.run(t0, t0 + 0.5, seen);
        rig.mc->on_damage(t0 + 0.5);
        rig.run(t0 + 0.5, t0 + 4.0, seen);
        CHECK(seen.item_end >= 0 && near_to(seen.item_end, t0 + 0.5, 0.011) && seen.callout_off >= 0,
              "a hit during a refusal ends its item at once, the callout still goes off");
    }
}

static void test_cancel(const spawns::MountBook& book) {
    std::printf("a damaging hit cancels a summon in progress\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    const double t0 = 300.0;
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), t0);
        Seen seen;
        rig.run(t0, t0 + 0.04, seen);
        rig.mc->on_damage(t0 + 0.04);
        rig.run(t0 + 0.04, t0 + 5.0, seen);
        CHECK(seen.item < 0 && seen.bar < 0 && seen.open < 0 && rig.registry.size() == 0 &&
                  rig.mc->phase() == MountController::Phase::Idle && !rig.mc->busy(),
              "a hit before the item: nothing is shown, nothing spawns, nothing is pending");
        CHECK(rig.mc->stats().cancelled == 1, "counted");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), t0);
        Seen seen;
        rig.run(t0, t0 + 0.09, seen);
        CHECK(seen.item >= 0 && seen.open < 0, "(the item is shown and the mount is not open yet)");
        rig.mc->on_damage(t0 + 0.09);
        rig.run(t0 + 0.09, t0 + 5.0, seen);
        CHECK(seen.item_end >= 0 && near_to(seen.item_end, t0 + 0.09, 0.011) && seen.open < 0 && rig.registry.size() == 0,
              "a hit between the item and the open: the item ends at once and the mount never opens");
    }
    check_cancel_after_open_and_refusal(book, *row, t0);
}

static void test_desummon(const spawns::MountBook& book) {
    std::printf("a desummon: the dissolve, the run, the close at +3.2 s\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    Rig rig(book);
    rig.mc->set_speeds([](const std::string&) { return 990.0; });
    rig.mc->summon(rig.request(*row), 400.0);
    Seen warm;
    rig.run(400.0, 403.0, warm);
    const world::WorldActor* m = rig.mount();
    CHECK(m != nullptr && rig.mc->phase() == MountController::Phase::Out, "(a mount is out)");
    if (!m) return;
    const uint64_t id = m->id;
    const double x0 = m->x;
    const double y0 = m->y;
    const uint32_t v0 = m->version;
    const double d0 = 410.0;
    CHECK(rig.mc->desummon(d0) && rig.mc->phase() == MountController::Phase::Desummoning, "the request begins the desummon");
    CHECK(!rig.mc->desummon(d0 + 0.01) && rig.mc->summon(rig.request(*row), d0 + 0.02) == MountController::Outcome::Ignored,
          "a second desummon, or a summon, while it dissolves does nothing");
    Seen seen;
    rig.run(d0, d0 + 4.0, seen);
    CHECK(near_to(seen.flags - d0, 0.100), "bIsDesummoning goes on at +0.1 s (retail +72 and +118 ms)");
    CHECK(seen.invulnerable && seen.untargetable, "invulnerable and untargetable");
    CHECK(near_to(seen.run - d0, 0.200), "the run begins at +0.2 s (CurrentMovementInput forward)");
    CHECK(near_to(seen.flags_off - d0, 3.100), "bIsDesummoning goes off at +3.1 s");
    CHECK(near_to(seen.input_off - d0, 3.100),
          "the movement input ends with the flag, at +3.1 s (the close is 100 ms later: a connection's pass is 67 ms, so no update is lost to it)");
    CHECK(near_to(seen.gone - d0, 3.200), "the channel closes (the actor leaves the registry) at +3.2 s");
    CHECK(rig.removed.size() == 1 && rig.removed[0] == id && rig.mc->phase() == MountController::Phase::Idle &&
              !rig.mc->busy() && rig.mc->actor_id() == 0,
          "removed once, the controller is idle");
    const double dx = seen.last_x - x0;
    const double dy = seen.last_y - y0;
    CHECK(std::fabs(dx) < 1.0 && std::fabs(dy - 990.0 * 2.9) < 990.0 * 0.15,
          "it ran forward along its yaw at its ground speed: 990 cm/s for about 2.9 s");
    CHECK(v0 > 0, "(the actor's version moved with every step)");
    Rig slow(book);
    slow.mc->summon(slow.request(*row), 1.0);
    Seen w;
    slow.run(1.0, 3.0, w);
    const double sx = slow.mount()->x;
    const double sy = slow.mount()->y;
    slow.mc->desummon(3.0);
    Seen w2;
    slow.run(3.0, 3.0 + 3.15, w2);
    const double expect = 600.0 * (row->ground_pct > 0 ? row->ground_pct / 100.0 : 1.0) * 2.9;
    CHECK(std::fabs((w2.last_y - sy) - expect) < expect * 0.1 && std::fabs(w2.last_x - sx) < 1.0,
          "with no speed table it runs at 600 cm/s times the item's ground percentage");
}

static void test_desummon_early(const spawns::MountBook& book) {
    std::printf("a desummon during the summon, a mount that left, the session's end\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 10.0);
        Seen seen;
        rig.run(10.0, 10.09, seen);
        CHECK(rig.mc->desummon(10.09), "a desummon before the open (the item is shown, the mount is not there)");
        rig.run(10.09, 15.0, seen);
        CHECK(seen.open < 0 && rig.registry.size() == 0 && rig.mc->phase() == MountController::Phase::Idle && seen.item_end >= 0,
              "no mount ever opens and the item ends");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 10.0);
        Seen seen;
        rig.run(10.0, 10.2, seen);
        CHECK(rig.mount() != nullptr && rig.mc->desummon(10.2), "a desummon while the mount is summoning (open, the cast not over)");
        rig.run(10.2, 15.0, seen);
        CHECK(seen.flags >= 0 && seen.gone >= 0 && rig.registry.count(world::ActorKind::Mount) == 0 && seen.item_end >= 0,
              "it dissolves and goes, and the cast's item ends");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 10.0);
        Seen seen;
        rig.run(10.0, 12.0, seen);
        rig.registry.remove(rig.mc->actor_id());
        rig.run(12.0, 12.1, seen);
        CHECK(rig.mc->phase() == MountController::Phase::Idle && rig.mc->actor_id() == 0 && !rig.mc->desummon(12.2),
              "a mount that left the world is no mount: idle, nothing to desummon");
        CHECK(rig.mc->summon(rig.request(*row), 13.0) == MountController::Outcome::Started, "and the next summon is made");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 10.0);
        Seen seen;
        rig.run(10.0, 12.0, seen);
        const uint64_t id = rig.mc->actor_id();
        rig.mc->shutdown();
        CHECK(rig.registry.size() == 0 && rig.removed.size() == 1 && rig.removed[0] == id &&
                  rig.mc->phase() == MountController::Phase::Idle && !rig.mc->busy(),
              "shutdown removes the mount (every view closes it) and clears the steps");
        Rig mid(book);
        mid.mc->summon(mid.request(*row), 10.0);
        mid.mc->shutdown();
        Seen s2;
        mid.run(10.0, 12.0, s2);
        CHECK(s2.open < 0 && mid.registry.size() == 0, "shutdown during the cast: nothing spawns afterwards");
    }
}

void run_mounts_summon() {
    test_summon_timeline(*g_book);
    test_one_mount(*g_book);
    test_refusals(*g_book);
    test_cancel(*g_book);
    test_desummon(*g_book);
    test_desummon_early(*g_book);
}
