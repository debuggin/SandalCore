// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package telemetry

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"io"
	"net/http"
	"slices"
	"strings"
	"sync"
)

const ETagRehydratedHeader = "X-ETag-Rehydrated"

func ComputeStrongETag(body []byte) string {
	sum := sha256.Sum256(body)
	return fmt.Sprintf(`"sha256-%x"`, sum[:8])
}

type etagBufferRecorder struct {
	header http.Header
	status int
	body   bytes.Buffer
}

func newETagBufferRecorder() *etagBufferRecorder {
	return &etagBufferRecorder{header: make(http.Header), status: http.StatusOK}
}

func (r *etagBufferRecorder) Header() http.Header         { return r.header }
func (r *etagBufferRecorder) WriteHeader(code int)        { r.status = code }
func (r *etagBufferRecorder) Write(b []byte) (int, error) { return r.body.Write(b) }

type etagState struct {
	mu    sync.RWMutex
	paths map[string]string
}

func (s *etagState) get(path string) string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.paths[path]
}

func (s *etagState) set(path, etag string) {
	s.mu.Lock()
	s.paths[path] = etag
	s.mu.Unlock()
}

func ETagMiddleware(next http.Handler) http.Handler {
	state := &etagState{paths: make(map[string]string)}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet || r.Method == http.MethodHead {
			serveConditionalRead(w, r, next, state)
			return
		}
		serveConditionalMutation(w, r, next, state)
	})
}

func serveConditionalRead(w http.ResponseWriter, r *http.Request, next http.Handler, state *etagState) {
	rec := newETagBufferRecorder()
	next.ServeHTTP(rec, r)
	bodyBytes := rec.body.Bytes()
	etag := strings.TrimSpace(rec.header.Get("ETag"))
	if rec.status == http.StatusOK && etag == "" {
		etag = ComputeStrongETag(bodyBytes)
		rec.header.Set("ETag", etag)
	}
	if etag != "" {
		state.set(r.URL.Path, etag)
	}
	if ifMatch := strings.TrimSpace(r.Header.Get("If-Match")); ifMatch != "" && !matchIfMatch(ifMatch, etag) {
		writePreconditionFailed(w, r, ifMatch)
		return
	}
	if inm := strings.TrimSpace(r.Header.Get("If-None-Match")); inm != "" && rec.status == http.StatusOK && matchIfNoneMatch(inm, etag) {
		writeNotModified(w, rec.header)
		return
	}
	flushRecordedResponse(w, rec, r.Method == http.MethodHead)
}

func serveConditionalMutation(w http.ResponseWriter, r *http.Request, next http.Handler, state *etagState) {
	ifMatch := strings.TrimSpace(r.Header.Get("If-Match"))
	if ifMatch == "" {
		next.ServeHTTP(w, r)
		return
	}
	known := state.get(r.URL.Path)
	if known != "" && !matchIfMatch(ifMatch, known) {
		writePreconditionFailed(w, r, ifMatch)
		return
	}
	rec := newETagBufferRecorder()
	next.ServeHTTP(rec, r)
	etag := strings.TrimSpace(rec.header.Get("ETag"))
	if etag == "" {
		etag = ComputeStrongETag(rec.body.Bytes())
		rec.header.Set("ETag", etag)
	}
	if known == "" && !matchIfMatch(ifMatch, etag) {
		writePreconditionFailed(w, r, ifMatch)
		return
	}
	state.set(r.URL.Path, etag)
	flushRecordedResponse(w, rec, false)
}

func writePreconditionFailed(w http.ResponseWriter, r *http.Request, ifMatch string) {
	writeProblemJSON(w, ProblemDetails{
		Type:     "urn:sandalcore:problem:precondition-failed",
		Title:    "Precondition Failed",
		Status:   http.StatusPreconditionFailed,
		Detail:   fmt.Sprintf("If-Match precondition %s did not match the current representation ETag.", ifMatch),
		Instance: r.URL.Path,
	})
}

func writeNotModified(w http.ResponseWriter, src http.Header) {
	for _, k := range []string{"ETag", "Cache-Control", "Alt-Svc", "Content-Location", "Date", "Expires", "Vary"} {
		if vals := src.Values(k); len(vals) > 0 {
			w.Header()[k] = slices.Clone(vals)
		}
	}
	w.Header().Del("Content-Type")
	w.Header().Del("Content-Length")
	w.WriteHeader(http.StatusNotModified)
}

