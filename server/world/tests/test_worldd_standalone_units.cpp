#include "quest/registry_points.h"
#include "vendor/node_tax.h"
#include "respawn/make_shrines.h"
#include "crafting/quick.h"
#include "crafting/rules.h"
#include "svc/world_stream.h"

namespace aoc { namespace worldd { namespace svc {

class WorldStreamServerTest {
public:
    using Directory = std::function<std::vector<PlayerHandle>()>;

    struct Options {
        std::string               address = "127.0.0.1:50057";
        std::string               token;
        std::chrono::milliseconds call_timeout{5000};
        size_t                    max_subscribers = 8;
    };

    WorldStreamServerTest(PlayerWorld& world, Directory directory, AdminQueue& queue, Options options);
    ~WorldStreamServerTest();
    WorldStreamServerTest(const WorldStreamServerTest&) = delete;
    WorldStreamServerTest& operator=(const WorldStreamServerTest&) = delete;

    bool start(std::string* error);
    void stop();
    void publish();
    void note_loop(std::chrono::microseconds busy);
    int port() const { return port_; }
    size_t subscribers() const;
    uint64_t refused() const;

    struct Impl;
    std::unique_ptr<Impl>         impl_;
    std::unique_ptr<grpc::Server> server_;
    int                           port_ = 0;
};

}}} // namespace aoc::worldd::svc

#define WorldStreamServer WorldStreamServerTest
#include "svc/world_stream_impl.h"
#undef WorldStreamServer

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void check_cond(bool ok, int line, const char* expr, const char* msg) {
    if (ok) {
        ++g_pass;
        std::printf("  [ok ] %s\n", msg);
    } else {
        ++g_fail;
        std::printf("  [FAIL] line %d: %s (%s)\n", line, msg, expr);
    }
    std::fflush(stdout);
}

#define CHECK(cond, msg) check_cond(static_cast<bool>(cond), __LINE__, #cond, msg)

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;

void populate_registry_fixtures(world::ActorRegistry& reg, content::PopulationData& pop) {
    world::WorldActor a1{};
    a1.id = 2;
    a1.actor.guid.ObjectId = 2;
    a1.kind = world::ActorKind::Npc;
    a1.npc.instance_guid = 100;
    a1.npc.home_x = 500.0;
    a1.npc.home_y = 200.0;
    a1.z = 30.0;
    reg.add(a1);

    world::WorldActor a2 = a1;
    a2.id = 4;
    a2.actor.guid.ObjectId = 4;
    a2.npc.home_x = 100.0;
    a2.npc.home_y = 400.0;
    a2.z = 10.0;
    reg.add(a2);

    world::WorldActor a3 = a1;
    a3.id = 6;
    a3.actor.guid.ObjectId = 6;
    a3.npc.instance_guid = 0;
    reg.add(a3);

    world::WorldActor a4 = a1;
    a4.id = 8;
    a4.actor.guid.ObjectId = 8;
    a4.kind = world::ActorKind::Gatherable;
    reg.add(a4);

    pop.world_spawns[10] = {10, "Main", "", "Verra_World_Master", false, 0};
    pop.world_spawns[20] = {20, "Inst", "", "Dungeon_Map", false, 0};
    pop.placements.push_back({10, 1, 1000, 10.0, 20.0, 30.0, 0.0, 1, 1});
    pop.placements.push_back({10, 2, 1000, 0.0, 0.0, 0.0, 0.0, 1, 1});
    pop.placements.push_back({20, 3, 1000, 99.0, 99.0, 99.0, 0.0, 1, 1});
    pop.placements.push_back({10, 4, 2000, 50.0, 60.0, 70.0, 0.0, 1, 1});

    content::NpcSetRow s1;
    s1.guid = 1000;
    s1.name = "S1";
    s1.members = {{1, 301, 5.0, 6.0, 7.0, 0.0, false}, {2, 302, 1.0, 2.0, 3.0, 0.0, false}};
    pop.sets[1000] = s1;

    content::NpcSetRow s2;
    s2.guid = 2000;
    s2.name = "S2";
    s2.members = {{3, 0, 0.0, 0.0, 0.0, 0.0, false}};
    pop.sets[2000] = s2;
}

