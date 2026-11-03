// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package telemetry

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"maps"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/google/uuid"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

const (
	IdempotencyHeaderKey   = "Idempotency-Key"
	IdempotentReplayHeader = "Idempotent-Replayed"
	ProblemMediaType       = "application/problem+json"
	maxIdempotentBodyBytes = 1 << 20
	defaultLedgerTTL       = 24 * time.Hour
)

var ErrInvalidUUIDv7 = errors.New("idempotency key must be a valid RFC 9562 UUIDv7")

type ProblemDetails struct {
	Type, Title   string
	Status        int
	Detail        string
	Instance      string `json:"instance,omitempty"`
	CorrelationID string `json:"correlation_id,omitempty"`
}

type IdempotencyRecord struct {
	CommandID     uuid.UUID
	CommandName   string
	PayloadSHA256 [32]byte
	StatusCode    int
	ResponseBody  []byte
	Headers       map[string]string
	CachedProto   proto.Message
	ExpiresAt     time.Time
}

type IdempotencyLedger interface {
	Get(ctx context.Context, commandID uuid.UUID) (IdempotencyRecord, bool, error)
	Put(ctx context.Context, rec IdempotencyRecord) error
}

type MemoryIdempotencyLedger struct {
	mu      sync.RWMutex
	ttl     time.Duration
	entries map[uuid.UUID]IdempotencyRecord
}

func NewMemoryIdempotencyLedger(ttl time.Duration) *MemoryIdempotencyLedger {
	return &MemoryIdempotencyLedger{ttl: max(ttl, time.Millisecond), entries: make(map[uuid.UUID]IdempotencyRecord)}
}

func cloneRecord(rec IdempotencyRecord) IdempotencyRecord {
	rec.ResponseBody, rec.Headers = bytes.Clone(rec.ResponseBody), maps.Clone(rec.Headers)
	if rec.CachedProto != nil {
		rec.CachedProto = proto.Clone(rec.CachedProto)
	}
	return rec
}

func (m *MemoryIdempotencyLedger) Get(_ context.Context, id uuid.UUID) (IdempotencyRecord, bool, error) {
	m.mu.RLock()
	rec, ok := m.entries[id]
	m.mu.RUnlock()
	if !ok || (!rec.ExpiresAt.IsZero() && time.Now().After(rec.ExpiresAt)) {
		return IdempotencyRecord{}, false, nil
	}
	return cloneRecord(rec), true, nil
}

func (m *MemoryIdempotencyLedger) Put(_ context.Context, rec IdempotencyRecord) error {
	now := time.Now()
	if rec.ExpiresAt.IsZero() {
		rec.ExpiresAt = now.Add(m.ttl)
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	for k, v := range m.entries {
		if !v.ExpiresAt.IsZero() && now.After(v.ExpiresAt) {
			delete(m.entries, k)
		}
	}
	m.entries[rec.CommandID] = cloneRecord(rec)
	return nil
}

func ValidateUUIDv7(raw string) (uuid.UUID, error) {
	if id, err := uuid.Parse(strings.TrimSpace(raw)); err == nil && id.Version() == uuid.Version(7) && id.Variant() == uuid.RFC4122 {
		return id, nil
	}
	return uuid.Nil, fmt.Errorf("%w: %q", ErrInvalidUUIDv7, raw)
}

func ComputePayloadSHA256(body []byte) [32]byte { return sha256.Sum256(body) }

func writeProblemJSON(w http.ResponseWriter, p ProblemDetails) {
	w.Header().Set("Content-Type", ProblemMediaType)
	w.WriteHeader(p.Status)
	body, _ := json.Marshal(map[string]any{"type": p.Type, "title": p.Title, "status": p.Status, "detail": p.Detail, "instance": p.Instance, "correlation_id": p.CorrelationID})
	_, _ = w.Write(append(body, '\n'))
}

type bodyRecorder struct {
	http.ResponseWriter
	status int
	body   bytes.Buffer
}

func (r *bodyRecorder) WriteHeader(code int) { r.status = code; r.ResponseWriter.WriteHeader(code) }
func (r *bodyRecorder) Write(b []byte) (int, error) {
	r.status = max(r.status, http.StatusOK)
	r.body.Write(b)
	return r.ResponseWriter.Write(b)
}

func NewIdempotencyMiddleware(ledger IdempotencyLedger, strict bool) func(http.Handler) http.Handler {
	if ledger == nil {
		ledger = NewMemoryIdempotencyLedger(defaultLedgerTTL)
	}
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			rawKey := strings.TrimSpace(r.Header.Get(IdempotencyHeaderKey))
			isMutating := r.Method == http.MethodPost || r.Method == http.MethodPut || r.Method == http.MethodPatch || r.Method == http.MethodDelete
			if !isMutating || (rawKey == "" && !strict) {
				next.ServeHTTP(w, r)
				return
			}
			if cmdID, err := ValidateUUIDv7(rawKey); err != nil {
				writeProblemJSON(w, ProblemDetails{Type: "urn:sandalcore:problem:invalid-idempotency-key", Title: "Invalid Idempotency-Key", Status: http.StatusBadRequest, Detail: err.Error(), Instance: r.URL.Path})
			} else {
				handleIdempotentHTTP(w, r, next, ledger, cmdID)
			}
		})
	}
}