func flushRecordedResponse(w http.ResponseWriter, rec *etagBufferRecorder, headOnly bool) {
	for k, vals := range rec.header {
		w.Header()[k] = slices.Clone(vals)
	}
	w.WriteHeader(rec.status)
	if !headOnly && rec.body.Len() > 0 {
		_, _ = w.Write(rec.body.Bytes())
	}
}

func matchIfNoneMatch(headerVal, currentETag string) bool {
	if currentETag == "" {
		return false
	}
	if strings.TrimSpace(headerVal) == "*" {
		return true
	}
	target := stripWeakPrefix(currentETag)
	for _, raw := range strings.Split(headerVal, ",") {
		if stripWeakPrefix(strings.TrimSpace(raw)) == target {
			return true
		}
	}
	return false
}

func matchIfMatch(headerVal, currentETag string) bool {
	if currentETag == "" || isWeakETag(currentETag) {
		return false
	}
	if strings.TrimSpace(headerVal) == "*" {
		return true
	}
	for _, raw := range strings.Split(headerVal, ",") {
		cand := strings.TrimSpace(raw)
		if !isWeakETag(cand) && cand == currentETag {
			return true
		}
	}
	return false
}

func isWeakETag(tag string) bool {
	return strings.HasPrefix(tag, "W/") || strings.HasPrefix(tag, "w/")
}

func stripWeakPrefix(tag string) string {
	if isWeakETag(tag) {
		return strings.TrimSpace(tag[2:])
	}
	return tag
}

type cachedETagEntry struct {
	etag   string
	body   []byte
	header http.Header
}

type ETagRoundTripper struct {
	Base  http.RoundTripper
	mu    sync.RWMutex
	cache map[string]cachedETagEntry
}

func NewETagRoundTripper(base http.RoundTripper) *ETagRoundTripper {
	if base == nil {
		base = http.DefaultTransport
	}
	return &ETagRoundTripper{Base: base, cache: make(map[string]cachedETagEntry)}
}

func NewETagCachingClient(base *http.Client) *http.Client {
	if base == nil {
		base = &http.Client{Timeout: defaultRWTimeout}
	}
	cloned := *base
	cloned.Transport = NewETagRoundTripper(base.Transport)
	return &cloned
}

func (t *ETagRoundTripper) RoundTrip(req *http.Request) (*http.Response, error) {
	rt := t.Base
	if rt == nil {
		rt = http.DefaultTransport
	}
	if req.Method != http.MethodGet {
		return rt.RoundTrip(req)
	}
	key := req.URL.String()
	cached, hasCached := t.loadEntry(key)
	outReq := req
	if hasCached && cached.etag != "" && req.Header.Get("If-None-Match") == "" {
		outReq = req.Clone(req.Context())
		outReq.Header.Set("If-None-Match", cached.etag)
	}
	resp, err := rt.RoundTrip(outReq)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode == http.StatusNotModified && hasCached {
		return rehydrateNotModified(resp, cached), nil
	}
	if resp.StatusCode == http.StatusOK && resp.Body != nil {
		return t.captureOKResponse(key, resp)
	}
	return resp, nil
}

func (t *ETagRoundTripper) loadEntry(key string) (cachedETagEntry, bool) {
	t.mu.RLock()
	defer t.mu.RUnlock()
	entry, ok := t.cache[key]
	return entry, ok
}

func (t *ETagRoundTripper) captureOKResponse(key string, resp *http.Response) (*http.Response, error) {
	etag := strings.TrimSpace(resp.Header.Get("ETag"))
	if etag == "" {
		return resp, nil
	}
	bodyBytes, err := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	if err != nil {
		return nil, err
	}
	t.mu.Lock()
	t.cache[key] = cachedETagEntry{etag: etag, body: bytes.Clone(bodyBytes), header: resp.Header.Clone()}
	t.mu.Unlock()
	resp.Body = io.NopCloser(bytes.NewReader(bodyBytes))
	resp.ContentLength = int64(len(bodyBytes))
	return resp, nil
}

func rehydrateNotModified(resp *http.Response, cached cachedETagEntry) *http.Response {
	if resp.Body != nil {
		_ = resp.Body.Close()
	}
	merged := cached.header.Clone()
	for k, vals := range resp.Header {
		merged[k] = slices.Clone(vals)
	}
	merged.Set(ETagRehydratedHeader, "true")
	bodyClone := bytes.Clone(cached.body)
	resp.StatusCode = http.StatusOK
	resp.Status = fmt.Sprintf("%d %s", http.StatusOK, http.StatusText(http.StatusOK))
	resp.Header = merged
	resp.ContentLength = int64(len(bodyClone))
	resp.Body = io.NopCloser(bytes.NewReader(bodyClone))
	return resp
}
