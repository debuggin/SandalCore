#include "../world/test_harness.h"

#include "dialogue/expression.h"
#include "dialogue/requirement.h"
#include "dialogue/talk.h"
#include "quest/engine.h"
#include "quest/quest_book.h"
#include "quest/quest_log.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace dlg = aoc::worldd::dialogue;
namespace qst = aoc::worldd::quest;
namespace cnt = aoc::worldd::content;
using aoc::protocol::repl::DialogueResponse;
using aoc::protocol::repl::DialogueResponseType;

struct StubExprHost final : dlg::ExprHost {
    int64_t citizen_node{100};
    std::optional<dlg::ExprValue> call(
        const std::string& name, const std::vector<dlg::ExprValue>& args) const override {
        if (name == "GetPlayerNode" && args.empty()) return dlg::ExprValue::of(citizen_node);
        if (name == "GetBuildingState" && args.empty()) {
            return dlg::ExprValue{0, "EBuildingState::Constructed"};
        }
        if (name == "IsNodeEqual" && args.size() == 2U) {
            return dlg::ExprValue::of(args[0].i == args[1].i ? 1 : 0);
        }
        return dlg::ExprValue::of(0);
    }
    int64_t random_range(int64_t lo, int64_t) const override { return lo; }
};

struct StubFacts final : dlg::Facts {
    const qst::QuestLog* log{nullptr};
    std::map<std::string, int64_t> player_vars{};
    std::map<int64_t, int64_t> currencies{};

    int64_t currency(int64_t def) const override {
        const auto it = currencies.find(def);
        return it != currencies.end() ? it->second : 0;
    }
    int64_t player_variable(const std::string& name) const override {
        const auto it = player_vars.find(name);
        return it != player_vars.end() ? it->second : 0;
    }
    std::optional<int64_t> quest_count(int64_t q, const std::string& query) const override {
        return log ? log->quest_count(q, query) : std::optional<int64_t>(0);
    }
    std::optional<int64_t> objective_count(
        int64_t q, int64_t o, const std::string& query) const override {
        return log ? log->objective_count(q, o, query) : std::optional<int64_t>(0);
    }
};

DialogueResponse make_choice(int64_t node, int32_t idx) {
    DialogueResponse r{};
    r.response_type = static_cast<uint8_t>(DialogueResponseType::Choice);
    r.node = node;
    r.choice_index = idx;
    return r;
}

int test_expression_oracle() {
    StubExprHost host{};
    const auto v1 = dlg::evaluate_expression(
        "const req = $#8574414081702566619:100$; IsNodeEqual(GetPlayerNode(), req) && 3 + 4 * 2 == 11",
        host);
    SANDAL_ASSERT(v1.has_value() && v1->i == 1);

    const auto v2 = dlg::evaluate_expression(
        "GetBuildingState() == EBuildingState::Constructed && \"tier1\" != \"tier2\" && RandIntRange(7, 9) == 7",
        host);
    SANDAL_ASSERT(v2.has_value() && v2->i == 1);

    host.citizen_node = 200;
    const auto v3 = dlg::evaluate_expression("GetPlayerNode() == 100 || !(10 >= 5)", host);
    SANDAL_ASSERT(v3.has_value() && v3->i == 0);

    const auto bad = dlg::evaluate_expression("10 / (", host);
    SANDAL_ASSERT(!bad.has_value());
    return 0;
}

int test_npc_dialogue_prerequisite_gate(const sandal::test::DataFreeConfig& cfg) {
    SANDAL_ASSERT(cfg.data_free);
    if (test_expression_oracle() != 0) return 1;

    cnt::DialogueRequirement req_citizen{};
    req_citizen.tokens = {1, 2, 0, 1};
    cnt::DialogueCriterion c_var{};
    c_var.kind = "PlayerVariable";
    c_var.op = "Equal";
    c_var.variable = "citizen_rank";
    c_var.value = 1;
    cnt::DialogueCriterion c_cur{};
    c_cur.kind = "Currency";
    c_cur.op = "GreaterThanOrEqual";
    c_cur.currency = 600;
    c_cur.value = 25;
    req_citizen.criteria = {c_var, c_cur};

    cnt::DialogueData data{};
    data.quests[777] = cnt::DialogueQuestRow{777, "Scout_Relic", false, false, req_citizen};

    qst::QuestLog log{};
    StubFacts facts{};
    facts.log = &log;
    facts.player_vars["citizen_rank"] = 1;
    facts.currencies[600] = 10;

    dlg::Book book(std::move(data));
    dlg::Evaluator ev(book, facts);
    SANDAL_ASSERT(!ev.holds(req_citizen));
    SANDAL_ASSERT(!ev.quest_available(777));

    facts.currencies[600] = 25;
    SANDAL_ASSERT(ev.holds(req_citizen));
    SANDAL_ASSERT(ev.quest_available(777));
    SANDAL_ASSERT(ev.holds(cnt::DialogueRequirement{}));
    SANDAL_ASSERT(!ev.quest_available(9999));
    return 0;
}

