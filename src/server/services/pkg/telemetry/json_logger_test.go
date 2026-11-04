package telemetry

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestParseTraceparentValidAndRoundTrip(t *testing.T) {
	const raw01 = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"
	tc := ParseTraceparent(raw01)
	if !tc.Valid {
		t.Fatalf("expected valid TraceContext for %q", raw01)
	}
	if tc.TraceID != "4bf92f3577b34da6a3ce929d0e0e4736" {
		t.Fatalf("unexpected TraceID: %q", tc.TraceID)
	}
	if tc.SpanID != "00f067aa0ba902b7" {
		t.Fatalf("unexpected SpanID: %q", tc.SpanID)
	}
	if tc.Flags != 0x01 {
		t.Fatalf("expected Flags 0x01, got 0x%02x", tc.Flags)
	}
	if got := FormatTraceparent(tc); got != raw01 {
		t.Fatalf("FormatTraceparent round-trip mismatch: got %q, want %q", got, raw01)
	}

	const raw00 = "00-0000000000000000000000000000000a-000000000000000b-00"
	tc00 := ParseTraceparent(raw00)
	if !tc00.Valid || tc00.Flags != 0x00 {
		t.Fatalf("expected valid unsampled TraceContext, got %+v", tc00)
	}
	if got := FormatTraceparent(tc00); got != raw00 {
		t.Fatalf("FormatTraceparent unsampled mismatch: got %q, want %q", got, raw00)
	}
}

func TestParseTraceparentRejectsMalformedInputs(t *testing.T) {
	cases := []struct {
		name   string
		header string
	}{
		{"empty", ""},
		{"short", "00-4bf92f3577b34da6-00f067aa-01"},
		{"long", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-extra"},
		{"bad_version", "01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"},
		{"missing_first_sep", "00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"},
		{"missing_second_sep", "00-4bf92f3577b34da6a3ce929d0e0e4736_00f067aa0ba902b7-01"},
		{"missing_third_sep", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7_01"},
		{"uppercase_trace_id", "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01"},
		{"non_hex_trace_id", "00-gbf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"},
		{"non_hex_span_id", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902bz-01"},
		{"non_hex_flags", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0g"},
		{"all_zero_trace_id", "00-00000000000000000000000000000000-00f067aa0ba902b7-01"},
		{"all_zero_span_id", "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01"},
	}
	for _, tc := range cases {
		if got := ParseTraceparent(tc.header); got.Valid {
			t.Errorf("%s: expected invalid TraceContext for %q, got %+v", tc.name, tc.header, got)
		}
	}
}

func TestFormatTraceparentFallbackOnInvalidContext(t *testing.T) {
	const fallback = "00-00000000000000000000000000000001-0000000000000001-01"
	invalids := []TraceContext{
		{},
		{TraceID: "4bf92f3577b34da6a3ce929d0e0e4736", SpanID: "00f067aa0ba902b7", Valid: false},
		{TraceID: "00000000000000000000000000000000", SpanID: "00f067aa0ba902b7", Valid: true},
		{TraceID: "4bf92f3577b34da6a3ce929d0e0e4736", SpanID: "0000000000000000", Valid: true},
	}
	for i, tc := range invalids {
		if got := FormatTraceparent(tc); got != fallback {
			t.Errorf("case %d: got %q, want %q", i, got, fallback)
		}
	}
}

func TestFormatCanonicalJSONLineAndRedaction(t *testing.T) {
	ev := CanonicalEvent{
		Timestamp:  "2026-11-03T16:00:00Z",
		Level:      "INFO",
		Service:    "quests",
		Event:      "quest.load",
		TraceID:    "4bf92f3577b34da6a3ce929d0e0e4736",
		SpanID:     "00f067aa0ba902b7",
		DurationMs: 12.5,
		Outcome:    "ok",
		Message:    "connected to postgres://admin:topsecret@127.0.0.1:5432/aoc?sslmode=disable",
	}
	line, err := FormatCanonicalJSONLine(ev)
	if err != nil {
		t.Fatalf("FormatCanonicalJSONLine error: %v", err)
	}
	if !strings.HasSuffix(line, "\n") {
		t.Fatalf("expected trailing newline in %q", line)
	}
	if strings.Contains(line, "topsecret") {
		t.Fatalf("expected DSN password redaction, got %s", line)
	}
	var decoded CanonicalEvent
	if err := json.Unmarshal([]byte(line), &decoded); err != nil {
		t.Fatalf("json.Unmarshal failed: %v", err)
	}
	if decoded.Traceparent != "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01" {
		t.Fatalf("unexpected synthesized Traceparent: %q", decoded.Traceparent)
	}
	if decoded.Timestamp != ev.Timestamp || decoded.Service != "quests" || decoded.DurationMs != 12.5 {
		t.Fatalf("decoded event mismatch: %+v", decoded)
	}
}

func TestFormatCanonicalJSONLinePreservesExplicitTraceparent(t *testing.T) {
	const customTP = "00-11111111111111111111111111111111-2222222222222222-00"
	line, err := FormatCanonicalJSONLine(CanonicalEvent{
		Level:       "ERROR",
		Traceparent: customTP,
		Outcome:     "error",
	})
	if err != nil {
		t.Fatalf("FormatCanonicalJSONLine error: %v", err)
	}
	var decoded CanonicalEvent
	if err := json.Unmarshal([]byte(line), &decoded); err != nil {
		t.Fatalf("json.Unmarshal error: %v", err)
	}
	if decoded.Traceparent != customTP {
		t.Fatalf("expected preserved Traceparent %q, got %q", customTP, decoded.Traceparent)
	}
}

func TestShouldSampleDecisionMatrix(t *testing.T) {
	const threshold = 50.0
	if !ShouldSample(CanonicalEvent{Level: "ERROR", Outcome: "ok", DurationMs: 1.0}, threshold) {
		t.Fatal("expected ERROR level to be sampled")
	}
	if !ShouldSample(CanonicalEvent{Level: "INFO", Outcome: "error", DurationMs: 1.0}, threshold) {
		t.Fatal("expected error outcome to be sampled")
	}
	if !ShouldSample(CanonicalEvent{Level: "INFO", Outcome: "ok", DurationMs: 50.0}, threshold) {
		t.Fatal("expected latency at exact threshold (50.0ms) to be sampled")
	}
	if !ShouldSample(CanonicalEvent{Level: "INFO", Outcome: "ok", DurationMs: 75.0}, threshold) {
		t.Fatal("expected latency above threshold to be sampled")
	}
	if ShouldSample(CanonicalEvent{Level: "INFO", Outcome: "ok", DurationMs: 49.9}, threshold) {
		t.Fatal("expected fast healthy event below threshold not to be sampled")
	}
}
