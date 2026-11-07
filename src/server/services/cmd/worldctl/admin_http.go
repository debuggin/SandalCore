// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"strconv"
	"strings"
	"text/tabwriter"
	"time"
)

func defaultMetricsURL() string {
	for _, k := range []string{"AOC_WORLD_METRICS_URL", "AOC_WORLD_PROM_URL", "WORLD_METRICS_URL"} {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			return strings.TrimRight(v, "/")
		}
	}
	return "http://127.0.0.1:18081"
}

func parseMetricsCmdFlags(args []string) (string, []string, error) {
	metricsURL := defaultMetricsURL()
	var pos []string
	for i := 0; i < len(args); i++ {
		a := strings.TrimSpace(args[i])
		if a == "--metrics-url" || a == "-metrics-url" {
			if i+1 >= len(args) || strings.TrimSpace(args[i+1]) == "" {
				return "", nil, usageError("--metrics-url needs a URL such as http://127.0.0.1:18081")
			}
			i++
			metricsURL = strings.TrimRight(strings.TrimSpace(args[i]), "/")
		} else if strings.HasPrefix(a, "--metrics-url=") || strings.HasPrefix(a, "-metrics-url=") {
			metricsURL = strings.TrimRight(strings.TrimSpace(a[strings.IndexByte(a, '=')+1:]), "/")
		} else if a == "--addr" || a == "-addr" || a == "--token-file" || a == "-token-file" {
			i++
		} else if strings.HasPrefix(a, "-") {
			return "", nil, usageError(fmt.Sprintf("unknown flag %q", a))
		} else {
			pos = append(pos, a)
		}
	}
	return metricsURL, pos, nil
}

func fetchWorkerAdminJSON(ctx context.Context, url string, dst any) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	cli := &http.Client{Timeout: 5 * time.Second}
	resp, err := cli.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("HTTP %d from %s", resp.StatusCode, url)
	}
	return json.NewDecoder(resp.Body).Decode(dst)
}

func runTickRate(ctx context.Context, args []string, out, errw io.Writer) int {
	metricsURL, pos, err := parseMetricsCmdFlags(args)
	if err != nil || len(pos) > 1 {
		fmt.Fprintf(errw, "worldctl tick-rate: usage: worldctl tick-rate [<hz>] [--metrics-url http://127.0.0.1:18081]\n")
		return 2
	}
	var payload struct {
		ServerID       string  `json:"server_id"`
		TickHz         float64 `json:"tick_hz"`
		PreviousTickHz float64 `json:"previous_tick_hz"`
	}
	if len(pos) == 0 {
		if err := fetchWorkerAdminJSON(ctx, metricsURL+"/healthz", &payload); err != nil {
			fmt.Fprintf(errw, "worldctl tick-rate: %v\n", err)
			return 1
		}
		srv := payload.ServerID
		if srv == "" {
			srv = "world"
		}
		fmt.Fprintf(out, "%s: tick_hz=%g Hz\n", srv, payload.TickHz)
		return 0
	}
	hz, perr := strconv.Atoi(strings.TrimSpace(pos[0]))
	if perr != nil || hz < 1 || hz > 120 {
		fmt.Fprintf(errw, "worldctl tick-rate: %q is not a whole number of Hz from 1 to 120\n", pos[0])
		return 2
	}
	if err := fetchWorkerAdminJSON(ctx, fmt.Sprintf("%s/admin/tick_hz?tick_hz=%d", metricsURL, hz), &payload); err != nil {
		fmt.Fprintf(errw, "worldctl tick-rate: %v\n", err)
		return 1
	}
	srv := payload.ServerID
	if srv == "" {
		srv = "world"
	}
	fmt.Fprintf(out, "%s: tick_hz=%g Hz (was %g Hz)\n", srv, payload.TickHz, payload.PreviousTickHz)
	return 0
}