func handleIdempotentHTTP(w http.ResponseWriter, r *http.Request, next http.Handler, ledger IdempotencyLedger, cmdID uuid.UUID) {
	var bodyBytes []byte
	if r.Body != nil {
		var err error
		bodyBytes, err = io.ReadAll(io.LimitReader(r.Body, maxIdempotentBodyBytes))
		_ = r.Body.Close()
		if err != nil {
			writeProblemJSON(w, ProblemDetails{Type: "urn:sandalcore:problem:invalid-request-body", Title: "Invalid Request Body", Status: http.StatusBadRequest, Detail: "Failed to read request payload.", Instance: r.URL.Path})
			return
		}
		r.Body = io.NopCloser(bytes.NewReader(bodyBytes))
	}
	digest := ComputePayloadSHA256(bodyBytes)
	if existing, ok, err := ledger.Get(r.Context(), cmdID); err == nil && ok {
		if existing.PayloadSHA256 != digest {
			writeProblemJSON(w, ProblemDetails{Type: "urn:sandalcore:problem:idempotency-key-conflict", Title: "Idempotency-Key Conflict", Status: http.StatusConflict, Detail: "Idempotency-Key was already used with a different request payload.", Instance: r.URL.Path, CorrelationID: cmdID.String()})
			return
		}
		for k, v := range existing.Headers {
			w.Header().Set(k, v)
		}
		w.Header().Set(IdempotentReplayHeader, "true")
		w.WriteHeader(existing.StatusCode)
		_, _ = w.Write(existing.ResponseBody)
		return
	}
	rec := &bodyRecorder{ResponseWriter: w, status: http.StatusOK}
	next.ServeHTTP(rec, r)
	headers := make(map[string]string, len(w.Header()))
	for k, vals := range w.Header() {
		headers[k] = vals[0]
	}
	_ = ledger.Put(r.Context(), IdempotencyRecord{CommandID: cmdID, CommandName: r.Method + " " + r.URL.Path, PayloadSHA256: digest, StatusCode: rec.status, ResponseBody: rec.body.Bytes(), Headers: headers})
}

type requestIDCarrier interface{ GetRequestId() string }

func UnaryIdempotencyInterceptor(ledger IdempotencyLedger, strict bool) grpc.UnaryServerInterceptor {
	if ledger == nil {
		ledger = NewMemoryIdempotencyLedger(defaultLedgerTTL)
	}
	return func(ctx context.Context, req any, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (any, error) {
		rawKey := extractGRPCRequestID(ctx, req)
		if rawKey == "" && !strict {
			return handler(ctx, req)
		}
		cmdID, err := ValidateUUIDv7(rawKey)
		if err != nil {
			return nil, status.Errorf(codes.InvalidArgument, "invalid UUIDv7 idempotency key: %v", err)
		}
		digest := ComputePayloadSHA256(marshalGRPCRequest(info.FullMethod, req))
		if existing, ok, err := ledger.Get(ctx, cmdID); err == nil && ok {
			if existing.PayloadSHA256 != digest {
				return nil, status.Errorf(codes.AlreadyExists, "idempotency key %s conflict for %s", cmdID, info.FullMethod)
			}
			if grpc.ServerTransportStreamFromContext(ctx) != nil {
				_ = grpc.SetHeader(ctx, metadata.Pairs("idempotent-replayed", "true"))
			}
			return existing.CachedProto, nil
		}
		resp, err := handler(ctx, req)
		if err == nil {
			pm, _ := resp.(proto.Message)
			_ = ledger.Put(ctx, IdempotencyRecord{CommandID: cmdID, CommandName: info.FullMethod, PayloadSHA256: digest, StatusCode: int(codes.OK), ResponseBody: marshalGRPCRequest("", resp)[1:], CachedProto: pm})
		}
		return resp, err
	}
}

func extractGRPCRequestID(ctx context.Context, req any) string {
	if c, ok := req.(requestIDCarrier); ok && strings.TrimSpace(c.GetRequestId()) != "" {
		return strings.TrimSpace(c.GetRequestId())
	}
	if md, ok := metadata.FromIncomingContext(ctx); ok {
		for _, key := range []string{"idempotency-key", "x-request-id"} {
			if vals := md.Get(key); len(vals) > 0 && strings.TrimSpace(vals[0]) != "" {
				return strings.TrimSpace(vals[0])
			}
		}
	}
	return ""
}

func marshalGRPCRequest(fullMethod string, req any) []byte {
	if pm, ok := req.(proto.Message); ok {
		b, _ := (proto.MarshalOptions{Deterministic: true}).Marshal(pm)
		return append(append([]byte(fullMethod), 0), b...)
	}
	b, _ := json.Marshal(req)
	return append(append([]byte(fullMethod), 0), b...)
}
