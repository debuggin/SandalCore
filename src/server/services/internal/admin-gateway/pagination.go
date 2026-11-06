// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"aocrop/services/internal/telemetry"
)

type cacheEntry struct {
	val       any
	expiresAt time.Time
}

type ttlCache struct {
	mu    sync.RWMutex
	items map[string]cacheEntry
}

func newTTLCache() *ttlCache {
	return &ttlCache{items: make(map[string]cacheEntry)}
}

func (c *ttlCache) get(key string, now time.Time) (any, bool) {
	c.mu.RLock()
	e, ok := c.items[key]
	c.mu.RUnlock()
	if !ok || !now.Before(e.expiresAt) {
		return nil, false
	}
	return e.val, true
}

func (c *ttlCache) set(key string, val any, ttl time.Duration, now time.Time) {
	c.mu.Lock()
	c.items[key] = cacheEntry{val: val, expiresAt: now.Add(ttl)}
	c.mu.Unlock()
}

func (c *ttlCache) invalidatePrefix(prefix string) {
	c.mu.Lock()
	for k := range c.items {
		if strings.HasPrefix(k, prefix) {
			delete(c.items, k)
		}
	}
	c.mu.Unlock()
}

type pageParams struct {
	Query    string
	Sort     string
	Fields   []string
	Limit    int
	Offset   int
	Page     int
	PageSize int
}

func parsePageParams(w http.ResponseWriter, r *http.Request, defLimit, maxLimit int) (pageParams, bool) {
	q := r.URL.Query()
	queryStr := firstNonEmpty(q.Get("q"), q.Get("search"))
	if utf8.RuneCountInString(queryStr) > 100 {
		writeProblem(w, r, http.StatusBadRequest, "invalid-query", "Query parameter 'q' exceeds 100 characters.")
		return pageParams{}, false
	}
	limit, offset, ok := parseLimitOffsetAndPage(w, r, q, defLimit, maxLimit)
	if !ok {
		return pageParams{}, false
	}
	sortSpec := strings.TrimSpace(q.Get("sort"))
	if sortSpec == "" {
		if sb := strings.TrimSpace(q.Get("sort_by")); sb != "" {
			if strings.EqualFold(strings.TrimSpace(q.Get("sort_order")), "desc") {
				sortSpec = "-" + sb
			} else {
				sortSpec = sb
			}
		}
	}
	var fields []string
	if rawFields := strings.TrimSpace(q.Get("fields")); rawFields != "" {
		for _, f := range strings.Split(rawFields, ",") {
			if tf := strings.TrimSpace(f); tf != "" {
				fields = append(fields, tf)
			}
		}
	}
	page := (offset / limit) + 1
	return pageParams{
		Query: queryStr, Sort: sortSpec, Fields: fields,
		Limit: limit, Offset: offset, Page: page, PageSize: limit,
	}, true
}

func parseLimitOffsetAndPage(w http.ResponseWriter, r *http.Request, q url.Values, defLimit, maxLimit int) (int, int, bool) {
	limit := defLimit
	if rawLimit := firstNonEmpty(q.Get("limit"), q.Get("page_size")); rawLimit != "" {
		parsed, err := strconv.Atoi(rawLimit)
		if err != nil || parsed < 1 {
			writeProblem(w, r, http.StatusBadRequest, "invalid-limit", fmt.Sprintf("Query parameter 'limit' must be a positive integer (1..%d).", maxLimit))
			return 0, 0, false
		}
		if parsed > maxLimit {
			parsed = maxLimit
		}
		limit = parsed
	}
	offset := 0
	if rawPage := strings.TrimSpace(q.Get("page")); rawPage != "" {
		page, err := strconv.Atoi(rawPage)
		if err != nil || page < 1 {
			writeProblem(w, r, http.StatusBadRequest, "invalid-page", "Query parameter 'page' must be an integer >= 1.")
			return 0, 0, false
		}
		offset = (page - 1) * limit
	}
	if rawOffset := firstNonEmpty(q.Get("offset"), q.Get("cursor")); rawOffset != "" {
		parsed, err := strconv.Atoi(rawOffset)
		if err != nil || parsed < 0 {
			writeProblem(w, r, http.StatusBadRequest, "invalid-offset", "Query parameter 'offset' or 'cursor' must be a non-negative integer.")
			return 0, 0, false
		}
		offset = parsed
	}
	return limit, offset, true
}

