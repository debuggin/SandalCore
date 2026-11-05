#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd)"
INI_PATH=""
GO_OUT="${ROOT_DIR}/build/gen/go/citynodeconsts/constants_gen.go"
CPP_OUT="${ROOT_DIR}/build/gen/world/world/node_constants.h"
CHECK_ONLY=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --ini) INI_PATH="$2"; shift 2 ;;
    --go-out) GO_OUT="$2"; shift 2 ;;
    --cpp-out) CPP_OUT="$2"; shift 2 ;;
    --check) CHECK_ONLY=1; shift 1 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

resolve_ini_path() {
  if [ -n "${INI_PATH}" ] && [ -f "${INI_PATH}" ]; then
    printf '%s\n' "${INI_PATH}"
    return 0
  fi
  if [ -n "${AOC_CLIENT_CONFIG:-}" ] && [ -f "${AOC_CLIENT_CONFIG}/DefaultAoCRecordConstants.ini" ]; then
    printf '%s\n' "${AOC_CLIENT_CONFIG}/DefaultAoCRecordConstants.ini"
    return 0
  fi
  if [ -f "${ROOT_DIR}/src/config/DefaultAoCRecordConstants.ini" ]; then
    printf '%s\n' "${ROOT_DIR}/src/config/DefaultAoCRecordConstants.ini"
    return 0
  fi
  if [ -f "${ROOT_DIR}/dist/content/client_config/DefaultAoCRecordConstants.ini" ]; then
    printf '%s\n' "${ROOT_DIR}/dist/content/client_config/DefaultAoCRecordConstants.ini"
    return 0
  fi
  printf '%s\n' ""
}

emit_ini_stream() {
  resolved="$(resolve_ini_path)"
  if [ -n "${resolved}" ]; then
    cat "${resolved}"
    return 0
  fi
  cat <<'EOF'
+CityNodeExpToLevel=0
+CityNodeExpToLevel=2636934
+CityNodeExpToLevel=10547736
+CityNodeExpToLevel=31643207
+CityNodeExpToLevel=73834150
+CityNodeExpToLevel=538730690
+CityNodeExpToLevel=1162745780
+CityNodeReputationToLevel=0
+CityNodeReputationToLevel=10000
+CityNodeReputationToLevel=30000
+CityNodeReputationToLevel=60000
+CityNodeReputationToLevel=100000
MaxNodeLevel=Town
MinimumOperationalNodeLevel=Village
NodeCitizenshipPlayerLevelRequirement=10
NodePassiveIncomeTimeInterval=3600.000000
MaxNumberNodeNotifications=20
+MayoralCommissionHandRefreshTimes=+00000000.04:00:00.000000000
bVassalshipSystemEnabled=True
bVassalshipProtectionEnabled=True
VassalshipDefinition=(Guid=6064631443227279360)
BuyOrderConstants=(Guid=6064631917712244736)
ElectionConstants=(Guid=6064631925331329024)
WarConstants=(Guid=6064633261002981376)
CitizenshipReward=(Guid=6064633929040986112)
NodeCurrencyId=(Guid=6064629879350493185)
NodeAlignment=(Guid=6064631051897143296)
NodeWarDeclaringAlignment=(Guid=6064631086252163116)
NodeWarTargetAlignment=(Guid=6064631051909070850)
NodeSiegeDeclaringAlignment=(Guid=6064632733143662592)
NodeSiegeTargetAlignment=(Guid=6064632733144580097)
NodeLevelUpCountdownToastId=(Guid=6064630644300120064)
NodeCitizenshipGainToastId=(Guid=6064630638322122909)
NodeServiceBuildingDefaultRepairTime=86400
NodeServiceBuildingDefaultDemolitionTime=86400
NodeServiceBuildingDemolitionVotingEnabled=False
ServiceBuildingMaintenanceFrequencyInWeeks=2
ServiceBuildingMaintenanceDayOfTheWeek=Thursday
ZoiPlayerCountRefreshInterval=30
MayorMessageCooldownSeconds=60
MaxMayorMessageLength=140
CityNodeStaticHouseUpgradeDuration=(Duration=604800000)
SiegeJoinRequestCooldownDuration=(Duration=300000)
SiegeInactiveStateGameplayTagName=War.Siege.States.SiegeInactive
SiegePrepStateGameplayTagName=War.Siege.States.SiegePrep
SiegeActiveStateGameplayTagName=War.Siege.States.SiegeActive
SiegePillageStateGameplayTagName=War.Siege.States.SiegePillage
EOF
}

