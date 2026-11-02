// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "nav_fixture.h"
#include "test_worldd_gm_travel_common.h"

#include "aoc/worldadmin/v1/worldadmin.grpc.pb.h"

namespace adminv1 = ::aoc::worldadmin::v1;

static const std::string kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct Admin {
    std::unique_ptr<svc::AdminQueue> queue = std::make_unique<svc::AdminQueue>();
    std::unique_ptr<GmTravel> travel;
    std::unique_ptr<svc::WorldAdminServer> server;
    std::unique_ptr<adminv1::WorldAdminService::Stub> stub;
    std::thread::id net_thread = std::this_thread::get_id();
    std::atomic<bool> directory_off_thread{false};

    explicit Admin(Harness &h, std::chrono::milliseconds timeout = std::chrono::milliseconds(4000)) {
        travel = std::make_unique<GmTravel>(
            *h.fx.world,
            [this, &h] {
                if (std::this_thread::get_id() != net_thread) {
                    directory_off_thread = true;
                }
                return std::vector<PlayerHandle>{PlayerHandle{h.s.get(), &h.conn}};
            },
            [&h] { return h.t; });
        svc::WorldAdminServer::Options o;
        o.address = "127.0.0.1:0";
        o.token = kToken;
        o.call_timeout = timeout;
        server = std::make_unique<svc::WorldAdminServer>(*travel, *queue, o);
        std::string error;
        if (!server->start(&error)) {
            std::printf("  [FAIL] the admin server does not start: %s\n", error.c_str());
            std::exit(2);
        }
        stub = adminv1::WorldAdminService::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(server->port()), grpc::InsecureChannelCredentials()));
    }

    template <class F> grpc::Status via_net_thread(F &&call) {
        std::atomic<bool> done{false};
        grpc::Status status;
        std::thread t([&] {
            status = call();
            done = true;
        });
        while (!done) {
            queue->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        t.join();
        return status;
    }

    static void authorize(grpc::ClientContext &ctx, const std::string &token) { ctx.AddMetadata("authorization", "Bearer " + token); }
};

static void test_admin_api_auth_refusals(Harness &h, Admin &a, const GoodPosition &start) {
    grpc::ClientContext ctx;
    adminv1::TeleportRequest rq;
    rq.set_character("cyb");
    rq.mutable_point()->set_x(5000);
    rq.mutable_point()->set_y(3000);
    rq.mutable_point()->set_z(1500);
    rq.mutable_point()->set_has_z(true);
    adminv1::TeleportResponse rs;
    const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
    CHECK(s.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a call with no token is refused (Unauthenticated)");
    grpc::ClientContext wrong;
    Admin::authorize(wrong, std::string(64, 'f'));
    const grpc::Status s2 = a.via_net_thread([&] { return a.stub->Teleport(&wrong, rq, &rs); });
    CHECK(s2.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a call with a wrong token is refused");
    grpc::ClientContext bare;
    bare.AddMetadata("authorization", "Bearer ");
    const grpc::Status s3 = a.via_net_thread([&] { return a.stub->Teleport(&bare, rq, &rs); });
    CHECK(s3.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a bare 'Bearer ' is refused");
    grpc::ClientContext raw;
    raw.AddMetadata("authorization", kToken);
    const grpc::Status s4 = a.via_net_thread([&] { return a.stub->Teleport(&raw, rq, &rs); });
    CHECK(s4.error_code() == grpc::StatusCode::UNAUTHENTICATED, "the token without 'Bearer ' is refused");
    CHECK(a.server->refused() == 4 && near_to(h.s->movement().good().x, start.x) && h.s->gm_teleports() == 0, "four refusals counted, the player did not move");
    CHECK(s.error_message().find(kToken) == std::string::npos && s2.error_message().find(kToken) == std::string::npos, "(the token is not in the answer)");
}

static void test_admin_api_list_and_teleport(Harness &h, Admin &a) {
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::ListPlayersResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->ListPlayers(&ctx, adminv1::ListPlayersRequest(), &rs); });
        CHECK(s.ok() && rs.players_size() == 1 && rs.players(0).name() == "cyb" && rs.players(0).character_id() == kCyb && !rs.players(0).mounted(), "ListPlayers: name, guid, position, mounted");
        CHECK(!a.directory_off_thread, "the session list was read on the network thread, not the gRPC thread");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::ListPlacesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->ListPlaces(&ctx, adminv1::ListPlacesRequest(), &rs); });
        CHECK(s.ok() && static_cast<size_t>(rs.places_size()) == a.travel->list_places().size() && rs.places(0).name() == "login", "ListPlaces: the login spot and the nodes");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.set_place("joeva");
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
        const world::CityNodeInfo *joeva = g_nodes->by_name("Joeva");
        CHECK(s.ok() && rs.place() == "Verra_RVR_Joeva" && joeva && near_to(rs.to().x(), joeva->x, 0.01) && near_to(h.s->movement().good().x, joeva->x, 0.01), "Teleport to a place: applied");
        CHECK(!a.directory_off_thread, "(and applied on the network thread)");
    }
}