func buildPaginatedEnvelope(r *http.Request, items any, domainKey string, total, limit, offset int, fields []string) (map[string]any, string) {
	if limit <= 0 {
		limit = 50
	}
	projected := projectSliceFields(items, fields)
	count := sliceLen(projected)
	hasMore := offset+count < total
	nextCursor := ""
	if hasMore {
		nextCursor = strconv.Itoa(offset + count)
	}
	page := (offset / limit) + 1
	totalPages := 1
	if total > 0 {
		totalPages = (total + limit - 1) / limit
	}
	links := buildHateoasLinks(r, limit, offset, total, hasMore)
	env := map[string]any{
		"schema_version": "1.0",
		"items":          projected,
		"count":          count,
		"total":          total,
		"limit":          limit,
		"offset":         offset,
		"page":           page,
		"page_size":      limit,
		"total_pages":    totalPages,
		"has_more":       hasMore,
		"next_cursor":    nextCursor,
		"_links":         links,
	}
	if domainKey != "" && domainKey != "items" {
		env[domainKey] = projected
	}
	hdrJSON, _ := json.Marshal(map[string]any{
		"total":       total,
		"page":        page,
		"page_size":   limit,
		"total_pages": totalPages,
		"has_more":    hasMore,
	})
	return env, string(hdrJSON)
}

func buildHateoasLinks(r *http.Request, limit, offset, total int, hasMore bool) map[string]map[string]string {
	makeHref := func(off int) map[string]string {
		q := cloneURLValues(r.URL.Query())
		q.Set("limit", strconv.Itoa(limit))
		q.Set("offset", strconv.Itoa(off))
		return map[string]string{"href": r.URL.Path + "?" + q.Encode()}
	}
	lastOffset := 0
	if total > limit {
		lastOffset = ((total - 1) / limit) * limit
	}
	links := map[string]map[string]string{
		"self":  makeHref(offset),
		"first": makeHref(0),
		"last":  makeHref(lastOffset),
	}
	if hasMore {
		links["next"] = makeHref(offset + limit)
	}
	if offset > 0 {
		prevOff := offset - limit
		if prevOff < 0 {
			prevOff = 0
		}
		links["prev"] = makeHref(prevOff)
	}
	return links
}

func cloneURLValues(src url.Values) url.Values {
	dst := make(url.Values, len(src))
	for k, v := range src {
		dst[k] = append([]string(nil), v...)
	}
	return dst
}

func projectSliceFields(items any, fields []string) any {
	if len(fields) == 0 {
		return items
	}
	b, err := json.Marshal(items)
	if err != nil {
		return items
	}
	var rawRows []map[string]any
	if err := json.Unmarshal(b, &rawRows); err != nil {
		return items
	}
	allow := make(map[string]bool, len(fields))
	for _, f := range fields {
		allow[strings.ToLower(strings.TrimSpace(f))] = true
	}
	out := make([]map[string]any, len(rawRows))
	for i, row := range rawRows {
		filtered := make(map[string]any, len(fields))
		for k, v := range row {
			if allow[strings.ToLower(k)] {
				filtered[k] = v
			}
		}
		out[i] = filtered
	}
	return out
}

func sliceLen(items any) int {
	b, err := json.Marshal(items)
	if err != nil {
		return 0
	}
	var arr []json.RawMessage
	if json.Unmarshal(b, &arr) == nil {
		return len(arr)
	}
	return 0
}

func checkAcceptHeader(w http.ResponseWriter, r *http.Request) bool {
	accept := strings.TrimSpace(r.Header.Get("Accept"))
	if accept == "" || strings.Contains(accept, "*/*") || strings.Contains(accept, "application/*") || strings.Contains(accept, "application/json") || strings.Contains(accept, "application/problem+json") {
		return true
	}
	writeProblem(w, r, http.StatusNotAcceptable, "not-acceptable", fmt.Sprintf("Accept header %q is not supported; use application/json.", accept))
	return false
}

func checkJSONContentType(w http.ResponseWriter, r *http.Request) bool {
	ct := strings.ToLower(strings.TrimSpace(r.Header.Get("Content-Type")))
	if ct == "" || strings.HasPrefix(ct, "application/json") {
		return true
	}
	writeProblem(w, r, http.StatusUnsupportedMediaType, "unsupported-media-type", fmt.Sprintf("Content-Type %q is not supported; use application/json.", ct))
	return false
}