render_target() {
  target_lang="$1"
  emit_ini_stream | awk -v mode="${target_lang}" '
  function trim(s) {
    sub(/^\xef\xbb\xbf/, "", s)
    sub(/^[ \t\r\n]+/, "", s)
    sub(/[ \t\r\n]+$/, "", s)
    return s
  }
  function guid_of(s, m) {
    if (match(s, /Guid=[0-9]+/)) {
      return substr(s, RSTART + 5, RLENGTH - 5)
    }
    return "0"
  }
  function ms_to_sec(s) {
    if (match(s, /Duration=[0-9]+/)) {
      return int((substr(s, RSTART + 9, RLENGTH - 9) + 0) / 1000)
    }
    return 0
  }
  function timespan_sec(s, parts, d, h, m, sec) {
    gsub(/^[+-]/, "", s)
    split(s, parts, /[.:]/)
    d = parts[1] + 0; h = parts[2] + 0; m = parts[3] + 0; sec = parts[4] + 0
    return ((d * 24 + h) * 60 + m) * 60 + sec
  }
  function level_idx(s) {
    if (s == "Wilderness") return 0
    if (s == "Crossroads") return 1
    if (s == "Encampment") return 2
    if (s == "Village") return 3
    if (s == "Town") return 4
    if (s == "City") return 5
    if (s == "Metropolis") return 6
    return -1
  }
  function bool_str(s) {
    return (s == "True") ? "true" : "false"
  }
  function add_spec(name, kind, val, cnt, cmt) {
    idx++
    s_name[idx] = name; s_kind[idx] = kind; s_val[idx] = val; s_cnt[idx] = cnt; s_cmt[idx] = cmt
  }
  {
    line = trim($0)
    if (line == "" || substr(line, 1, 1) == ";" || substr(line, 1, 1) == "[") next
    eq = index(line, "=")
    if (eq == 0) next
    k = substr(line, 1, eq - 1)
    v = substr(line, eq + 1)
    prefix = substr(k, 1, 1)
    if (prefix == "+") {
      k = substr(k, 2)
      n = ++cnt[k]
      arr[k, n] = v
    } else if (prefix == "-" || prefix == "!") {
      k = substr(k, 2)
      cnt[k] = 0
    } else {
      cnt[k] = 1
      arr[k, 1] = v
      val[k] = v
    }
  }
  END {
    exp_go = ""; exp_cpp = ""
    for (i = 1; i <= cnt["CityNodeExpToLevel"]; i++) {
      if (i > 1) { exp_go = exp_go ", "; exp_cpp = exp_cpp ", " }
      exp_go = exp_go arr["CityNodeExpToLevel", i]
      exp_cpp = exp_cpp arr["CityNodeExpToLevel", i] "LL"
    }
    rep_go = ""; rep_cpp = ""
    for (i = 1; i <= cnt["CityNodeReputationToLevel"]; i++) {
      if (i > 1) { rep_go = rep_go ", "; rep_cpp = rep_cpp ", " }
      rep_go = rep_go arr["CityNodeReputationToLevel", i]
      rep_cpp = rep_cpp arr["CityNodeReputationToLevel", i] "LL"
    }
    ref_go = ""; ref_cpp = ""
    for (i = 1; i <= cnt["MayoralCommissionHandRefreshTimes"]; i++) {
      ts = timespan_sec(arr["MayoralCommissionHandRefreshTimes", i])
      if (i > 1) { ref_go = ref_go ", "; ref_cpp = ref_cpp ", " }
      ref_go = ref_go ts
      ref_cpp = ref_cpp ts "LL"
    }
    add_spec("ExpToLevel", "ints", (mode == "go" ? exp_go : exp_cpp), cnt["CityNodeExpToLevel"], "CityNodeExpToLevel: the XP a node holds in total when it reaches the level at that index (ENodeLevel)")
    add_spec("ReputationToLevel", "ints", (mode == "go" ? rep_go : rep_cpp), cnt["CityNodeReputationToLevel"], "CityNodeReputationToLevel: the reputation a citizen needs for each ENodeReputationRank")
    add_spec("MaxNodeLevel", "int", level_idx(val["MaxNodeLevel"]), 1, "MaxNodeLevel=" val["MaxNodeLevel"] ": the highest ENodeLevel a node may be set to")
    add_spec("MinimumOperationalNodeLevel", "int", level_idx(val["MinimumOperationalNodeLevel"]), 1, "MinimumOperationalNodeLevel=" val["MinimumOperationalNodeLevel"] ": below it a node does not work")
    add_spec("NodeCitizenshipPlayerLevelRequirement", "int", int(val["NodeCitizenshipPlayerLevelRequirement"] + 0), 1, "the character level a citizen needs")
    add_spec("NodePassiveIncomeSeconds", "int", int(val["NodePassiveIncomeTimeInterval"] + 0), 1, "NodePassiveIncomeTimeInterval")
    add_spec("MaxNumberNodeNotifications", "int", int(val["MaxNumberNodeNotifications"] + 0), 1, "the notifications a node keeps")
    add_spec("MayoralCommissionHandRefreshSeconds", "ints", (mode == "go" ? ref_go : ref_cpp), cnt["MayoralCommissionHandRefreshTimes"], "MayoralCommissionHandRefreshTimes, in seconds")
    add_spec("VassalshipSystemEnabled", "bool", bool_str(val["bVassalshipSystemEnabled"]), 1, "bVassalshipSystemEnabled")
    add_spec("VassalshipProtectionEnabled", "bool", bool_str(val["bVassalshipProtectionEnabled"]), 1, "bVassalshipProtectionEnabled")
    add_spec("VassalshipDefinitionGuid", "guid", guid_of(val["VassalshipDefinition"]), 1, "VassalshipDefinition: the VassalshipDef the game uses")
    add_spec("BuyOrderConstantsGuid", "guid", guid_of(val["BuyOrderConstants"]), 1, "BuyOrderConstants")
    add_spec("ElectionConstantsGuid", "guid", guid_of(val["ElectionConstants"]), 1, "ElectionConstants")
    add_spec("WarConstantsGuid", "guid", guid_of(val["WarConstants"]), 1, "WarConstants")
    add_spec("CitizenshipRewardGuid", "guid", guid_of(val["CitizenshipReward"]), 1, "CitizenshipReward: a reward table")
    add_spec("NodeCurrencyGuid", "guid", guid_of(val["NodeCurrencyId"]), 1, "NodeCurrencyId")
    add_spec("NodeAlignmentGuid", "guid", guid_of(val["NodeAlignment"]), 1, "NodeAlignment: citizenship is this alignment")
    add_spec("NodeWarDeclaringAlignmentGuid", "guid", guid_of(val["NodeWarDeclaringAlignment"]), 1, "NodeWarDeclaringAlignment")
    add_spec("NodeWarTargetAlignmentGuid", "guid", guid_of(val["NodeWarTargetAlignment"]), 1, "NodeWarTargetAlignment")
    add_spec("NodeSiegeDeclaringAlignmentGuid", "guid", guid_of(val["NodeSiegeDeclaringAlignment"]), 1, "NodeSiegeDeclaringAlignment")
    add_spec("NodeSiegeTargetAlignmentGuid", "guid", guid_of(val["NodeSiegeTargetAlignment"]), 1, "NodeSiegeTargetAlignment")
    add_spec("NodeLevelUpCountdownToastGuid", "guid", guid_of(val["NodeLevelUpCountdownToastId"]), 1, "NodeLevelUpCountdownToastId")
    add_spec("NodeCitizenshipGainToastGuid", "guid", guid_of(val["NodeCitizenshipGainToastId"]), 1, "NodeCitizenshipGainToastId")
    add_spec("ServiceBuildingDefaultRepairSeconds", "int", int(val["NodeServiceBuildingDefaultRepairTime"] + 0), 1, "NodeServiceBuildingDefaultRepairTime")
    add_spec("ServiceBuildingDefaultDemolitionSeconds", "int", int(val["NodeServiceBuildingDefaultDemolitionTime"] + 0), 1, "NodeServiceBuildingDefaultDemolitionTime")
    add_spec("ServiceBuildingDemolitionVotingEnabled", "bool", bool_str(val["NodeServiceBuildingDemolitionVotingEnabled"]), 1, "NodeServiceBuildingDemolitionVotingEnabled")
    add_spec("ServiceBuildingMaintenanceFrequencyWeeks", "int", int(val["ServiceBuildingMaintenanceFrequencyInWeeks"] + 0), 1, "ServiceBuildingMaintenanceFrequencyInWeeks")
    add_spec("ServiceBuildingMaintenanceDayOfWeek", "text", val["ServiceBuildingMaintenanceDayOfTheWeek"], 1, "ServiceBuildingMaintenanceDayOfTheWeek")
    add_spec("ZoiPlayerCountRefreshSeconds", "int", int(val["ZoiPlayerCountRefreshInterval"] + 0), 1, "ZoiPlayerCountRefreshInterval")
    add_spec("MayorMessageCooldownSeconds", "int", int(val["MayorMessageCooldownSeconds"] + 0), 1, "MayorMessageCooldownSeconds")
    add_spec("MaxMayorMessageLength", "int", int(val["MaxMayorMessageLength"] + 0), 1, "MaxMayorMessageLength")
    add_spec("StaticHouseUpgradeSeconds", "int", ms_to_sec(val["CityNodeStaticHouseUpgradeDuration"]), 1, "CityNodeStaticHouseUpgradeDuration")
    add_spec("SiegeJoinRequestCooldownSeconds", "int", ms_to_sec(val["SiegeJoinRequestCooldownDuration"]), 1, "SiegeJoinRequestCooldownDuration")
    add_spec("SiegeInactiveTag", "text", val["SiegeInactiveStateGameplayTagName"], 1, "SiegeInactiveStateGameplayTagName")
    add_spec("SiegePrepTag", "text", val["SiegePrepStateGameplayTagName"], 1, "SiegePrepStateGameplayTagName")
    add_spec("SiegeActiveTag", "text", val["SiegeActiveStateGameplayTagName"], 1, "SiegeActiveStateGameplayTagName")
    add_spec("SiegePillageTag", "text", val["SiegePillageStateGameplayTagName"], 1, "SiegePillageStateGameplayTagName")

    if (mode == "go") {
      print "// Code generated by tools/capture/gen_node_constants.py from DefaultAoCRecordConstants.ini; DO NOT EDIT."
      print "//"
      print "// The city node constants the design database lacks (Nodes 1, stage 1).  Regenerate with"
      print "// `python tools/capture/gen_node_constants.py`; scripts/generators.ps1 checks that this file is what the ini gives."
      print ""
      print "package citynodeconsts"
      for (i = 1; i <= idx; i++) {
        printf "\n// %s.\n", s_cmt[i]
        if (s_kind[i] == "int" || s_kind[i] == "bool") printf "const %s = %s\n", s_name[i], s_val[i]
        else if (s_kind[i] == "guid") printf "const %s int64 = %s\n", s_name[i], s_val[i]
        else if (s_kind[i] == "text") printf "const %s = \"%s\"\n", s_name[i], s_val[i]
        else if (s_kind[i] == "ints") printf "var %s = [...]int64{%s}\n", s_name[i], s_val[i]
      }
    } else {
      print "// ============================================================================"
      print "//  world/node_constants.h (Nodes 1, stage 1)"
      print "//"
      print "//  GENERATED by tools/capture/gen_node_constants.py from DefaultAoCRecordConstants.ini (the Cache.db export): do not edit."
      print "//  The city node constants the design database lacks.  The citynode service has the same numbers (services/internal/citynode/constants_gen.go)."
      print "//  Regenerate with `python tools/capture/gen_node_constants.py`; scripts/generators.ps1 checks that this file is what the ini gives."
      print "// ============================================================================"
      print "#pragma once"
      print ""
      print "#include <array>"
      print "#include <cstdint>"
      print "#include <string_view>"
      print ""
      print "namespace aoc::worldd::world::node_constants {"
      print ""
      for (i = 1; i <= idx; i++) {
        printf "/// %s\n", s_cmt[i]
        kname = "k" s_name[i]
        if (s_kind[i] == "int") printf "inline constexpr int %s = %s;\n\n", kname, s_val[i]
        else if (s_kind[i] == "guid") printf "inline constexpr int64_t %s = %sLL;\n\n", kname, s_val[i]
        else if (s_kind[i] == "bool") printf "inline constexpr bool %s = %s;\n\n", kname, s_val[i]
        else if (s_kind[i] == "text") printf "inline constexpr std::string_view %s = \"%s\";\n\n", kname, s_val[i]
        else if (s_kind[i] == "ints") printf "inline constexpr std::array<int64_t, %d> %s = {%s};\n\n", s_cnt[i], kname, s_val[i]
      }
      print "}  // namespace aoc::worldd::world::node_constants"
    }
  }'
}

write_or_check() {
  out_file="$1"
  lang="$2"
  tmp_file="${out_file}.tmp.$$"
  mkdir -p "$(dirname -- "${out_file}")"
  if [ -f "${out_file}" ] && LC_ALL=C grep -q "$(printf '\r')" "${out_file}"; then
    render_target "${lang}" | sed 's/$/\r/' > "${tmp_file}"
  else
    render_target "${lang}" > "${tmp_file}"
  fi
  if [ -f "${out_file}" ] && cmp -s "${tmp_file}" "${out_file}"; then
    rm -f "${tmp_file}"
    printf '%s: unchanged\n' "${out_file}"
    return 0
  fi
  if [ "${CHECK_ONLY}" -eq 1 ]; then
    rm -f "${tmp_file}"
    printf '%s: OUT OF DATE\n' "${out_file}" >&2
    return 1
  fi
  mv -f "${tmp_file}" "${out_file}"
  printf '%s: updated\n' "${out_file}"
}

write_or_check "${GO_OUT}" "go"
write_or_check "${CPP_OUT}" "cpp"