func runSimulateNpcs(ctx context.Context, args []string, out, errw io.Writer) int {
	metricsURL, pos, err := parseMetricsCmdFlags(args)
	if err != nil || len(pos) > 1 {
		fmt.Fprintf(errw, "worldctl simulate-npcs: usage: worldctl simulate-npcs [on|off] [--metrics-url http://127.0.0.1:18081]\n")
		return 2
	}
	endpoint := metricsURL + "/healthz"
	if len(pos) == 1 {
		switch strings.ToLower(strings.TrimSpace(pos[0])) {
		case "on", "true", "1", "yes":
			endpoint = metricsURL + "/admin/simulate_all_npcs?simulate_all_npcs=1"
		case "off", "false", "0", "no":
			endpoint = metricsURL + "/admin/simulate_all_npcs?simulate_all_npcs=0"
		default:
			fmt.Fprintf(errw, "worldctl simulate-npcs: %q must be on or off\n", pos[0])
			return 2
		}
	}
	var payload struct {
		ServerID        string `json:"server_id"`
		SimulateAllNpcs bool   `json:"simulate_all_npcs"`
	}
	if err := fetchWorkerAdminJSON(ctx, endpoint, &payload); err != nil {
		fmt.Fprintf(errw, "worldctl simulate-npcs: %v\n", err)
		return 1
	}
	srv, state := payload.ServerID, "off"
	if srv == "" {
		srv = "world"
	}
	if payload.SimulateAllNpcs {
		state = "on"
	}
	fmt.Fprintf(out, "%s: simulate_all_npcs=%s (%t)\n", srv, state, payload.SimulateAllNpcs)
	return 0
}

func parseTimeOfDayArg(raw string) (float64, bool) {
	s := strings.ToLower(strings.TrimSpace(raw))
	switch s {
	case "dawn", "sunrise", "morning":
		return 6.0, true
	case "noon", "midday":
		return 12.0, true
	case "dusk", "sunset", "evening":
		return 18.0, true
	case "night", "midnight":
		return 0.0, true
	}
	if strings.Contains(s, ":") {
		parts := strings.Split(s, ":")
		if len(parts) < 2 || len(parts) > 3 {
			return 0, false
		}
		hh, errH := strconv.Atoi(parts[0])
		mm, errM := strconv.Atoi(parts[1])
		ss := 0
		if len(parts) == 3 {
			var errS error
			ss, errS = strconv.Atoi(parts[2])
			if errS != nil || ss < 0 || ss >= 60 {
				return 0, false
			}
		}
		if errH != nil || errM != nil || hh < 0 || hh > 24 || mm < 0 || mm >= 60 || (hh == 24 && (mm > 0 || ss > 0)) {
			return 0, false
		}
		return math.Mod(float64(hh)+float64(mm)/60.0+float64(ss)/3600.0, 24.0), true
	}
	v, err := parseNumber(s)
	if err != nil || v < 0.0 || v > 24.0 {
		return 0, false
	}
	return math.Mod(v, 24.0), true
}

func formatClockHHMMSS(hours float64) string {
	h := math.Mod(math.Mod(hours, 24.0)+24.0, 24.0)
	totalSec := int(math.Round(h*3600.0)) % 86400
	return fmt.Sprintf("%02d:%02d:%02d", totalSec/3600, (totalSec%3600)/60, totalSec%60)
}

