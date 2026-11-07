#include <cassert>
#include <cstdlib>

#include "npc/mover.h"
#include "spawns/area.h"
#include "tests/nav_fixture.h"
#include "tests/test_world_ability_session_common.h"

namespace {

using namespace aoc::worldd;

#if defined(_WIN32)
void set_env_var(const char* key, const char* val) { _putenv_s(key, val); }
#else
void set_env_var(const char* key, const char* val) { setenv(key, val, 1); }
#endif

struct BorderAnchor {
  double x_cm = -459889.6;
  double y_cm = 0.0;
};

BorderAnchor find_riverlands_sandsquall_border() noexcept {
  const double x = -459889.6;
  double lo = 599317.1;
  double hi = 873514.2;
  for (int i = 0; i < 40; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (spawns::resolve_point_region_bit(x, mid) ==
        spawns::RegionBit::Riverlands) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return {x, 0.5 * (lo + hi)};
}

void test_region_resolution_and_signed_distance(const BorderAnchor& border) {
  CHECK(spawns::region_bit_slug(spawns::RegionBit::Riverlands) == "riverlands",
        "region_bit_slug maps Riverlands");
  CHECK(spawns::resolve_point_region_bit(border.x_cm, border.y_cm - 2500.0) ==
            spawns::RegionBit::Riverlands,
        "point -2500 cm south of border resolves to Riverlands");
  CHECK(spawns::resolve_point_region_bit(border.x_cm, border.y_cm + 2500.0) ==
            spawns::RegionBit::Sandsquall,
        "point +2500 cm north of border resolves to Sandsquall");

  const float dist_inside = spawns::signed_region_border_distance_cm(
      border.x_cm, border.y_cm - 2500.0, spawns::RegionBit::Riverlands);
  const float dist_shallow = spawns::signed_region_border_distance_cm(
      border.x_cm, border.y_cm + 2500.0, spawns::RegionBit::Riverlands);
  const float dist_deep = spawns::signed_region_border_distance_cm(
      border.x_cm, border.y_cm + 8000.0, spawns::RegionBit::Riverlands);
  CHECK(dist_inside <= 0.0f && dist_shallow > 0.0f &&
            dist_shallow < world::kRegionHandoverHysteresisCm &&
            dist_deep >= world::kRegionHandoverHysteresisCm,
        "signed_region_border_distance_cm matches deadband and penetration");
}

void test_boundary_jitter_and_sustained_crossing(const BorderAnchor& border) {
  world::RegionalHandoverState state{};
  state.authoritative_region = spawns::RegionBit::Riverlands;
  state.candidate_region = spawns::RegionBit::Riverlands;

  int64_t now_ms = 1000;
  for (int i = 0; i < 10; ++i) {
    const double offset_cm = (i % 2 == 0) ? 2500.0 : -2500.0;
    const auto dec = world::evaluate_regional_handover(
        state, border.x_cm, border.y_cm + offset_cm, now_ms);
    CHECK(dec == world::BorderDecision::StayLocal,
          "oscillation within +/-2500 cm stays local");
    now_ms += 100;
  }
  CHECK(state.handover_count == 0 && state.last_suppressed_by_hysteresis,
        "10 border oscillations produce 0 handovers");

  const auto d1 = world::evaluate_regional_handover(
      state, border.x_cm, border.y_cm + 8000.0, 2000);
  const auto d2 = world::evaluate_regional_handover(
      state, border.x_cm, border.y_cm + 8000.0, 2100);
  const auto d3 = world::evaluate_regional_handover(
      state, border.x_cm, border.y_cm + 8000.0, 2200);
  CHECK(d1 == world::BorderDecision::StayLocal &&
            d2 == world::BorderDecision::StayLocal &&
            d3 == world::BorderDecision::HandoverPending,
        "3 consecutive samples beyond 5000 cm commit handover");
  for (int i = 0; i < 3; ++i) {
    CHECK(world::evaluate_regional_handover(
              state, border.x_cm, border.y_cm - 8000.0, 2700 + i * 50) ==
              world::BorderDecision::StayLocal,
          "reverse crossing inside 2000ms cooldown is suppressed");
  }
  world::BorderDecision back_dec = world::BorderDecision::StayLocal;
  for (int i = 0; i < 3; ++i) {
    back_dec = world::evaluate_regional_handover(
        state, border.x_cm, border.y_cm - 8000.0, 4700 + i * 100);
  }
  CHECK(back_dec == world::BorderDecision::HandoverPending &&
            state.handover_count == 2 && state.lease_epoch == 3,
        "reverse crossing after >=2000ms cooldown commits");
}

void test_16_bot_level50_8_class_roster_and_embersprings() {
  set_env_var("AOC_WORLD_UNCLAMP_LEVEL_50", "1");
  PlayerSession::set_global_default_level(50);
  PlayerSession::set_global_unlock_all_embersprings(true);

  struct Spec {
    int id;
    const char* name;
    int64_t arch;
  };
  const Spec specs[8] = {
      {1, "Fighter", 17746},  {2, "Bard", 17747},     {3, "Cleric", 17748},
      {4, "Mage", 17749},     {5, "Ranger", 17750},   {6, "Rogue", 17751},
      {7, "Summoner", 17752}, {8, "Tank", 17753},
  };

  int spawned_bots = 0;
  for (const auto& sp : specs) {
    for (int copy = 0; copy < 2; ++copy) {
      svc::Character rec = class25(sp.id, sp.name, sp.arch);
      rec.level = 50;
      Harness bot(0, false, &rec);
      const size_t unlocked = bot.s->unlock_all_embersprings(&bot.conn);
      CHECK(bot.s->progress().level == 50, "bot spawned at Max Level 50");
      CHECK(!bot.s->known_abilities().empty(), "bot class skills unlocked");
      CHECK(unlocked >= 1 && !bot.s->attunements().discovered.empty() &&
                bot.s->attunements().home != 0,
            "all Ember Springs unlocked and home shrine attuned");
      ++spawned_bots;
    }
  }
  CHECK(spawned_bots == 16, "spawned 16 Level-50 bots across all 8 classes");
}

void test_navmesh_boundary_zigzag_and_mounted_crossing() {
  nav::AreaNav& nav = navfix::shared_nav();
  npc::Mover mover;
  mover.init(&nav, 88.0, 360.0);
  mover.place(-1500.0, 3000.0, 1000.0, 0.0);
  CHECK(mover.move_to({1500.0, 3000.0, 1000.0}, 600.0),
        "NavMesh path across x=0 boundary found");
  for (int i = 0; i < 40 && mover.moving(); ++i) mover.advance(0.2);
  CHECK(mover.ground().x > 500.0, "bot crossed x=0 boundary via NavMesh");

  CHECK(mover.move_to({-1500.0, 3000.0, 1000.0}, 600.0),
        "NavMesh return zig-zag path found");
  for (int i = 0; i < 40 && mover.moving(); ++i) mover.advance(0.2);
  CHECK(mover.ground().x < -500.0, "bot zig-zagged back across x=0 boundary");
}

void test_emberspring_respawn_and_cross_boundary_combat_loot() {
  set_env_var("AOC_WORLD_UNCLAMP_LEVEL_50", "1");
  PlayerSession::set_global_default_level(50);
  PlayerSession::set_global_unlock_all_embersprings(true);

  svc::Character rec = bard25();
  rec.level = 50;
  rec.items.push_back(
      {svc::ItemContainer::Backpack, 0, 109162169652920ll,
       "00005001000000110000002200000033", 20, 1, 0.0f, 1700000000000ll});
  Run r(0, false, &rec);
  CHECK(r.h.s->progress().level == 50 &&
            r.h.s->unlock_all_embersprings(&r.h.conn) >= 1,
        "Level-50 combat bot with unlocked Ember Springs ready");

  r.trigger(kScepterCombo);
  r.play(0.3, true);
  CHECK(!r.h.s->abilities_failed(),
        "Level-50 class ability triggered cleanly while moving");

  aoc::protocol::ServerGuidAllocator guids2{61, 0x6ee87ee4u};
  PlayerWorld w2(nullptr, "", bootstrap::GameStateMode::Tier2, 61, guids2);
  world::WorldActor pile{};
  pile.id = 99002;
  pile.actor.guid.ObjectId = pile.id;
  pile.kind = world::ActorKind::Pile;
  pile.x = r.h.fx.world->area().center_x + 4000.0;
  pile.y = r.h.fx.world->area().center_y;
  pile.label = "APlayerLootPile";
  const bool added_pile = r.h.fx.world->registry().add(pile);
  CHECK(added_pile && r.h.fx.world->sync_boundary_halo_actors(w2) >= 1 &&
            w2.registry().find(99002) != nullptr,
        "player loot pile synced across worker boundary within 350m AoI halo");
}

}  // namespace

int main() {
  const BorderAnchor border = find_riverlands_sandsquall_border();
  test_region_resolution_and_signed_distance(border);
  test_boundary_jitter_and_sustained_crossing(border);
  test_16_bot_level50_8_class_roster_and_embersprings();
  test_navmesh_boundary_zigzag_and_mounted_crossing();
  test_emberspring_respawn_and_cross_boundary_combat_loot();

  std::printf("\nResult: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