void test_quest_registry_points() {
    std::printf("--- quest/registry_points.h ---\n");
    world::ActorRegistry reg;
    content::PopulationData pop;
    populate_registry_fixtures(reg, pop);

    quest::RegistryPoints rp(reg, &pop);
    std::vector<quest::MarkerPoint> out;

    rp.npc_places(100, out);
    CHECK(out.size() == 2 && out[0].x == 100.0 && out[1].x == 500.0, "npc_places returns sorted live NPC coordinates");

    out.clear();
    rp.npc_places(301, out);
    CHECK(out.size() == 1 && out[0].x == 15.0 && out[0].y == 26.0 && out[0].z == 37.0, "npc_places falls back to build_data and data_places");

    out.clear();
    rp.npc_places(999999, out);
    CHECK(out.empty(), "npc_places returns empty for missing instance");

    out.clear();
    rp.creature_places({301}, {1000}, out);
    CHECK(out.size() == 2 && out[0].x == 11.0 && out[1].x == 15.0, "creature_places deduplicates set+instance and sorts via sort_from");

    out.clear();
    rp.creature_places({302}, {}, out);
    CHECK(out.size() == 1 && out[0].x == 11.0 && out[0].y == 22.0 && out[0].z == 33.0, "creature_places resolves instance holders");

    out.clear();
    rp.creature_places({999999}, {888888}, out);
    CHECK(out.empty(), "creature_places returns empty on unknown instance and set");

    quest::RegistryPoints rp_null(reg, nullptr);
    out.clear();
    rp_null.creature_places({301}, {1000}, out);
    CHECK(out.empty(), "creature_places handles null PopulationData");
}

void test_vendor_node_tax() {
    std::printf("--- vendor/node_tax.h & vendor/town_shops.h ---\n");
    const int64_t node_id = 609001234567LL;
    repl::NodeTaxDetails untaxed = vendor::untaxed_table(node_id);
    repl::NodeTaxDetails def_tbl = vendor::default_table(node_id);

    CHECK(untaxed.node == node_id, "untaxed_table sets node_guid");
    CHECK(def_tbl.node == node_id, "default_table sets node_guid");
    CHECK(untaxed.entries.size() == 78 && def_tbl.entries.size() == 78, "both tables contain 78 entries");

    bool all_zero = true;
    for (const auto& e : untaxed.entries) {
        if (e.flat_tax != 0 || e.tax_percent != 0.0f) {
            all_zero = false;
        }
    }
    CHECK(all_zero, "untaxed_table clears all flat_tax and tax_percent rates");

    int active_roots = 0;
    for (const auto& e : def_tbl.entries) {
        if (e.tag == "NodeTax" && e.flag_a && e.flag_b && std::fabs(e.tax_percent - 0.5f) < 1e-6f) {
            ++active_roots;
        }
    }
    CHECK(active_roots == 1, "default_table activates root NodeTax at 50%");

    vendor::TaxRate empty_rate = vendor::default_tax_rate("");
    vendor::TaxRate none_rate = vendor::default_tax_rate("None");
    vendor::TaxRate child_rate = vendor::default_tax_rate("NodeTax.GoodsTax.MarketplaceTax");
    vendor::TaxRate unknown_rate = vendor::default_tax_rate("UnknownRoot.Tag");

    CHECK(empty_rate.percent == 0.0f && none_rate.percent == 0.0f, "empty and None tags have 0% tax");
    CHECK(std::fabs(child_rate.percent - 0.5f) < 1e-6f, "child tag climbs hierarchy to active NodeTax root");
    CHECK(unknown_rate.percent == 0.0f, "unknown root tag resolves to 0% tax");
    CHECK(vendor::taxed_unit(0, child_rate) == 0, "taxed_unit returns 0 for non-positive price");
    CHECK(vendor::taxed_unit(23, child_rate) == 35, "taxed_unit rounds up 23 + ceil(11.5) = 35");
}

