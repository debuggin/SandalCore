// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "ability_rig.h"

#include <cmath>
#include <cstring>
#include <set>

inline int g_pass = 0;
inline int g_fail = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      g_pass++;                                                                \
    } else {                                                                   \
      std::printf("  [FAIL] %s\n", msg);                                       \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)
#define CHECKF(cond, ...)                                                      \
  do {                                                                         \
    if (cond) {                                                                \
      g_pass++;                                                                \
    } else {                                                                   \
      std::printf("  [FAIL] ");                                                \
      std::printf(__VA_ARGS__);                                                \
      std::printf("\n");                                                       \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

constexpr double kManaMult25 = 5.6875;

inline const std::vector<std::string> kTalents = {
    "Bard_Passive_Merriment", "Bard_Passive_Dance_Power",
    "Bard_Passive_QuickWit_Jest", "Bard_QuickWit_Jest"};

inline bool near(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

template <class P>
const ability_rig::Seen *find_event(const std::vector<ability_rig::Seen> &seen,
                                    P pred, size_t from = 0) {
  for (size_t i = from; i < seen.size(); ++i) {
    if (pred(seen[i]))
      return &seen[i];
  }
  return nullptr;
}

inline const ability_rig::bootstrap::EffectItem *
effect_added(const ability_rig::Seen &s, int64_t guid) {
  for (const auto &e : s.change.effects_added)
    if (e.effect_guid == guid)
      return &e;
  return nullptr;
}

inline const ability_rig::bootstrap::EffectItem *
effect_changed(const ability_rig::Seen &s, int64_t guid) {
  for (const auto &e : s.change.effects_changed)
    if (e.effect_guid == guid)
      return &e;
  return nullptr;
}

inline bool has_stat(const ability_rig::Seen &s, int64_t record) {
  for (const auto &st : s.change.stats)
    if (st.record == record)
      return true;
  return false;
}

inline float stat_of(const ability_rig::Seen &s, int64_t record) {
  for (const auto &st : s.change.stats)
    if (st.record == record)
      return st.value;
  return -1;
}

namespace abilities = ability_rig::abilities;
namespace bootstrap = ability_rig::bootstrap;
namespace content = ability_rig::content;
namespace stats = ability_rig::stats;
using ability_rig::AbilityPipeline;
using ability_rig::kMoveSpeedMult;
using ability_rig::kResonanceStat;
using ability_rig::Refusal;
using ability_rig::refusal_name;
using ability_rig::Rig;
using ability_rig::Seen;

void run_ability_pipeline_costs_tests();