static void test_admin_api_errors_and_speed(Harness &h, Admin &a) {
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.mutable_point()->set_x(100000);
        rq.mutable_point()->set_y(100000);
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT && s.error_message().find("give z") != std::string::npos, "a point with no ground and no z: InvalidArgument, asking for z");
        grpc::ClientContext ctx2;
        Admin::authorize(ctx2, kToken);
        adminv1::TeleportRequest none;
        none.set_character("cyb");
        const grpc::Status s2 = a.via_net_thread([&] { return a.stub->Teleport(&ctx2, none, &rs); });
        CHECK(s2.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "no target: InvalidArgument");
        grpc::ClientContext ctx3;
        Admin::authorize(ctx3, kToken);
        none.set_character("nobody");
        none.set_place("joeva");
        const grpc::Status s3 = a.via_net_thread([&] { return a.stub->Teleport(&ctx3, none, &rs); });
        CHECK(s3.error_code() == grpc::StatusCode::NOT_FOUND, "an unknown character: NotFound");
        grpc::ClientContext ctx4;
        Admin::authorize(ctx4, kToken);
        none.set_character("cyb");
        none.set_place("Narnia");
        const grpc::Status s4 = a.via_net_thread([&] { return a.stub->Teleport(&ctx4, none, &rs); });
        CHECK(s4.error_code() == grpc::StatusCode::NOT_FOUND, "an unknown place: NotFound");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetMoveSpeedRequest rq;
        rq.set_character("cyb");
        rq.set_multiplier(4);
        adminv1::SetMoveSpeedResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetMoveSpeed(&ctx, rq, &rs); });
        CHECK(s.ok() && rs.multiplier() == 4 && rs.previous() == 1 && h.s->gm_speed() == 4.0, "SetMoveSpeed: applied");
        grpc::ClientContext ctx2;
        Admin::authorize(ctx2, kToken);
        rq.set_multiplier(11);
        const grpc::Status s2 = a.via_net_thread([&] { return a.stub->SetMoveSpeed(&ctx2, rq, &rs); });
        CHECK(s2.error_code() == grpc::StatusCode::INVALID_ARGUMENT && h.s->gm_speed() == 4.0, "a multiplier of 11: InvalidArgument, nothing changed");
    }
    CHECK(!a.directory_off_thread, "(no call touched the sessions off the network thread)");
}

static void test_admin_api() {
    std::printf("the admin API: the service token, the queue, the network thread\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    Admin a(h);
    const GoodPosition start = h.s->movement().good();
    test_admin_api_auth_refusals(h, a, start);
    test_admin_api_list_and_teleport(h, a);
    test_admin_api_errors_and_speed(h, a);
}

static void test_admin_queue() {
    std::printf("the admin queue: a call the network thread does not take is not applied\n");
    {
        Harness h(cyb_on_the_ground(), on_the_ground);
        h.play();
        Admin a(h, std::chrono::milliseconds(200));
        const GoodPosition start = h.s->movement().good();
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.mutable_point()->set_x(5000);
        rq.mutable_point()->set_y(3000);
        rq.mutable_point()->set_z(1500);
        rq.mutable_point()->set_has_z(true);
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.stub->Teleport(&ctx, rq, &rs);
        CHECK(s.error_code() == grpc::StatusCode::UNAVAILABLE, "a call the network thread does not take in time ends with Unavailable");
        CHECK(a.queue->abandoned() == 1, "(it is counted as given up)");
        CHECK(a.queue->drain() == 0 && near_to(h.s->movement().good().x, start.x) && h.s->gm_teleports() == 0, "and it is never applied, even when the network thread comes round later");
        a.server->stop();
        CHECK(!a.queue->run([] {}, std::chrono::milliseconds(50)), "(a stopped server's queue takes nothing)");
    }
    {
        svc::AdminQueue q;
        int ran = 0;
        std::atomic<bool> in_task{false};
        std::thread t([&] {
            q.run(
                [&] {
                    in_task = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    ++ran;
                },
                std::chrono::milliseconds(100));
        });
        while (!in_task) {
            q.drain();
        }
        t.join();
        CHECK(ran == 1, "a task the network thread has started is waited for, whatever the timeout");
        q.run([] {}, std::chrono::milliseconds(1));
        std::thread t2([&] { q.run([&] { throw std::runtime_error("boom"); }, std::chrono::milliseconds(2000)); });
        size_t drained = 0;
        for (int i = 0; i < 2000 && drained == 0; ++i) {
            drained = q.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        t2.join();
        CHECK(drained == 1, "a task that throws is answered, not left to hang its caller");
    }
}

int main() {
    if (!init_gm_travel_globals()) return 2;
    test_admin_api();
    test_admin_queue();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