void populate_shrine_data(content::RespawnData& data, std::vector<spawns::RespawnPoint>& points) {
    data.respawns[10] = {10, "ValidShrine", "", respawn::kAttunedRespawnKind, 100, 1.0, 1.0, 1.0, 0.0, 500.0};
    data.respawns[20] = {20, "WrongKind", "", 999, 100, 1.0, 1.0, 1.0, 0.0, 500.0};
    data.respawns[30] = {30, "NoPlace", "", respawn::kAttunedRespawnKind, 999, 1.0, 1.0, 1.0, 0.0, 500.0};
    data.respawns[40] = {40, "Unnumbered", "", respawn::kAttunedRespawnKind, 200, 1.0, 1.0, 1.0, 0.0, 500.0};

    data.places[100] = {100, "PlaceNumbered", "", "Verra", 7, 1000.0, 2000.0, 300.0, 250.0};
    data.places[200] = {200, "PlaceUnnumbered", "", "Verra", 0, 4000.0, 5000.0, 600.0, 250.0};

    data.instance_respawn[5001] = 10;
    data.instance_respawn[5002] = 20;
    data.instance_respawn[5003] = 30;
    data.instance_respawn[5004] = 40;
    data.instance_respawn[5005] = 10;

    points.push_back({1.0, 2.0, 3.0, 0.0, false, "NotAttunable", 5000});
    points.push_back({1.0, 2.0, 3.0, 0.0, true, "NoMapping", 5999});
    points.push_back({1.0, 2.0, 3.0, 0.0, true, "WrongKind", 5002});
    points.push_back({1.0, 2.0, 3.0, 0.0, true, "NoPlace", 5003});
    points.push_back({1010.0, 2020.0, 310.0, 45.0, true, "Numbered", 5001});
    points.push_back({4010.0, 5020.0, 610.0, 90.0, true, "Unnumbered", 5004});
    points.push_back({1010.0, 2020.0, 310.0, 45.0, true, "Duplicate", 5005});
}

void test_respawn_make_shrines() {
    std::printf("--- respawn/make_shrines.h ---\n");
    world::ActorRegistry reg;
    content::RespawnData data;
    std::vector<spawns::RespawnPoint> points;
    populate_shrine_data(data, points);

    spawns::CapturedData empty_cap;
    aoc::protocol::ServerGuidAllocator guids(1, 1);
    spawns::Minter minter = [&]() { return guids.next(); };
    std::vector<std::string> notes;
    auto empty_res = respawn::make_shrines(reg, empty_cap, data, points, minter, 1, 10.0,
                                           [&](std::string s) { notes.push_back(std::move(s)); });
    CHECK(empty_res.made == 0 && !notes.empty(), "make_shrines notes missing template on empty CapturedData");

    std::string err;
    auto cap = spawns::CapturedData::load(err);
    CHECK(static_cast<bool>(cap), "CapturedData::load succeeds for shrine template");
    if (!cap) return;

    notes.clear();
    auto res = respawn::make_shrines(reg, *cap, data, points, minter, 1, 10.0,
                                     [&](std::string s) { notes.push_back(std::move(s)); });
    CHECK(res.made == 2, "make_shrines spawns numbered and unnumbered shrines");
    CHECK(res.no_number == 1, "make_shrines counts unnumbered shrine");
    CHECK(res.no_record == 3, "make_shrines counts missing instance_respawn, wrong kind, and missing place");
    CHECK(res.duplicates == 1, "make_shrines counts duplicate respawn definition");
    CHECK(res.refused == 0 && reg.size() == 2, "make_shrines adds 2 shrine actors to registry");
}