dlg::Book build_branch_dialogue_book() {
    cnt::DialogueCriterion c_var{};
    c_var.kind = "PlayerVariable";
    c_var.op = "Equal";
    c_var.variable = "citizen_rank";
    c_var.value = 1;
    const cnt::DialogueRequirement req_rank{{1, 1, 0}, {c_var}};

    cnt::DialogueData d{};
    d.roots[1000] = 1000;
    d.quests[777] = cnt::DialogueQuestRow{777, "Scout_Relic", false, false, {}};
    d.nodes[1000] = cnt::DialogueNodeRow{
        1000, "Root", "Response", "", 0, true, {{2000, req_rank, {}}, {3000, {}, {}}}, {}, {}};
    d.nodes[2000] = cnt::DialogueNodeRow{
        2000, "Auto", "Automatic", "", 0, false, {{2001, {}, {}}}, {}, {}};

    cnt::DialogueActionRow done_act{};
    done_act.action = "CompleteDialogueObjective";
    done_act.quest = 777;
    done_act.objective = 7771;
    d.nodes[2001] = cnt::DialogueNodeRow{
        2001, "Done", "Response", "", 0, true, {{0, {}, {}}}, {done_act}, {}};
    d.nodes[3000] = cnt::DialogueNodeRow{3000, "Offer", "AcceptQuest", "", 777, false, {}, {}, {}};

    dlg::Book book(std::move(d));
    book.set_accepts({{3000, cnt::QuestAcceptRow{3000, 777, 2000, {}}}});
    return book;
}

int test_dialogue_choice_branch_transition() {
    cnt::QuestData qdata{};
    cnt::QuestRow qrow{};
    qrow.guid = 777;
    qrow.name = "Scout_Relic";
    cnt::QuestObjectiveRow obj{};
    obj.guid = 7771;
    obj.type = "Dialogue";
    obj.count = 1;
    obj.awake = true;
    obj.completes = true;
    obj.sources = {cnt::QuestSource{"NPC", 9001, 2001, 0, 0}};
    qrow.objectives.push_back(obj);
    qdata.quests[777] = qrow;
    qst::QuestBook qbook(std::move(qdata));

    qst::QuestLog qlog{};
    StubFacts facts{};
    facts.log = &qlog;
    facts.player_vars["citizen_rank"] = 0;
    dlg::Book book = build_branch_dialogue_book();
    dlg::Evaluator ev(book, facts);
    std::vector<qst::Effect> effects{};
    qst::Engine qeng(qlog, qbook, ev, facts, effects, 100.0);

    dlg::Talk talk{};
    SANDAL_ASSERT(talk.begin(book, ev, 9001, 1000).outcome == dlg::Outcome::Changed && talk.open());
    SANDAL_ASSERT(talk.state().lines.size() == 1U && talk.state().lines[0].index == 1);
    SANDAL_ASSERT(talk.choose(book, ev, make_choice(9999, 0)).outcome == dlg::Outcome::Refused);
    SANDAL_ASSERT(talk.choose(book, ev, make_choice(1000, 0)).outcome == dlg::Outcome::Refused);

    const auto offer_res = talk.choose(book, ev, make_choice(1000, 1));
    SANDAL_ASSERT(offer_res.outcome == dlg::Outcome::Changed && offer_res.offer);
    SANDAL_ASSERT(talk.sub().has_value() && talk.sub()->quest == 777);

    SANDAL_ASSERT(qeng.start(*qbook.quest(777), 9001) != nullptr);
    const auto sub_res = talk.finish_sub(book, ev, true);
    SANDAL_ASSERT(sub_res.outcome == dlg::Outcome::Changed && talk.state().shown_node == 2001);

    const auto end_res = talk.choose(book, ev, make_choice(2001, 0));
    SANDAL_ASSERT(end_res.outcome == dlg::Outcome::Changed && !talk.open());
    for (const auto& run : end_res.actions) {
        if (run.action.action == "CompleteDialogueObjective") {
            qeng.dialogue(9001, run.visited, true);
        }
    }
    SANDAL_ASSERT(qlog.find(777) != nullptr && qlog.find(777)->status == qst::Status::Completed);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "npc_dialogue_prerequisite_gate")) {
        if (test_npc_dialogue_prerequisite_gate(cfg) != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "dialogue_choice_branch_transition")) {
        if (test_dialogue_choice_branch_transition() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
