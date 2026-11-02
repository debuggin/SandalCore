#include "server/world/telemetry/health_metrics_http.h"

#include <cassert>
#include <string>

int main() {
    using namespace aoc::world::telemetry;
    const auto livez = RenderLivez();
    assert(livez.status_code == 200);
    assert(livez.body.find("alive") != std::string::npos);

    HealthMetricsState st{};
    assert(RenderReadyz(st).status_code == 503);
    st.content_loaded = true;
    st.mesh_registered = true;
    assert(RenderReadyz(st).status_code == 200);
    assert(RenderHealthz(st).status_code == 200);

    const auto metrics = HandleProbePath("/metrics", st);
    assert(metrics.status_code == 200);
    assert(metrics.content_type.starts_with("application/openmetrics-text"));
    assert(metrics.body.ends_with("# EOF\n"));
    assert(HandleProbePath("/unknown", st).status_code == 404);

    std::string order{};
    const DrainHooks hooks{
        [&]() { order += '1'; }, [&]() { order += '2'; }, [&]() { order += '3'; },
        [&]() { order += '4'; }, [&]() { order += '5'; },
    };
    const auto drain = ExecuteGracefulDrain(st, hooks, 5000);
    assert(st.draining);
    assert(RenderReadyz(st).status_code == 503);
    assert(order == "12345");
    assert(drain.completed_within_budget && drain.phases_completed == 5 && drain.elapsed_ms < 5000);
    assert(!ExecuteGracefulDrain(st, hooks, 0).completed_within_budget);
    return 0;
}
