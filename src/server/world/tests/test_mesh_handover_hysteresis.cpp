#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "net/tick_loop.h"
#include "session/player_world.h"
#include "spawns/area.h"
#include "world/map_stream.h"

namespace {

using namespace aoc::worldd;

std::pair<double, double> find_riverlands_sandsquall_border() noexcept {
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

void test_polygon_cutout_and_scope_defaults() {
  const auto bad = spawns::AreaConfig::parse_polygon_cm("100,200;300,400");
  assert(bad.size() == 2);

  const auto square =
      spawns::AreaConfig::parse_polygon_cm("0,0;10000,0;10000,10000;0,10000");
  assert(square.size() == 4);

  spawns::AreaConfig cutout = spawns::AreaConfig::continent();
  cutout.polygon_cutout_cm = square;
  assert(cutout.scope_kind() == "polygon_cutout");
  assert(cutout.contains(5000.0, 5000.0));
  assert(!cutout.contains(15000.0, 5000.0));

  spawns::AreaConfig parent = spawns::AreaConfig::riverlands();
  parent.excluded_polygons_cm.push_back(square);
  assert(!parent.contains(5000.0, 5000.0));
  assert(parent.contains(-600000.0, 500000.0));

  assert(spawns::AreaConfig::continent().default_tick_hz_for_scope() == 30);
  assert(spawns::AreaConfig::continent().default_npc_sim_step_cap_for_scope() >=
         5200);
  assert(spawns::AreaConfig::riverlands().default_tick_hz_for_scope() == 45);
  assert(
      spawns::AreaConfig::riverlands().default_npc_sim_step_cap_for_scope() >=
      3600);
  assert(cutout.default_tick_hz_for_scope() == 100);
  assert(cutout.default_npc_sim_step_cap_for_scope() >= 1500);
}

void test_six_partition_topologies_modes_1_to_4() {
  using Mode = spawns::PartitionTopologyMode;
  const auto rnd_region = spawns::pick_random_region(42);
  assert(rnd_region != spawns::RegionBit::None);

  const auto m1 = spawns::build_partition_topology(Mode::SingleRiverlands);
  assert(m1.size() == 1 && m1[0].scope_kind() == "region");
  assert(m1[0].region_mask ==
         static_cast<uint8_t>(spawns::RegionBit::Riverlands));
  assert(m1[0].default_tick_hz_for_scope() == 45);

  const auto m2 = spawns::build_partition_topology(Mode::SingleWorld);
  assert(m2.size() == 1 && m2[0].scope_kind() == "world");
  assert(m2[0].default_tick_hz_for_scope() == 30);
  assert(m2[0].default_npc_sim_step_cap_for_scope() >= 5200);

  const auto m3 = spawns::build_partition_topology(Mode::PerRegion);
  assert(m3.size() == 5);
  uint8_t mask_union = 0;
  for (const auto& w : m3) {
    assert(w.default_tick_hz_for_scope() == 45);
    mask_union |= w.region_mask;
  }
  assert(mask_union == static_cast<uint8_t>(spawns::RegionBit::All));

  const auto m4 =
      spawns::build_partition_topology(Mode::PerZoiInRegion, rnd_region);
  assert(m4.size() >= 2);
  for (const auto& w : m4) {
    assert(w.scope_kind() == "zoi" && w.default_tick_hz_for_scope() == 60);
    assert(w.default_npc_sim_step_cap_for_scope() >= 2600);
  }
}

void test_six_partition_topologies_modes_5_and_6() {
  using Mode = spawns::PartitionTopologyMode;
  const auto m5 = spawns::build_partition_topology(
      Mode::PerZoiAndNodePlugsInRegion, spawns::RegionBit::Riverlands);
  const spawns::AreaConfig* plug_worker = nullptr;
  for (const auto& w : m5) {
    if (w.scope_kind() == "node_plug") plug_worker = &w;
  }
  assert(plug_worker != nullptr && !plug_worker->include_node_plugs.empty());
  assert(plug_worker->default_tick_hz_for_scope() == 100);
  assert(plug_worker->default_npc_sim_step_cap_for_scope() >= 1500);
  const double cx = plug_worker->include_node_plugs.front().cx_cm;
  const double cy = plug_worker->include_node_plugs.front().cy_cm;
  assert(plug_worker->contains(cx, cy));
  for (const auto& w : m5) {
    if (w.scope_kind() == "zoi") assert(!w.contains(cx, cy));
  }

  const auto m6 = spawns::build_partition_topology(
      Mode::PerZoiNodePlugsAndPoisInRegion, spawns::RegionBit::Riverlands);
  const spawns::AreaConfig* poi_worker = nullptr;
  for (const auto& w : m6) {
    if (w.scope_kind() == "polygon_cutout") poi_worker = &w;
  }
  assert(poi_worker != nullptr && poi_worker->polygon_cutout_cm.size() >= 3);
  assert(poi_worker->default_tick_hz_for_scope() == 100);
  double px = 0.0;
  double py = 0.0;
  for (const auto& [vx, vy] : poi_worker->polygon_cutout_cm) {
    px += vx;
    py += vy;
  }
  px /= static_cast<double>(poi_worker->polygon_cutout_cm.size());
  py /= static_cast<double>(poi_worker->polygon_cutout_cm.size());
  assert(poi_worker->contains(px, py));
  for (const auto& w : m6) {
    if (w.scope_kind() == "zoi") assert(!w.contains(px, py));
  }
}

void test_boundary_zigzag_and_mounted_handover() {
  aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
  PlayerWorld w1(nullptr, "", bootstrap::GameStateMode::Tier2, 60, guids);
  PlayerWorld w2(nullptr, "", bootstrap::GameStateMode::Tier2, 61, guids);
  const auto border = find_riverlands_sandsquall_border();

  assert(w1.step_player_border_handover(101, border.first,
                                        border.second + 2500.0, 100) ==
         world::BorderDecision::StayLocal);
  assert(w1.step_player_border_handover(101, border.first,
                                        border.second - 2500.0, 200) ==
         world::BorderDecision::StayLocal);
  assert(w1.step_player_border_handover(101, border.first,
                                        border.second + 8000.0, 300) ==
         world::BorderDecision::StayLocal);
  assert(w1.step_player_border_handover(101, border.first,
                                        border.second + 8000.0, 400) ==
         world::BorderDecision::StayLocal);
  assert(w1.step_player_border_handover(101, border.first,
                                        border.second + 8000.0, 500) ==
         world::BorderDecision::HandoverPending);

  PlayerWorld::HandoverMountSnapshot snap{};
  snap.active = true;
  snap.mount_item_record = 77001;
  snap.mount_actor_guid = 88001;
  snap.speed_multiplier = 1.65;
  snap.movement_mode = 2;
  w1.remember_handover_mount("char-101", snap);
  w1.commit_player_handover(101, 500);
  const auto taken = w1.take_handover_mount("char-101");
  assert(taken.has_value());
  w2.remember_handover_mount("char-101", *taken);

  const auto restored = w2.take_handover_mount("char-101");
  assert(restored.has_value() && restored->active);
  assert(restored->mount_item_record == 77001 &&
         restored->speed_multiplier == 1.65);
}

void test_cross_boundary_halo_and_hostile_npc_audit() {
  aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
  PlayerWorld w1(nullptr, "", bootstrap::GameStateMode::Tier2, 60, guids);
  PlayerWorld w2(nullptr, "", bootstrap::GameStateMode::Tier2, 61, guids);

  world::WorldActor player_a{};
  player_a.id = 102;
  player_a.actor.guid.ObjectId = player_a.id;
  player_a.kind = world::ActorKind::Player;
  player_a.x = w2.area().center_x + 10000.0;
  player_a.y = w2.area().center_y;
  player_a.label = "PlayerA";
  assert(w1.registry().add(player_a));

  world::WorldActor player_b = player_a;
  player_b.id = 202;
  player_b.actor.guid.ObjectId = player_b.id;
  player_b.x = w1.area().center_x + 12000.0;
  player_b.label = "PlayerB";
  assert(w2.registry().add(player_b));

  world::WorldActor monster{};
  monster.id = 5002;
  monster.actor.guid.ObjectId = monster.id;
  monster.kind = world::ActorKind::Npc;
  monster.x = w1.area().center_x + 8000.0;
  monster.y = w1.area().center_y;
  monster.npc.name = "Goblin Scout";
  monster.npc.aggro_radius_cm = 1200.0;
  assert(w2.registry().add(monster));

  world::WorldActor pile{};
  pile.id = 6002;
  pile.actor.guid.ObjectId = pile.id;
  pile.kind = world::ActorKind::Pile;
  pile.x = w1.area().center_x + 5000.0;
  pile.y = w1.area().center_y;
  pile.label = "APlayerLootPile";
  assert(w2.registry().add(pile));

  assert(w1.sync_boundary_halo_actors(w2) >= 4);
  assert(w1.registry().find(202) != nullptr);
  assert(w1.registry().find(5002) != nullptr);
  assert(w1.registry().find(6002) != nullptr);
  assert(w2.registry().find(102) != nullptr);

  TickLoop::set_global_runtime_tick_hz(25);
  assert(TickLoop::effective_tick_hz(30) == 25);
  TickLoop::set_global_runtime_tick_hz(500);
  assert(TickLoop::effective_tick_hz(30) == 120);
  TickLoop::set_global_runtime_tick_hz(0);
  assert(TickLoop::effective_tick_hz(30) == 30);

  world::WorldActor raven{};
  raven.kind = world::ActorKind::Npc;
  raven.npc.name = "Raven";
  world::WorldActor mycelette = raven;
  mycelette.npc.name = "Mycelette";
  world::WorldActor guard = raven;
  guard.npc.name = "Citizen Guard";
  assert(world::mapstream::is_hostile_npc(raven));
  assert(world::mapstream::is_hostile_npc(mycelette));
  assert(!world::mapstream::is_hostile_npc(guard));
}

}  // namespace

int main() {
  test_polygon_cutout_and_scope_defaults();
  test_six_partition_topologies_modes_1_to_4();
  test_six_partition_topologies_modes_5_and_6();
  test_boundary_zigzag_and_mounted_handover();
  test_cross_boundary_halo_and_hostile_npc_audit();
  std::printf("[PASS] test_mesh_handover_hysteresis passed\n");
  return 0;
}