func runTimeOfDay(ctx context.Context, args []string, out, errw io.Writer) int {
	metricsURL, pos, err := parseMetricsCmdFlags(args)
	if err != nil || len(pos) > 1 {
		fmt.Fprintf(errw, "worldctl time: usage: worldctl time [<HH:MM|hours|dawn|noon|dusk|night>] [--metrics-url http://127.0.0.1:18081]\n")
		return 2
	}
	var payload struct {
		ServerID        string  `json:"server_id"`
		TimeOfDayHours  float64 `json:"time_of_day_hours"`
		PreviousHours   float64 `json:"previous_time_of_day_hours"`
		TimeOfDayLabel  string  `json:"time_of_day_label"`
	}
	if len(pos) == 0 {
		if err := fetchWorkerAdminJSON(ctx, metricsURL+"/healthz", &payload); err != nil {
			if err2 := fetchWorkerAdminJSON(ctx, metricsURL+"/admin/time_of_day", &payload); err2 != nil {
				fmt.Fprintf(errw, "worldctl time: %v\n", err)
				return 1
			}
		}
		srv := payload.ServerID
		if srv == "" {
			srv = "world"
		}
		fmt.Fprintf(out, "%s: time_of_day=%s (%.2f h, 9x game rate)\n", srv, formatClockHHMMSS(payload.TimeOfDayHours), payload.TimeOfDayHours)
		return 0
	}
	hours, ok := parseTimeOfDayArg(pos[0])
	if !ok {
		fmt.Fprintf(errw, "worldctl time: %q is not a valid time (0..24 hours, HH:MM, or dawn/noon/dusk/night)\n", pos[0])
		return 2
	}
	if err := fetchWorkerAdminJSON(ctx, fmt.Sprintf("%s/admin/time_of_day?hours=%.4f", metricsURL, hours), &payload); err != nil {
		fmt.Fprintf(errw, "worldctl time: %v\n", err)
		return 1
	}
	srv := payload.ServerID
	if srv == "" {
		srv = "world"
	}
	fmt.Fprintf(out, "%s: time_of_day=%s (%.2f h, was %.2f h, 9x game rate)\n", srv, formatClockHHMMSS(payload.TimeOfDayHours), payload.TimeOfDayHours, payload.PreviousHours)
	return 0
}

func runWorkers(ctx context.Context, args []string, out, errw io.Writer) int {
	metricsURL, pos, err := parseMetricsCmdFlags(args)
	if err != nil || len(pos) != 0 {
		fmt.Fprintf(errw, "worldctl workers: usage: worldctl workers [--metrics-url http://127.0.0.1:18081]\n")
		return 2
	}
	var gwPayload struct {
		TimeOfDayHours float64 `json:"time_of_day_hours"`
		Workers        []struct {
			WorkerID  string  `json:"worker_id"`
			ScopeKind string  `json:"scope_kind"`
			ScopeKey  string  `json:"scope_key"`
			TickHz    float64 `json:"tick_hz"`
			CPUPct    float64 `json:"cpu_pct"`
		} `json:"workers"`
	}
	tw := tabwriter.NewWriter(out, 0, 0, 2, ' ', 0)
	fmt.Fprintln(tw, "WORKER_ID\tSCOPE_KIND\tSCOPE_KEY\tTICK_HZ\tCPU_PCT\tTIME_OF_DAY")
	if err := fetchWorkerAdminJSON(ctx, metricsURL+"/api/v1/world/workers", &gwPayload); err == nil && len(gwPayload.Workers) > 0 {
		tod := formatClockHHMMSS(gwPayload.TimeOfDayHours)
		for _, w := range gwPayload.Workers {
			fmt.Fprintf(tw, "%s\t%s\t%s\t%g Hz\t%.1f%%\t%s\n", w.WorkerID, w.ScopeKind, w.ScopeKey, w.TickHz, w.CPUPct, tod)
		}
		_ = tw.Flush()
		return 0
	}
	var hzPayload struct {
		ServerID         string  `json:"server_id"`
		PartitionKind    string  `json:"partition_kind"`
		PartitionID      string  `json:"partition_id"`
		TickHz           float64 `json:"tick_hz"`
		CPUSaturationPct float64 `json:"cpu_saturation_pct"`
		TimeOfDayHours   float64 `json:"time_of_day_hours"`
	}
	if err := fetchWorkerAdminJSON(ctx, metricsURL+"/healthz", &hzPayload); err != nil {
		fmt.Fprintf(errw, "worldctl workers: %v\n", err)
		return 1
	}
	srv, kind, key := hzPayload.ServerID, hzPayload.PartitionKind, hzPayload.PartitionID
	if srv == "" {
		srv = "world"
	}
	if kind == "" {
		kind = "region"
	}
	if key == "" {
		key = srv
	}
	fmt.Fprintf(tw, "%s\t%s\t%s\t%g Hz\t%.1f%%\t%s\n", srv, kind, key, hzPayload.TickHz, hzPayload.CPUSaturationPct, formatClockHHMMSS(hzPayload.TimeOfDayHours))
	_ = tw.Flush()
	return 0
}