func normalizeETagStem(tag string) string {
	t := strings.TrimSpace(tag)
	if strings.HasPrefix(t, "W/") || strings.HasPrefix(t, "w/") {
		t = strings.TrimSpace(t[2:])
	}
	t = strings.Trim(t, `"`)
	t = strings.TrimSuffix(t, "-br")
	t = strings.TrimSuffix(t, "-gzip")
	return t
}

func matchesIfNoneMatch(headerVal, etag string) bool {
	if strings.TrimSpace(headerVal) == "" || strings.TrimSpace(etag) == "" {
		return false
	}
	target := normalizeETagStem(etag)
	for _, part := range strings.Split(headerVal, ",") {
		cand := strings.TrimSpace(part)
		if cand == "*" || (target != "" && normalizeETagStem(cand) == target) {
			return true
		}
	}
	return false
}

func notModifiedSince(r *http.Request, lastMod time.Time) bool {
	if lastMod.IsZero() || strings.TrimSpace(r.Header.Get("If-None-Match")) != "" {
		return false
	}
	imsRaw := strings.TrimSpace(r.Header.Get("If-Modified-Since"))
	if imsRaw == "" {
		return false
	}
	ims, err := http.ParseTime(imsRaw)
	if err != nil {
		return false
	}
	return !lastMod.UTC().Truncate(time.Second).After(ims.UTC().Truncate(time.Second))
}

func acceptsEncoding(r *http.Request, enc string) bool {
	for _, part := range strings.Split(strings.ToLower(r.Header.Get("Accept-Encoding")), ",") {
		token := strings.TrimSpace(strings.SplitN(part, ";", 2)[0])
		if token == enc && !strings.Contains(part, "q=0") {
			return true
		}
	}
	return false
}

// encodeBrotli frames payload into RFC 7932 compliant uncompressed Brotli metablocks.
func encodeBrotli(data []byte) []byte {
	if len(data) == 0 {
		return []byte{0x06}
	}
	var out bytes.Buffer
	first := true
	for len(data) > 0 {
		chunk := data
		if len(chunk) > 65536 {
			chunk = data[:65536]
		}
		data = data[len(chunk):]
		mlenMinus1 := uint32(len(chunk) - 1)
		if first {
			hdr := (mlenMinus1 << 4) | (1 << 20)
			out.Write([]byte{byte(hdr), byte(hdr >> 8), byte(hdr >> 16)})
			first = false
		} else {
			hdr := (mlenMinus1 << 3) | (1 << 19)
			out.Write([]byte{byte(hdr), byte(hdr >> 8), byte(hdr >> 16)})
		}
		out.Write(chunk)
	}
	out.WriteByte(0x03)
	return out.Bytes()
}

func decodeBrotli(stream []byte) ([]byte, error) {
	if len(stream) == 0 {
		return nil, fmt.Errorf("brotli: empty stream")
	}
	if len(stream) == 1 && stream[0] == 0x06 {
		return []byte{}, nil
	}
	var out bytes.Buffer
	pos, first := 0, true
	for pos < len(stream) {
		if !first && stream[pos] == 0x03 && pos+1 == len(stream) {
			return out.Bytes(), nil
		}
		if pos+3 > len(stream) {
			return nil, fmt.Errorf("brotli: truncated metablock header at %d", pos)
		}
		hdr := uint32(stream[pos]) | (uint32(stream[pos+1]) << 8) | (uint32(stream[pos+2]) << 16)
		pos += 3
		var mlen int
		if first {
			mlen = int((hdr>>4)&0xFFFF) + 1
			first = false
		} else {
			mlen = int((hdr>>3)&0xFFFF) + 1
		}
		if pos+mlen > len(stream) {
			return nil, fmt.Errorf("brotli: truncated metablock payload at %d", pos)
		}
		out.Write(stream[pos : pos+mlen])
		pos += mlen
	}
	return out.Bytes(), nil
}

func writeCachedJSON(w http.ResponseWriter, r *http.Request, cacheControl string, payload any) {
	writeCachedJSONWithModTime(w, r, cacheControl, time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC), payload)
}