void test_crafting_quick_and_helpers() {
    std::printf("--- crafting/quick.h & station/cost helpers ---\n");
    crafting::QuickMake qm;
    CHECK(!qm.active && qm.station == 0 && std::fabs(crafting::kQuickDriftCm - 60.0) < 1e-6, "QuickMake defaults and drift constant");

    content::CraftStationDef d{};
    d.name = "ProcessingStation_CratePacking_Basic";
    CHECK(crafting::packs_crates(d) && crafting::takes_tag_inputs(d) && crafting::serves_quick_make(d), "CratePacking station serves quick make");

    d.name = "CraftingStation_Commodity_Market";
    CHECK(crafting::is_commodity_table(d) && crafting::takes_tag_inputs(d) && crafting::serves_quick_make(d), "Commodity station serves quick make");

    d.name = "CraftingStation_Smithing_T1";
    CHECK(!crafting::takes_tag_inputs(d) && crafting::serves_quick_make(d), "Standard CraftingStation serves quick make");

    d.name = "CraftingStation_GatheringTools";
    CHECK(!crafting::serves_quick_make(d), "GatheringTools station is excluded from quick make");

    d.name = "CraftingStation_Smithing_T1";
    d.processing = true;
    CHECK(!crafting::serves_quick_make(d), "Processing station is excluded from quick make");

    repl::StationTax tax{};
    tax.percent = 0.5f;
    CHECK(crafting::taxed_unit(23, tax) == 35, "crafting::taxed_unit rounds half-up for money");
    repl::StationTax glint_tax{};
    glint_tax.percent = 0.1f;
    CHECK(crafting::taxed_unit(85, glint_tax, items::kGlintTier) == 93, "crafting::taxed_unit floors for Glint");

    content::CraftRecipeData rdata;
    rdata.costs[50] = {items::kGlintTier, 80};
    rdata.costs[60] = {999999, 80};
    content::CraftRecipe r{};
    r.std_cost = "12.6";
    CHECK(crafting::recipe_unit_cost(rdata, r).value == 13, "recipe_unit_cost parses and rounds std_cost");
    r.std_cost.clear();
    r.cost_guid = 50;
    CHECK(crafting::recipe_unit_cost(rdata, r).tier == items::kGlintTier, "recipe_unit_cost looks up cost_guid");
    r.cost_guid = 60;
    CHECK(!crafting::recipe_unit_cost(rdata, r).ok, "recipe_unit_cost rejects unsupported currency tier");
}

void test_crafting_predicates_and_tags() {
    std::printf("--- crafting/rules.h predicates & tags ---\n");
    CHECK(crafting::csv_has("Tropics,Forest,Desert", "forest") && !crafting::csv_has("Tropics", "Snow"), "csv_has matches case-insensitively");

    std::vector<std::string> keys = crafting::tag_key({"  Wood ", "METAL", "wood"});
    CHECK(keys.size() == 2 && keys[0] == "metal" && keys[1] == "wood", "tag_key trims, lowercases, sorts, and deduplicates");
    CHECK(crafting::split_tags("a,b,c").size() == 3, "split_tags splits comma-separated tags");

    crafting::NodeFacts nf{101, 3, "Forest"};
    std::string why;
    CHECK(crafting::predicate_holds({"NodeLevel", 3, "", "", ""}, nf, why), "NodeLevel predicate passes when level >= required");
    CHECK(!crafting::predicate_holds({"NodeLevel", 4, "", "", ""}, nf, why), "NodeLevel predicate fails when level < required");
    CHECK(crafting::predicate_holds({"Biome", 0, "Desert,Forest", "", ""}, nf, why), "Biome predicate matches node biome");
    CHECK(!crafting::predicate_holds({"Biome", 0, "Tundra", "", ""}, nf, why), "Biome predicate fails on mismatch");
    CHECK(crafting::predicate_holds({"Node", 0, "", "", "101,202"}, nf, why), "Node predicate matches node guid");
    CHECK(!crafting::predicate_holds({"BuildingModification", 0, "", "", ""}, nf, why), "BuildingModification predicate refuses");
    CHECK(!crafting::predicate_holds({"MysteryPred", 0, "", "", ""}, nf, why), "Unknown predicate type refuses");
}

