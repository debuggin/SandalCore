// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_stats_common.h"

#include <cstring>
#include <fstream>
#include <sstream>

using namespace aoc::worldd::stats;

#ifndef STATS_CORPUS_PATH
#define STATS_CORPUS_PATH "build/gen/fixtures/corpus.txt"
#endif

namespace {

std::vector<uint8_t> base64_decode(const std::string& s) {
    static int8_t t[256];
    static bool init = false;
    if (!init) {
        std::memset(t, -1, sizeof(t));
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        init = true;
    }
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int8_t v = t[static_cast<uint8_t>(c)];
        if (v < 0) return {};
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::vector<uint8_t> hex_decode(const std::string& s) {
    std::vector<uint8_t> out;
    if (s == "-") return out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

struct CorpusParserState {
    Corpus& c;
    std::string& err;
    std::string cur_bundle;
    std::string b64;
    size_t bundle_len = 0;
    size_t line_no = 0;
    Case* cur = nullptr;
    bool skipping = false;
    DamageProfile* cur_profile = nullptr;
    MapView* cur_view = nullptr;
    GatherBlock* cur_gather = nullptr;

    bool bad(const char* m) {
        err = "line " + std::to_string(line_no) + ": " + m;
        return false;
    }
};

bool parse_gather_line(CorpusParserState& st, const std::vector<std::string>& w) {
    const std::string& k = w[0];
    GatherContext& g = st.cur_gather->ctx;
    if (k == "quality" || k == "duration") {
        if (w.size() != 2 || w[1].size() != 16) return st.bad("quality|duration F64");
        (k == "quality" ? g.quality : g.base_duration) = bits_to_double(hex64(w[1]));
        return true;
    }
    if (k == "rarity" || k == "tier") {
        if (w.size() != 2) return st.bad("rarity|tier N");
        (k == "rarity" ? g.item_rarity : g.source_tier) = std::stoi(w[1]);
        return true;
    }
    if (k == "rating") {
        if (w.size() != 3 || w[2].size() != 16) return st.bad("rating STATGUID F64");
        g.ratings[std::stoll(w[1])] = bits_to_double(hex64(w[2]));
        return true;
    }
    if (k == "q") {
        if (w.size() != 6 || w[4] != "=" || w[5].size() != 16) return st.bad("q FN NAME ROLE = F64");
        GatherQuery q;
        q.fn = std::stoi(w[1]);
        q.name = w[2] == "-" ? "" : w[2];
        q.role = std::stoi(w[3]);
        q.want = hex64(w[5]);
        st.cur_gather->queries.push_back(std::move(q));
        return true;
    }
    return st.bad("a line that is no part of a gatherhost block");
}

bool parse_case_state_scalar(CorpusParserState& st, const std::vector<std::string>& w) {
    TableState& s = st.cur->state;
    const std::string& what = w[1];
    if (what == "combat") s.combat = true;
    else if (what == "moving") s.moving_ = true;
    else if (what == "unkillable") s.unkillable_ = true;
    else if (what == "actor") s.actor = bits_to_double(hex64(w[2]));
    else if (what == "npc") s.npc = bits_to_double(hex64(w[2]));
    else if (what == "dist") {
        const int i = std::stoi(w[2]);
        if (i < 0 || i >= 8) return st.bad("state dist: there are 8 stats");
        s.dist[i] = bits_to_double(hex64(w[3]));
    } else {
        return st.bad("unknown state line");
    }
    return true;
}

bool parse_case_state_line(CorpusParserState& st, const std::vector<std::string>& w) {
    TableState& s = st.cur->state;
    const std::string& what = w[1];
    if (what == "effect") s.effects[{std::stoull(w[2]), std::stoll(w[3])}] = bits_to_double(hex64(w[4]));
    else if (what == "tag") s.tags[w[2]] = bits_to_double(hex64(w[3]));
    else if (what == "skill") s.skills.insert({std::stoull(w[2]), std::stoll(w[3]), std::stoull(w[4]), std::stoll(w[5])});
    else if (what == "prof") s.professions[w[2]] = bits_to_double(hex64(w[3]));
    else if (what == "consumed") s.consumed[{std::stoull(w[2]), std::stoll(w[3])}] = bits_to_double(hex64(w[4]));
    else return parse_case_state_scalar(st, w);
    return true;
}

bool parse_case_member_line(CorpusParserState& st, const std::vector<std::string>& w) {
    const std::string& k = w[0];
    if (k == "mod") st.cur->in.mods.push_back({std::stoll(w[1]), bits_to_double(hex64(w[2]))});
    else if (k == "tag") st.cur->in.equipped.push_back({w[1], w[2]});
    else if (k == "want") st.cur->want.push_back({std::stoll(w[1]), hex64(w[2])});
    else if (k == "digest") {
        st.cur->digest = hex64(w[1]);
        st.cur->members = std::stoul(w[2]);
        st.cur->faults = std::stoi(w[3]);
        st.cur->ignored = std::stoi(w[4]);
    } else if (k == "cap") st.cur->caps.push_back({std::stoll(w[1]), static_cast<uint32_t>(hex64(w[2])), w[3]});
    else if (k == "state") return parse_case_state_line(st, w);
    else if (k == "rounding" || k == "item") return true;
    else return st.bad("unknown case line");
    return true;
}

bool parse_damage_or_fuzz_line(CorpusParserState& st, const std::vector<std::string>& w) {
    const std::string& k = w[0];
    if (k == "damage") {
        if (w.size() != 10) return st.bad("damage NAME PROFILE FN TAG PHYS FLAGS INSTIGATOR TARGET WANT");
        DamageCall d;
        d.name = w[1];
        d.profile = w[2];
        d.fn = std::stoi(w[3]);
        d.hit.tag = w[4] == "-" ? "" : w[4];
        d.hit.physical = w[5] == "1";
        d.hit.flags = static_cast<uint8_t>(std::stoi(w[6]));
        d.instigator = w[7];
        d.target = w[8];
        d.want = hex64(w[9]);
        st.c.damages.push_back(std::move(d));
        return true;
    }
    if (k == "fuzz") {
        if (w.size() != 6) return st.bad("fuzz");
        st.c.fuzzes.push_back({w[1], std::stoull(w[2]), std::stoi(w[3]), std::stoul(w[4]), hex64(w[5])});
        return true;
    }
    if (k == "refuse") {
        if (w.size() != 5) return st.bad("refuse");
        st.c.refusals.push_back({w[1], w[2], std::stoi(w[3]), std::stoi(w[4])});
        return true;
    }
    if (k == "hostile") {
        if (w.size() != 6) return st.bad("hostile");
        Hostile h;
        h.name = w[1];
        h.fix = w[2] == "1";
        h.whole = w[2] == "2";
        h.pos = std::stoul(w[3]);
        h.del = std::stoul(w[4]);
        h.ins = hex_decode(w[5]);
        st.c.hostiles.push_back(std::move(h));
        return true;
    }
    return false;
}

bool parse_profile_or_view_line(CorpusParserState& st, const std::vector<std::string>& w) {
    const std::string& k = w[0];
    if (k == "dprofile") {
        if (w.size() != 2) return st.bad("dprofile NAME");
        st.cur_profile = &st.c.profiles[w[1]];
        return true;
    }
    if (k == "dp" && st.cur_profile) {
        if (w.size() != 3 || !set_profile_key(*st.cur_profile, w[1], std::stoll(w[2]))) return st.bad("dp KEY GUID");
        return true;
    }
    if (k == "de" && st.cur_profile) {
        if (w.size() != 6) return st.bad("de TAG D M A MIT");
        st.cur_profile->elements.push_back({w[1], std::stoll(w[2]), std::stoll(w[3]), std::stoll(w[4]), std::stoll(w[5])});
        return true;
    }
    if (k == "dview") {
        if (w.size() != 2) return st.bad("dview NAME");
        st.cur_view = &st.c.views[w[1]];
        return true;
    }
    if (k == "v" && st.cur_view) {
        if (w.size() != 3) return st.bad("v GUID F64");
        st.cur_view->v[std::stoll(w[1])] = bits_to_double(hex64(w[2]));
        return true;
    }
    return false;
}

bool parse_top_level_line(CorpusParserState& st, const std::vector<std::string>& w) {
    const std::string& k = w[0];
    if (k == "bundle") {
        if (w.size() != 3) return st.bad("bundle NAME LEN");
        st.cur_bundle = w[1];
        st.bundle_len = std::stoul(w[2]);
        st.b64.clear();
    } else if (k == "derive") {
        if (w.size() != 4) return st.bad("derive");
        st.c.derived.push_back({w[1], w[2]});
    } else if (k == "case") {
        if (w.size() != 5) return st.bad("case");
        st.c.cases.emplace_back();
        st.cur = &st.c.cases.back();
        st.cur->name = w[1];
        st.cur->bundle = w[2];
        st.cur->in.class_enum = std::stoi(w[3]);
        st.cur->in.level = std::stoi(w[4]);
    } else if (k == "end") {
        st.cur = nullptr;
        st.cur_profile = nullptr;
        st.cur_view = nullptr;
        st.cur_gather = nullptr;
    } else if (k == "dprofile" || k == "dp" || k == "de" || k == "dview" || k == "v") {
        return parse_profile_or_view_line(st, w);
    } else if (k == "damage" || k == "fuzz" || k == "refuse" || k == "hostile") {
        return parse_damage_or_fuzz_line(st, w);
    } else if (st.cur) {
        return parse_case_member_line(st, w);
    } else {
        return st.bad("a line outside a case");
    }
    return true;
}

bool parse_corpus_line(CorpusParserState& st, const std::vector<std::string>& w) {
    if (!st.cur_bundle.empty()) {
        if (w[0] == "endbundle") {
            std::vector<uint8_t> raw = base64_decode(st.b64);
            if (raw.size() != st.bundle_len) return st.bad("bundle length");
            st.c.bundles[st.cur_bundle] = std::move(raw);
            st.cur_bundle.clear();
        } else {
            st.b64 += w[0];
        }
        return true;
    }
    const std::string& k = w[0];
    if (st.skipping) {
        if (k == "end") st.skipping = false;
        return true;
    }
    if (k == "fxcase" || k == "nodehost") {
        st.skipping = true;
        return true;
    }
    if (k == "gatherhost") {
        if (w.size() != 2) return st.bad("gatherhost NAME");
        st.c.gathers.emplace_back();
        st.cur_gather = &st.c.gathers.back();
        st.cur_gather->name = w[1];
        return true;
    }
    if (st.cur_gather && k != "end") return parse_gather_line(st, w);
    return parse_top_level_line(st, w);
}

bool load_corpus(const std::string& path, Corpus& c, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    CorpusParserState st{c, err};
    std::string line;
    while (std::getline(f, line)) {
        ++st.line_no;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::vector<std::string> w;
        for (std::string x; is >> x;) w.push_back(x);
        if (!parse_corpus_line(st, w)) return false;
    }
    if (!st.cur_bundle.empty() || st.cur || st.cur_profile || st.cur_view) {
        err = "unterminated bundle or case";
        return false;
    }
    return true;
}

std::vector<uint8_t> apply_edit(const std::vector<uint8_t>& orig, const Hostile& h, bool& ok) {
    if (h.whole) {
        ok = true;
        return h.ins;
    }
    ok = h.pos <= orig.size() && h.pos + h.del <= orig.size();
    if (!ok) return {};
    std::vector<uint8_t> out(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(h.pos));
    out.insert(out.end(), h.ins.begin(), h.ins.end());
    out.insert(out.end(), orig.begin() + static_cast<std::ptrdiff_t>(h.pos + h.del), orig.end());
    if (h.fix && out.size() >= 16) {
        const uint32_t len = static_cast<uint32_t>(out.size() - 16);
        for (int i = 0; i < 4; ++i) out[8 + i] = static_cast<uint8_t>(len >> (8 * i));
        fix_crc(out);
    }
    return out;
}

void parse_corpus_bundles(const Corpus& c, std::map<std::string, StatBundle>& bundles) {
    std::printf("bundles\n");
    for (const auto& [name, raw] : c.bundles) {
        StatBundle b;
        std::string err;
        const bool ok = parse_bundle(raw, b, err);
        CHECKF(ok, "bundle %s: %s", name.c_str(), err.c_str());
        if (ok) bundles[name] = std::move(b);
    }
    for (const auto& [name, of] : c.derived) {
        auto it = c.bundles.find(of);
        CHECKF(it != c.bundles.end(), "derive %s: no bundle %s", name.c_str(), of.c_str());
        if (it == c.bundles.end()) continue;
        std::vector<uint8_t> raw = it->second;
        raw[6] = 2;
        raw[7] = 0;
        StatBundle b;
        std::string err;
        const bool ok = parse_bundle(raw, b, err);
        CHECKF(ok && b.mode == Mode::Double, "derived bundle %s: %s", name.c_str(), err.c_str());
        if (ok) bundles[name] = std::move(b);
    }
    std::printf("  %zu bundles parsed and verified\n", bundles.size());
}

void verify_case_wants_and_digest(const Case& cs, const StatResult& res, size_t& nwant) {
    bool values_ok = cs.want.empty() || cs.want.size() == res.stats.size();
    for (size_t i = 0; !cs.want.empty() && values_ok && i < res.stats.size(); ++i) {
        ++nwant;
        if (cs.want[i].guid != res.stats[i].guid || cs.want[i].value != double_bits(res.stats[i].value)) {
            values_ok = false;
            std::printf("    stat %lld is %016llx, Go has %016llx\n", static_cast<long long>(res.stats[i].guid),
                        static_cast<unsigned long long>(double_bits(res.stats[i].value)),
                        static_cast<unsigned long long>(cs.want[i].value));
        }
    }
    CHECKF(values_ok, "case %s: the values differ from the Go engine's", cs.name.c_str());
    CHECKF(digest(res) == cs.digest, "case %s: digest %016llx, Go has %016llx", cs.name.c_str(),
           static_cast<unsigned long long>(digest(res)), static_cast<unsigned long long>(cs.digest));
}

void verify_case_captures(const Case& cs, const StatResult& res, size_t& ncap) {
    if (cs.caps.empty()) return;
    std::map<int64_t, double> got;
    for (const StatOut& s : res.stats) got[s.guid] = s.value;
    size_t exact = 0, wrong = 0;
    for (const Cap& cp : cs.caps) {
        ++ncap;
        auto g = got.find(cp.guid);
        const char* m = g == got.end() ? "missing" : compare(g->second, static_cast<double>(std::bit_cast<float>(cp.bits)));
        if (cp.match == "exact" && std::strcmp(m, "exact") == 0) ++exact;
        if (cp.match != m) ++wrong;
    }
    CHECKF(wrong == 0, "case %s: %zu of %zu captured stats compare differently from the Go engine's", cs.name.c_str(), wrong, cs.caps.size());
    std::printf("  %s: %zu of %zu captured stats equal as float32\n", cs.name.c_str(), exact, cs.caps.size());
    if (cs.name == "bard_l1") CHECK(exact == 442 && cs.caps.size() == 442, "the captured Bard: 442 of 442 stats equal as float32");
}

void verify_corpus_cases(const Corpus& c, const std::map<std::string, StatBundle>& bundles) {
    std::printf("cases\n");
    size_t ncase = 0, nwant = 0, ncap = 0;
    for (const Case& cs : c.cases) {
        auto it = bundles.find(cs.bundle);
        CHECKF(it != bundles.end(), "case %s: no bundle %s", cs.name.c_str(), cs.bundle.c_str());
        if (it == bundles.end()) continue;
        StatResult res;
        std::string err;
        const bool ran = run_stats(it->second, cs.in, cs.state, res, err);
        CHECKF(ran, "case %s: %s", cs.name.c_str(), err.c_str());
        if (!ran) continue;
        ++ncase;
        CHECKF(res.stats.size() == cs.members && res.faults == cs.faults && res.ignored_mods == cs.ignored,
               "case %s: %zu stats, %d faults, %d ignored mods; the corpus has %zu, %d, %d", cs.name.c_str(), res.stats.size(),
               res.faults, res.ignored_mods, cs.members, cs.faults, cs.ignored);
        verify_case_wants_and_digest(cs, res, nwant);
        verify_case_captures(cs, res, ncap);
    }
    std::printf("  %zu cases, %zu values and %zu captured stats checked\n", ncase, nwant, ncap);
    CHECK(ncase >= 30, "the corpus has its cases");
}

void verify_corpus_refusals_and_hostiles(const Corpus& c, const std::map<std::string, StatBundle>& bundles) {
    std::printf("a valid bundle that asks too much\n");
    for (const Refusal& r : c.refusals) {
        auto it = bundles.find(r.bundle);
        CHECKF(it != bundles.end(), "refuse %s: no bundle %s", r.name.c_str(), r.bundle.c_str());
        if (it == bundles.end()) continue;
        StatInputs in;
        in.class_enum = r.class_enum;
        in.level = r.level;
        StatResult res;
        std::string e;
        CHECKF(!run_stats(it->second, in, StatState{}, res, e), "refuse %s: the run finished", r.name.c_str());
        std::printf("  %s: %s\n", r.name.c_str(), e.c_str());
    }
    CHECK(!c.refusals.empty(), "the corpus has a bundle that asks too much");

    std::printf("hostile bundles\n");
    auto tiny = c.bundles.find("tiny");
    CHECK(tiny != c.bundles.end(), "the corpus has the bundle tiny");
    size_t refused = 0;
    if (tiny != c.bundles.end()) {
        for (const Hostile& h : c.hostiles) {
            bool ok = false;
            std::vector<uint8_t> blob = apply_edit(tiny->second, h, ok);
            CHECKF(ok, "hostile %s: the edit does not fit", h.name.c_str());
            StatBundle b;
            std::string err;
            const bool accepted = ok && parse_bundle(blob, b, err);
            CHECKF(!accepted, "hostile %s was accepted", h.name.c_str());
            if (!accepted) ++refused;
        }
    }
    std::printf("  %zu of %zu hostile blobs refused\n", refused, c.hostiles.size());
    CHECK(c.hostiles.size() >= 60, "the corpus has its hostile blobs");
}

void test_corpus(const Corpus& c, std::map<std::string, StatBundle>& bundles) {
    parse_corpus_bundles(c, bundles);
    verify_corpus_cases(c, bundles);
    verify_corpus_refusals_and_hostiles(c, bundles);
}

uint64_t next_xorshift(uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

void mutate_fuzz_bundle(std::vector<uint8_t>& m, uint64_t& x) {
    for (int k = static_cast<int>(next_xorshift(x) % 4) + 1; k > 0; --k) {
        const uint64_t o = next_xorshift(x) % 5;
        const size_t at = 16 + static_cast<size_t>(next_xorshift(x) % (m.size() - 16));
        if (o <= 2) {
            const unsigned bit = static_cast<unsigned>(next_xorshift(x) % 8);
            m[at] = static_cast<uint8_t>(m[at] ^ (1u << bit));
        } else if (o == 3) {
            m[at] = static_cast<uint8_t>(next_xorshift(x));
        } else {
            m.erase(m.begin() + static_cast<std::ptrdiff_t>(at));
            const uint32_t len = static_cast<uint32_t>(m.size() - 16);
            for (int j = 0; j < 4; ++j) m[8 + j] = static_cast<uint8_t>(len >> (8 * j));
        }
    }
    if (next_xorshift(x) % 10 != 0) fix_crc(m);
}

void fuzz_parity(const std::vector<uint8_t>& base, uint64_t seed, int n, size_t& accepted, uint64_t& hash) {
    uint64_t x = seed;
    hash = 14695981039346656037ull;
    auto fold = [&](uint8_t b) {
        hash ^= b;
        hash *= 1099511628211ull;
    };
    accepted = 0;
    std::string err;
    for (int i = 0; i < n; ++i) {
        std::vector<uint8_t> m = base;
        mutate_fuzz_bundle(m, x);
        StatBundle b;
        if (!parse_bundle(m, b, err)) {
            fold(0);
            continue;
        }
        ++accepted;
        if (b.classes.empty()) {
            fold(3);
            continue;
        }
        StatInputs in;
        in.class_enum = b.classes[0].enum_value;
        in.level = 20;
        StatResult res;
        if (!run_stats(b, in, StatState{}, res, err)) {
            fold(2);
            continue;
        }
        fold(1);
        const uint64_t d = digest(res);
        for (int j = 0; j < 8; ++j) fold(static_cast<uint8_t>(d >> (8 * j)));
    }
}

void test_fuzz_parity(const Corpus& c) {
    std::printf("differential fuzz\n");
    for (const FuzzLine& f : c.fuzzes) {
        auto it = c.bundles.find(f.bundle);
        CHECKF(it != c.bundles.end(), "fuzz: no bundle %s", f.bundle.c_str());
        if (it == c.bundles.end()) continue;
        size_t acc = 0;
        uint64_t h = 0;
        fuzz_parity(it->second, f.seed, f.n, acc, h);
        CHECKF(acc == f.accepted && h == f.hash, "fuzz %s: %zu accepted with hash %016llx; Go has %zu and %016llx", f.bundle.c_str(), acc,
               static_cast<unsigned long long>(h), f.accepted, static_cast<unsigned long long>(f.hash));
        std::printf("  %s: %d damaged bundles, %zu accepted and run, the verdicts and results hash alike\n", f.bundle.c_str(), f.n, acc);
    }
    CHECK(c.fuzzes.size() >= 3, "the corpus has its differential fuzz");
}

uint64_t g_rng = 88172645463325252ull;
uint64_t rnd() { return next_xorshift(g_rng); }

void mutate_damage_bundle(std::vector<uint8_t>& m) {
    for (int k = static_cast<int>(rnd() % 4) + 1; k > 0; --k) {
        switch (rnd() % 5) {
            case 0: case 1: case 2: m[16 + rnd() % (m.size() - 16)] ^= static_cast<uint8_t>(1u << (rnd() % 8)); break;
            case 3: m[16 + rnd() % (m.size() - 16)] = static_cast<uint8_t>(rnd()); break;
            default: {
                const size_t at = 16 + rnd() % (m.size() - 16);
                m.erase(m.begin() + static_cast<std::ptrdiff_t>(at));
                const uint32_t len = static_cast<uint32_t>(m.size() - 16);
                for (int j = 0; j < 4; ++j) m[8 + j] = static_cast<uint8_t>(len >> (8 * j));
            }
        }
    }
    if (rnd() % 10) fix_crc(m);
}

bool verify_damaged_bundle_runs(const StatBundle& mb, std::string& err) {
    for (const ClassDef& cd : mb.classes) {
        for (int level : {1, 30, kMaxLevel}) {
            StatInputs in;
            in.class_enum = cd.enum_value;
            in.level = level;
            in.mods.push_back({1001, 1.0});
            StatResult res;
            if (!run_stats(mb, in, StatState{}, res, err)) {
                CHECKF(false, "a verified bundle failed to run: %s", err.c_str());
                return false;
            }
        }
    }
    return true;
}

void test_damage(const std::vector<uint8_t>& raw, const char* label) {
    std::printf("damaged bytes (%s)\n", label);
    StatBundle b;
    std::string err;
    size_t refused = 0, tried = 0;
    for (size_t n = 0; n < raw.size(); n += (n < 400 ? 1 : 97)) {
        ++tried;
        if (!parse_bundle(raw.data(), n, b, err)) ++refused;
    }
    CHECKF(refused == tried, "%zu of %zu truncations refused", refused, tried);
    size_t accepted = 0;
    for (int i = 0; i < 6000; ++i) {
        std::vector<uint8_t> m = raw;
        mutate_damage_bundle(m);
        StatBundle mb;
        if (!parse_bundle(m, mb, err)) continue;
        ++accepted;
        if (!verify_damaged_bundle_runs(mb, err)) return;
    }
    CHECK(true, "random damage never crashes the reader or the engine");
    std::printf("  %zu of 6000 damaged bundles passed the verifier and ran\n", accepted);
}

void test_inputs(const StatBundle& b) {
    std::printf("inputs\n");
    StatResult res;
    std::string err;
    StatInputs in;
    in.class_enum = b.classes[0].enum_value;
    for (int level : {0, -1, kMaxLevel + 1}) {
        in.level = level;
        CHECKF(!run_stats(b, in, StatState{}, res, err), "level %d is refused", level);
    }
    in.level = 1;
    in.class_enum = 0;
    CHECK(!run_stats(b, in, StatState{}, res, err), "class 0 is refused");
    in.class_enum = 99;
    CHECK(!run_stats(b, in, StatState{}, res, err), "class 99 is refused");
    in.class_enum = b.classes[0].enum_value;
    in.mods = {{b.stats[0].guid, 5}, {b.stats[0].guid, 2.5}, {123456789, 1}};
    CHECK(run_stats(b, in, StatState{}, res, err), "a good run");
    CHECK(res.ignored_mods == 1, "a mod for a stat the bundle does not have is ignored and counted");
    StatInputs one = in;
    one.mods = {{b.stats[0].guid, 7.5}};
    StatResult res2;
    CHECK(run_stats(b, one, StatState{}, res2, err) && digest(res) == digest(res2), "repeated mods sum");
    CHECK(tag_matches("Item.Gear.Weapon.Shield", "Item.Gear.Weapon.Shield") && tag_matches("Item.Gear.Weapon.Shield.Tower", "Item.Gear.Weapon.Shield") &&
              !tag_matches("Item.Gear.Weapon.ShieldBash", "Item.Gear.Weapon.Shield") && !tag_matches("Item.Gear", "Item.Gear.Weapon"),
          "tags match themselves and their descendants");
}

}  // namespace

int main() {
    Corpus corpus;
    std::string err;
    if (!load_corpus(STATS_CORPUS_PATH, corpus, err)) {
        std::printf("  [FAIL] corpus %s: %s\n", STATS_CORPUS_PATH, err.c_str());
        return 1;
    }
    std::map<std::string, StatBundle> bundles;
    test_corpus(corpus, bundles);
    test_damage_corpus(corpus);
    test_gather_reads(corpus);
    test_fuzz_parity(corpus);
    if (bundles.count("main")) test_damage(corpus.bundles["main"], "the real graph");
    if (bundles.count("tiny")) {
        test_damage(corpus.bundles["tiny"], "the tiny graph");
        test_inputs(bundles["tiny"]);
    }
    test_gear_rules();
    if (bundles.count("main")) test_character_stats(bundles["main"]);
    if (bundles.count("main")) test_equipment_beyond_a_float(bundles["main"], bundles.count("main_double") ? &bundles["main_double"] : nullptr);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