func writeCachedJSONWithModTime(w http.ResponseWriter, r *http.Request, cacheControl string, lastMod time.Time, payload any) {
	var rawBuf bytes.Buffer
	_ = json.NewEncoder(&rawBuf).Encode(payload)
	body := rawBuf.Bytes()
	etag := telemetry.ComputeStrongETag(body)
	enc := ""
	if acceptsEncoding(r, "br") {
		enc = "br"
		etag = strings.TrimSuffix(etag, `"`) + `-br"`
	} else if acceptsEncoding(r, "gzip") {
		enc = "gzip"
		etag = strings.TrimSuffix(etag, `"`) + `-gzip"`
	}
	w.Header().Set("ETag", etag)
	w.Header().Set("Vary", "Accept-Encoding")
	if !lastMod.IsZero() {
		w.Header().Set("Last-Modified", lastMod.UTC().Format(http.TimeFormat))
	}
	if cacheControl != "" {
		w.Header().Set("Cache-Control", cacheControl)
	}
	if matchesIfNoneMatch(r.Header.Get("If-None-Match"), etag) || notModifiedSince(r, lastMod) {
		w.WriteHeader(http.StatusNotModified)
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	writeEncodedBody(w, r, enc, body)
}

func writeEncodedBody(w http.ResponseWriter, r *http.Request, enc string, body []byte) {
	switch enc {
	case "br":
		brBytes := encodeBrotli(body)
		w.Header().Set("Content-Encoding", "br")
		w.Header().Set("Content-Length", strconv.Itoa(len(brBytes)))
		w.WriteHeader(http.StatusOK)
		if r.Method != http.MethodHead {
			_, _ = w.Write(brBytes)
		}
	case "gzip":
		var gzBuf bytes.Buffer
		gz := gzip.NewWriter(&gzBuf)
		_, _ = gz.Write(body)
		_ = gz.Close()
		w.Header().Set("Content-Encoding", "gzip")
		w.Header().Set("Content-Length", strconv.Itoa(gzBuf.Len()))
		w.WriteHeader(http.StatusOK)
		if r.Method != http.MethodHead {
			_, _ = w.Write(gzBuf.Bytes())
		}
	default:
		w.Header().Set("Content-Length", strconv.Itoa(len(body)))
		w.WriteHeader(http.StatusOK)
		if r.Method != http.MethodHead {
			_, _ = w.Write(body)
		}
	}
}

func (s *Server) handleOpenAPI(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	doc := map[string]any{
		"openapi": "3.1.0",
		"info": map[string]any{
			"title":   "SandalCore Mesh-Gateway & Web Admin API",
			"version": "1.0.0",
		},
		"paths": map[string]any{
			"/api/v1/catalog/items":           map[string]any{"get": map[string]any{"summary": "Search extracted items from content.db and item_names.tsv"}},
			"/api/v1/catalog/npcs":            map[string]any{"get": map[string]any{"summary": "Search extracted NPCs from content.db and npc_names.tsv"}},
			"/api/v1/catalog/places":          map[string]any{"get": map[string]any{"summary": "Search Verra spawn places and city nodes"}},
			"/api/v1/world/pmtiles":           map[string]any{"get": map[string]any{"summary": "Serve verra.pmtiles with HTTP Range support"}},
			"/api/v1/world/tiles/{z}/{x}/{y}": map[string]any{"get": map[string]any{"summary": "Serve Verra base pyramid (z=0..7) and node plug tier (z=8) PNG tiles"}},
			"/api/v1/world/map-meta":          map[string]any{"get": map[string]any{"summary": "Serve Verra grid metadata and 26 levelable nodes with live stages"}},
			"/api/v1/world/players":           map[string]any{"get": map[string]any{"summary": "Search online and offline players"}},
			"/api/v1/world/nodes/{node}":      map[string]any{"put": map[string]any{"summary": "Set city node stage (0..6)"}},
			"/api/v1/gm/commands":             map[string]any{"get": map[string]any{"summary": "List GM commands"}, "post": map[string]any{"summary": "Execute GM command via WorldAdminService / CityNodeService"}},
			"/api/v1/gm/containers":           map[string]any{"get": map[string]any{"summary": "Scrape 11 aoc-* container stats and world status"}},
			"/api/v1/gm/logs":                 map[string]any{"get": map[string]any{"summary": "Tail demuxed container logs from /var/run/docker.sock"}},
		},
		"components": map[string]any{
			"schemas": map[string]any{
				"ProblemDetails": map[string]any{
					"type":     "object",
					"required": []string{"type", "title", "status", "detail"},
				},
			},
		},
	}
	writeCachedJSON(w, r, "public, max-age=60", doc)
}