void test_crafting_plan_start_quick() {
    std::printf("--- crafting/rules.h plan_start (quick=true) ---\n");
    std::string err;
    std::vector<svc::InventoryItem> bag_items = {
        {svc::ItemContainer::Backpack, 0, 10, "00000000000000000000000000000001", 5, 1, 0.0f, 0},
        {svc::ItemContainer::Backpack, 1, 20, "00000000000000000000000000000002", 5, 1, 0.0f, 0},
    };
    auto inv = items::Inventory::from_items(bag_items, err);
    CHECK(static_cast<bool>(inv), "Inventory::from_items builds test bag");

    content::CraftStationDef st{};
    st.guid = 900;
    st.name = "CraftingStation_Smithing_T1";
    st.recipe_set = 500;
    st.max_tier = 4;

    content::CraftRecipeData recipes;
    recipes.set_lists[500] = {501};
    recipes.list_recipes[501] = {700};

    content::CraftRecipe rec{};
    rec.guid = 700;
    rec.name = "IronSword";
    rec.profession = 1;
    rec.tier = 1;
    rec.base_duration = 2.0;
    rec.std_cost = "10";
    rec.learnable = true;
    rec.inventory_destination = true;
    rec.inputs = {{10, 0, "", 1, 0, true, true}, {20, 0, "", 2, 0, true, false}};
    recipes.recipes[700] = rec;

    repl::StartCraft req{};
    req.recipe = 700;
    req.quantity = 1;
    req.general = {{20, {{0, 2, false}}}};

    crafting::StartFacts facts{};
    facts.recipes = &recipes;
    facts.station = &st;
    facts.inventory = &(*inv);
    facts.copper = 100;
    facts.quick = true;
    facts.tier_of = [](int64_t) { return 2; };
    facts.taught = [](int64_t) { return true; };

    crafting::Plan plan = crafting::plan_start(req, facts);
    CHECK(plan.ok && plan.cost == 10 && plan.duration_ms == 2000 && plan.takes.size() == 2, "plan_start succeeds for valid quick craft");

    req.quantity = 2;
    CHECK(!crafting::plan_start(req, facts).ok, "plan_start rejects quantity != 1 for quick craft");
    req.quantity = 1;
    facts.copper = 5;
    CHECK(!crafting::plan_start(req, facts).ok, "plan_start rejects insufficient copper");
}

void test_world_stream_detail_helpers() {
    std::printf("--- svc/world_stream_impl.h stream_detail ---\n");
    namespace sd = svc::stream_detail;
    namespace wsv1 = ::aoc::worldadmin::v1;
    namespace mstream = world::mapstream;

    CHECK(sd::leaf_of("/Game/NPCs/Goblin.Default__Goblin_C") == "Default__Goblin", "leaf_of extracts suffix after slash and dot and strips _C");
    CHECK(sd::leaf_of("PlainName") == "PlainName", "leaf_of preserves plain string");

    world::WorldActor a{};
    a.npc.archetype = "/Game/NPCs/Bandit.Default__Bandit_C";
    std::string scratch;
    CHECK(sd::display_name(a, scratch) == "Bandit", "display_name strips Default__ and _C from archetype");
    a.label = "CustomLabel";
    CHECK(sd::display_name(a, scratch) == "CustomLabel", "display_name prefers label over archetype");
    a.npc.name = "BossBandit";
    CHECK(sd::display_name(a, scratch) == "BossBandit", "display_name prefers npc.name first");

    CHECK(sd::ci_contains("Hello World", "") && sd::ci_contains("Hello World", "world"), "ci_contains matches empty and case-insensitive substring");
    CHECK(!sd::ci_contains("Hi", "longer") && !sd::ci_contains("Hello World", "mars"), "ci_contains rejects longer or missing needle");
    CHECK(sd::ci_less("apple", "Banana") && !sd::ci_less("Banana", "apple"), "ci_less orders case-insensitively");

    CHECK(std::string(sd::ai_state_name(npc::ai::State::Idle)) == "idle", "ai_state_name maps Idle");
    CHECK(std::string(sd::ai_state_name(npc::ai::State::Dead)) == "dead", "ai_state_name maps Dead");
    CHECK(std::string(sd::source_name(world::Source::Runtime)) == "runtime", "source_name maps Runtime");

    mstream::Rec rec{};
    rec.id = 42;
    rec.kind = mstream::kNpc;
    rec.health = 80;
    wsv1::Entity ent;
    sd::put_entity(&ent, rec);
    CHECK(ent.id() == 42 && ent.health_pct() == 80, "put_entity populates protobuf Entity");
}

