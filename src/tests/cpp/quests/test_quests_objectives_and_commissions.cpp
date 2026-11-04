#include "../world/test_harness.h"

#include "dialogue/requirement.h"
#include "quest/engine.h"
#include "quest/quest_book.h"
#include "quest/quest_log.h"
#include "quest/quest_wire.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace dlg = aoc::worldd::dialogue;
namespace qst = aoc::worldd::quest;
namespace cnt = aoc::worldd::content;

struct QuestFacts final : dlg::Facts {
    const qst::QuestLog* log{nullptr};
    std::map<int64_t, int64_t> items{};

    int64_t item_count(
        const std::vector<int64_t>& records, const std::vector<std::string>&) const override {
        int64_t sum = 0;
        for (const int64_t r : records) {
            const auto it = items.find(r);
            if (it != items.end()) sum += it->second;
        }
        return sum;
    }
    std::optional<int64_t> quest_count(int64_t q, const std::string& query) const override {
        return log ? log->quest_count(q, query) : std::optional<int64_t>(0);
    }
    std::optional<int64_t> objective_count(
        int64_t q, int64_t o, const std::string& query) const override {
        return log ? log->objective_count(q, o, query) : std::optional<int64_t>(0);
    }
};

cnt::QuestRow build_multi_step_quest() {
    cnt::QuestRow q{};
    q.guid = 5000;
    q.name = "Cull_And_Scout";
    q.flavor = "Quest";

    cnt::QuestObjectiveRow kill_obj{};
    kill_obj.guid = 5001;
    kill_obj.type = "Kill";
    kill_obj.count = 5;
    kill_obj.awake = true;
    kill_obj.advanced = true;
    kill_obj.elite = 1;
    kill_obj.kill_instances = {8001};
    cnt::QuestAction act_pos{};
    act_pos.action = "ActivateObjective";
    act_pos.quest = 5000;
    act_pos.objective = 5003;
    kill_obj.on_success = {act_pos};

    cnt::QuestObjectiveRow craft_obj{};
    craft_obj.guid = 5002;
    craft_obj.type = "Collect";
    craft_obj.collect_type = "Craft";
    craft_obj.count = 3;
    craft_obj.awake = true;
    craft_obj.items = {9001};

    cnt::QuestObjectiveRow pos_obj{};
    pos_obj.guid = 5003;
    pos_obj.type = "GoTo";
    pos_obj.count = 1;
    pos_obj.awake = false;
    pos_obj.locations = {cnt::QuestLocation{1000.0, 2000.0, 50.0, 100.0, true}};

    q.objectives = {kill_obj, craft_obj, pos_obj};
    return q;
}

int test_quest_objective_counter_step() {
    cnt::QuestData qdata{};
    qdata.quests[5000] = build_multi_step_quest();
    qst::QuestBook qbook(std::move(qdata));
    qst::QuestLog qlog{};
    QuestFacts facts{};
    facts.log = &qlog;
    dlg::Book dbook(cnt::DialogueData{});
    dlg::Evaluator ev(dbook, facts);
    std::vector<qst::Effect> effects{};
    qst::Engine eng(qlog, qbook, ev, facts, effects, 1000.0);

    const auto* def = qbook.quest(5000);
    SANDAL_ASSERT(def != nullptr && eng.start(*def, 100) != nullptr);
    const auto* inst = qlog.find(5000);
    SANDAL_ASSERT(inst != nullptr && inst->objectives[0].active && !inst->objectives[2].active);

    eng.kill(8002, 2);
    eng.kill(8001, 0);
    SANDAL_ASSERT(inst->objectives[0].count == 0);

    for (int i = 0; i < 5; ++i) eng.kill(8001, 1);
    SANDAL_ASSERT(inst->objectives[0].completed && inst->objectives[2].active);

    eng.collected("Purchase", 9001, 2);
    SANDAL_ASSERT(inst->objectives[1].count == 0);
    eng.collected("Craft", 9001, 5);
    SANDAL_ASSERT(inst->objectives[1].count == 3 && inst->objectives[1].completed);

    eng.position(1500.0, 2000.0, 50.0);
    SANDAL_ASSERT(inst->status == qst::Status::Active);
    eng.position(1050.0, 2000.0, 50.0);
    SANDAL_ASSERT(inst->status == qst::Status::Completed);
    return 0;
}

int test_commission_board_reward_payout(const sandal::test::DataFreeConfig& cfg) {
    SANDAL_ASSERT(cfg.data_free);
    SANDAL_ASSERT(qst::giver_flavor("Commission") == 1U);
    SANDAL_ASSERT(qst::giver_flavor("Quest") == 0U);

    cnt::QuestRow comm{};
    comm.guid = 6000;
    comm.name = "Board_Supply_Order";
    comm.flavor = "Commission";
    comm.rewards = {7001};

    cnt::QuestObjectiveRow deposit_obj{};
    deposit_obj.guid = 6001;
    deposit_obj.type = "TurnIn";
    deposit_obj.count = 3;
    deposit_obj.awake = true;
    deposit_obj.completes = true;
    deposit_obj.crate = true;
    deposit_obj.crate_items = {9100};
    deposit_obj.rewards = {7002};
    comm.objectives.push_back(deposit_obj);

    cnt::QuestData qdata{};
    qdata.quests[6000] = comm;
    qst::QuestBook qbook(std::move(qdata));
    qst::QuestLog qlog{};
    QuestFacts facts{};
    facts.log = &qlog;
    dlg::Book dbook(cnt::DialogueData{});
    dlg::Evaluator ev(dbook, facts);
    std::vector<qst::Effect> effects{};
    qst::Engine eng(qlog, qbook, ev, facts, effects, 2000.0);

    SANDAL_ASSERT(eng.start(*qbook.quest(6000), 200) != nullptr);
    effects.clear();
    SANDAL_ASSERT(eng.deposit(9999, 0, 2) == 0);
    SANDAL_ASSERT(eng.deposit(9100, 0, 2) == 2);
    SANDAL_ASSERT(qlog.find(6000)->status == qst::Status::Active);

    SANDAL_ASSERT(eng.deposit(9100, 0, 2) == 1);
    SANDAL_ASSERT(qlog.find(6000)->status == qst::Status::Completed);

    const auto reward_it = std::find_if(effects.begin(), effects.end(), [](const qst::Effect& e) {
        return e.kind == qst::Effect::Kind::GrantRewards;
    });
    SANDAL_ASSERT(reward_it != effects.end() && reward_it->tables.size() == 2U);
    SANDAL_ASSERT(reward_it->tables[0] == 7001 && reward_it->tables[1] == 7002);

    SANDAL_ASSERT(qlog.finish(6000) && qlog.find(6000) == nullptr && qlog.completions(6000) == 1);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "quest_objective_counter_step")) {
        if (test_quest_objective_counter_step() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "commission_board_reward_payout")) {
        if (test_commission_board_reward_payout(cfg) != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
