package telemetry

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"

	"aocrop/services/internal/config"
)

type TraceContext struct {
	TraceID, SpanID string
	Flags           uint8
	Valid           bool
}

type CanonicalEvent struct {
	Timestamp   string  `json:"timestamp"`
	Level       string  `json:"level"`
	Service     string  `json:"service"`
	Event       string  `json:"event"`
	TraceID     string  `json:"trace_id"`
	SpanID      string  `json:"span_id"`
	Traceparent string  `json:"traceparent"`
	DurationMs  float64 `json:"duration_ms"`
	Outcome     string  `json:"outcome"`
	Message     string  `json:"message"`
}

func isValidHex(s string, n int, nonZero bool) bool {
	if len(s) != n || strings.Trim(s, "0123456789abcdef") != "" {
		return false
	}
	return !nonZero || strings.Trim(s, "0") != ""
}
func ParseTraceparent(header string) TraceContext {
	if len(header) != 55 || header[:3] != "00-" || header[35] != '-' || header[52] != '-' {
		return TraceContext{}
	}
	tid, sid, flg := header[3:35], header[36:52], header[53:55]
	if !isValidHex(tid, 32, true) || !isValidHex(sid, 16, true) || !isValidHex(flg, 2, false) {
		return TraceContext{}
	}
	v, _ := strconv.ParseUint(flg, 16, 8)
	return TraceContext{TraceID: tid, SpanID: sid, Flags: uint8(v), Valid: true}
}
func FormatTraceparent(tc TraceContext) string {
	if !tc.Valid || !isValidHex(tc.TraceID, 32, true) || !isValidHex(tc.SpanID, 16, true) {
		return "00-00000000000000000000000000000001-0000000000000001-01"
	}
	return fmt.Sprintf("00-%s-%s-%02x", tc.TraceID, tc.SpanID, tc.Flags)
}
func FormatCanonicalJSONLine(ev CanonicalEvent) (string, error) {
	ev.Message = config.RedactDSN(ev.Message)
	if ev.Traceparent == "" {
		tc := TraceContext{TraceID: ev.TraceID, SpanID: ev.SpanID, Flags: 1, Valid: ev.TraceID != "" && ev.SpanID != ""}
		ev.Traceparent = FormatTraceparent(tc)
	}
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	err := enc.Encode(ev)
	return buf.String(), err
}
func ShouldSample(ev CanonicalEvent, slowThresholdMs float64) bool {
	return ev.Level == "ERROR" || ev.Outcome == "error" || ev.DurationMs >= slowThresholdMs
}