void test_world_stream_rpc_and_overlay() {
    std::printf("--- svc/world_stream_impl.h RPC & gather_overlay ---\n");
    namespace wsv1 = ::aoc::worldadmin::v1;
    namespace mstream = world::mapstream;

    aoc::protocol::ServerGuidAllocator guids(60, 1);
    PlayerWorld world(nullptr, "no content.db", bootstrap::GameStateMode::Tier2, 60, guids);
    world::WorldActor npc{};
    npc.id = 502;
    npc.actor.guid.ObjectId = 502;
    npc.kind = world::ActorKind::Npc;
    npc.npc.name = "StreamGuard";
    npc.x = 10.0;
    npc.y = 20.0;
    world.registry().add(npc);

    svc::AdminQueue queue;
    svc::WorldStreamServerTest::Options opts;
    opts.address = "127.0.0.1:0";
    opts.token = "stream-secret";
    opts.max_subscribers = 1;

    svc::WorldStreamServerTest srv(world, []() { return std::vector<PlayerHandle>{}; }, queue, opts);
    std::vector<mstream::Overlay> ov;
    std::vector<combat::CombatWorld::Summary> scratch;
    srv.impl_->gather_overlay(ov, scratch);
    CHECK(ov.empty(), "gather_overlay returns empty when CombatWorld is null");

    std::string err;
    CHECK(srv.start(&err) && srv.port() > 0, "WorldStreamServer starts on ephemeral port");
    srv.note_loop(std::chrono::microseconds(1500));
    srv.publish();

    auto chan = grpc::CreateChannel("127.0.0.1:" + std::to_string(srv.port()), grpc::InsecureChannelCredentials());
    auto stub = wsv1::WorldStreamService::NewStub(chan);

    grpc::ClientContext unauth_ctx;
    wsv1::GetStatusResponse status_resp;
    CHECK(stub->GetStatus(&unauth_ctx, wsv1::GetStatusRequest{}, &status_resp).error_code() == grpc::StatusCode::UNAUTHENTICATED, "GetStatus rejects missing token");
    CHECK(srv.refused() == 1, "refused counter increments on unauthorized call");

    grpc::ClientContext auth_ctx;
    auth_ctx.AddMetadata("authorization", "Bearer stream-secret");
    CHECK(stub->GetStatus(&auth_ctx, wsv1::GetStatusRequest{}, &status_resp).ok() && status_resp.tick() == 1, "GetStatus returns published tick");

    grpc::ClientContext sub_ctx;
    sub_ctx.AddMetadata("authorization", "Bearer stream-secret");
    wsv1::SubscribeRequest sub_req;
    sub_req.mutable_area()->set_min_x_cm(-500);
    sub_req.mutable_area()->set_min_y_cm(-500);
    sub_req.mutable_area()->set_max_x_cm(500);
    sub_req.mutable_area()->set_max_y_cm(500);
    sub_req.set_detail(wsv1::STREAM_DETAIL_ENTITIES);
    auto reader = stub->Subscribe(&sub_ctx, sub_req);
    wsv1::SubscribeResponse sub_resp;
    CHECK(reader->Read(&sub_resp) && sub_resp.has_snapshot() && sub_resp.snapshot().entities_size() == 1, "Subscribe receives initial entity snapshot");
    sub_ctx.TryCancel();
    srv.stop();
    (void)reader->Finish();
}

} // namespace

int main() {
    test_quest_registry_points();
    test_vendor_node_tax();
    test_respawn_make_shrines();
    test_crafting_quick_and_helpers();
    test_crafting_predicates_and_tags();
    test_crafting_plan_start_quick();
    test_world_stream_detail_helpers();
    test_world_stream_rpc_and_overlay();

    std::printf("\nResult: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
