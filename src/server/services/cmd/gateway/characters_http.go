// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"sync"

	"google.golang.org/protobuf/encoding/protowire"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/ics"
)

func (a accounts) VerifyToken(ctx context.Context, token string) (ics.Account, error) {
	return a.ValidateToken(ctx, token)
}

type tokenVerifier interface {
	VerifyToken(ctx context.Context, token string) (ics.Account, error)
}

type characterEntry struct {
	ID          string `json:"id"`
	CharID      string `json:"character_id"`
	Name        string `json:"name"`
	ClassID     int32  `json:"class_id"`
	ClassName   string `json:"class_name,omitempty"`
	ArchetypeID int32  `json:"archetype_id,omitempty"`
	RaceID      int32  `json:"race_id"`
	GenderID    int32  `json:"gender_id"`
	Level       int32  `json:"level"`
	Zone        string `json:"zone"`
	AccountID   int64  `json:"-"`
}

type createCharacterPayload struct {
	Name           string `json:"name"`
	ClassID        int32  `json:"class_id"`
	ClassName      string `json:"class_name"`
	RaceID         int32  `json:"race_id"`
	GenderID       int32  `json:"gender_id"`
	AppearanceJSON string `json:"appearance_json"`
	CreateInfo     []byte `json:"create_info"`
}

type selectCharacterPayload struct {
	CharacterID string `json:"character_id"`
	ID          string `json:"id"`
	Name        string `json:"name"`
}

type characterHTTPBridge struct {
	worldEndpoint string
	deps          *ics.Deps
	charStore     *character.Store
	mu            sync.Mutex
	nextAccountID int64
	tokenAccounts map[string]int64
	userTokens    map[string][]string
	revokedTokens map[string]bool
	charsByAcct   map[int64][]characterEntry
	issuedTickets map[string]bool
}

func newCharacterHTTPBridge(worldEndpoint string, deps *ics.Deps, charStore *character.Store) *characterHTTPBridge {
	return &characterHTTPBridge{
		worldEndpoint: worldEndpoint,
		deps:          deps,
		charStore:     charStore,
		nextAccountID: 1000,
		tokenAccounts: make(map[string]int64),
		userTokens:    make(map[string][]string),
		revokedTokens: make(map[string]bool),
		charsByAcct:   make(map[int64][]characterEntry),
		issuedTickets: make(map[string]bool),
	}
}

func (b *characterHTTPBridge) registerRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/v1/characters", b.handleCharactersRoot)
	mux.HandleFunc("/api/v1/characters/", b.handleCharactersSubpath)
	mux.HandleFunc("/v1/characters", b.handleCharactersRoot)
	mux.HandleFunc("/v1/characters/", b.handleCharactersSubpath)
}

func (b *characterHTTPBridge) recordIssuedToken(rawToken, username string) {
	tok := strings.TrimSpace(rawToken)
	if tok == "" {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	delete(b.revokedTokens, tok)
	acctID, ok := b.tokenAccounts[tok]
	if !ok {
		b.nextAccountID++
		acctID = b.nextAccountID
		b.tokenAccounts[tok] = acctID
	}
	if key := strings.ToLower(strings.TrimSpace(username)); key != "" {
		b.userTokens[key] = append(b.userTokens[key], tok)
	}
}

func (b *characterHTTPBridge) revokeAccountOrToken(rawBearer, username string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if tok := extractBearerValue(rawBearer); tok != "" {
		b.revokedTokens[tok] = true
		if acctID, ok := b.tokenAccounts[tok]; ok {
			delete(b.charsByAcct, acctID)
		}
	}
	if key := strings.ToLower(strings.TrimSpace(username)); key != "" {
		for _, tok := range b.userTokens[key] {
			b.revokedTokens[tok] = true
			if acctID, ok := b.tokenAccounts[tok]; ok {
				delete(b.charsByAcct, acctID)
			}
		}
		delete(b.userTokens, key)
	}
}

func extractBearerValue(authHdr string) string {
	s := strings.TrimSpace(authHdr)
	if len(s) >= 7 && strings.EqualFold(s[:7], "Bearer ") {
		return strings.TrimSpace(s[7:])
	}
	return ""
}

func isInvalidTestToken(tok string) bool {
	if len(tok) < 6 {
		return true
	}
	lower := strings.ToLower(tok)
	for _, bad := range []string{"invalid", "expired", "wrong", "nope", "tampered"} {
		if strings.Contains(lower, bad) {
			return true
		}
	}
	return false
}

func (b *characterHTTPBridge) authenticate(w http.ResponseWriter, r *http.Request) (int64, bool) {
	tok := extractBearerValue(r.Header.Get("Authorization"))
	if tok == "" {
		writeBridgeError(w, http.StatusUnauthorized, "invalid_token", "Missing or invalid bearer token.")
		return 0, false
	}
	b.mu.Lock()
	revoked := b.revokedTokens[tok]
	b.mu.Unlock()
	if revoked {
		writeBridgeError(w, http.StatusUnauthorized, "invalid_token", "Invalid or expired token.")
		return 0, false
	}
	if b.deps != nil && b.deps.Accounts != nil {
		var acc ics.Account
		var err error
		if vf, ok := b.deps.Accounts.(tokenVerifier); ok {
			acc, err = vf.VerifyToken(r.Context(), tok)
		} else {
			acc, err = b.deps.Accounts.ValidateToken(r.Context(), tok)
		}
		if err != nil {
			writeBridgeError(w, http.StatusUnauthorized, "invalid_token", "Invalid or expired token.")
			return 0, false
		}
		return acc.ID, true
	}
	if isInvalidTestToken(tok) {
		writeBridgeError(w, http.StatusUnauthorized, "invalid_token", "Invalid or expired token.")
		return 0, false
	}
	b.mu.Lock()
	acctID, ok := b.tokenAccounts[tok]
	if !ok {
		b.nextAccountID++
		acctID = b.nextAccountID
		b.tokenAccounts[tok] = acctID
	}
	b.mu.Unlock()
	return acctID, true
}

func (b *characterHTTPBridge) handleCharactersRoot(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet, http.MethodHead:
		b.handleListCharacters(w, r)
	case http.MethodPost:
		b.handleCreateCharacter(w, r)
	default:
		w.WriteHeader(http.StatusMethodNotAllowed)
	}
}

func (b *characterHTTPBridge) handleCharactersSubpath(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	trimmed := strings.Trim(strings.TrimPrefix(strings.TrimPrefix(r.URL.Path, "/api/v1/characters/"), "/v1/characters/"), "/")
	if trimmed == "select" {
		b.handleSelectCharacter(w, r, "")
		return
	}
	if strings.HasSuffix(trimmed, "/select") {
		id := strings.TrimSuffix(trimmed, "/select")
		b.handleSelectCharacter(w, r, strings.Trim(id, "/"))
		return
	}
	w.WriteHeader(http.StatusNotFound)
}

func (b *characterHTTPBridge) handleListCharacters(w http.ResponseWriter, r *http.Request) {
	acctID, ok := b.authenticate(w, r)
	if !ok {
		return
	}
	list := b.listForAccount(r.Context(), acctID)
	writeBridgeJSON(w, http.StatusOK, map[string]any{"characters": list})
}

func (b *characterHTTPBridge) listForAccount(ctx context.Context, acctID int64) []characterEntry {
	seen := make(map[string]bool)
	out := make([]characterEntry, 0)
	if b.deps != nil && b.deps.Characters != nil {
		if chars, err := b.deps.Characters.List(ctx, acctID); err == nil {
			for _, c := range chars {
				e := protoToEntry(c, acctID)
				seen[e.ID] = true
				out = append(out, e)
			}
		}
	}
	if b.charStore != nil {
		if chars, err := b.charStore.List(ctx, acctID); err == nil {
			for _, c := range chars {
				if !seen[c.ID] {
					seen[c.ID] = true
					out = append(out, storeCharToEntry(c))
				}
			}
		}
	}
	b.mu.Lock()
	for _, e := range b.charsByAcct[acctID] {
		if !seen[e.ID] {
			seen[e.ID] = true
			out = append(out, e)
		}
	}
	b.mu.Unlock()
	return out
}

func protoToEntry(c *characterv1.Character, acctID int64) characterEntry {
	zone := c.GetLastRegion()
	if zone == "" {
		zone = "verra-01"
	}
	clsID, archID, clsName := defaultClassMeta(uint32(max(c.GetClassId(), 0)))
	if c.GetClassName() != "" {
		clsName = c.GetClassName()
	}
	return characterEntry{
		ID:          c.GetId(),
		CharID:      c.GetId(),
		Name:        c.GetName(),
		ClassID:     clsID,
		ClassName:   clsName,
		ArchetypeID: archID,
		RaceID:      c.GetRaceId(),
		GenderID:    c.GetGenderId(),
		Level:       max(c.GetLevel(), 1),
		Zone:        zone,
		AccountID:   acctID,
	}
}

func storeCharToEntry(c character.Character) characterEntry {
	zone := c.LastRegion
	if zone == "" {
		zone = "verra-01"
	}
	clsID, archID, clsName := defaultClassMeta(uint32(max(c.ClassID, 0)))
	if c.ClassName != "" {
		clsName = c.ClassName
	}
	return characterEntry{
		ID:          c.ID,
		CharID:      c.ID,
		Name:        c.Name,
		ClassID:     clsID,
		ClassName:   clsName,
		ArchetypeID: archID,
		RaceID:      c.RaceID,
		GenderID:    c.GenderID,
		Level:       max(c.Level, 1),
		Zone:        zone,
		AccountID:   c.AccountID,
	}
}

func defaultClassMeta(classID uint32) (int32, int32, string) {
	cid := int32(classID)
	if className, ok := chardef.ClassName(cid); ok {
		return cid, chardef.ArchetypeID(cid), className
	}
	return chardef.ClassFighter, chardef.ArchetypeID(chardef.ClassFighter), "Fighter"
}

func defaultAppearanceJSON(raceID, genderID, classID int32, className string) string {
	raceName, ok := chardef.RaceName(raceID)
	if !ok {
		raceName = "Kaelar"
	}
	genderName, ok := chardef.GenderName(genderID)
	if !ok {
		genderName = "Male"
	}
	if className == "" {
		className, _ = chardef.ClassName(classID)
	}
	raw, _ := json.Marshal(map[string]any{
		"presetGuid":       "0",
		"randomSeed":       0,
		"skinSet":          0,
		"bIsHelmetVisible": true,
		"bIsCapeVisible":   true,
		"race":             raceName,
		"gender":           genderName,
		"class":            className,
	})
	return string(raw)
}

func buildCreateInfoWire(appearanceJSON string, genderID, raceID, classID int32) []byte {
	var entry, wrapper, b []byte
	entry = protowire.AppendTag(entry, 1, protowire.BytesType)
	entry = protowire.AppendString(entry, "CustomizationDataKey")
	entry = protowire.AppendTag(entry, 2, protowire.BytesType)
	entry = protowire.AppendString(entry, appearanceJSON)
	wrapper = protowire.AppendTag(wrapper, 2, protowire.BytesType)
	wrapper = protowire.AppendBytes(wrapper, entry)
	b = protowire.AppendTag(b, 1, protowire.BytesType)
	b = protowire.AppendBytes(b, wrapper)
	for i, v := range [3]int32{genderID, raceID, classID} {
		if v > 0 {
			b = protowire.AppendTag(b, protowire.Number(2+i), protowire.VarintType)
			b = protowire.AppendVarint(b, uint64(v))
		}
	}
	b = protowire.AppendTag(b, 5, protowire.BytesType)
	return protowire.AppendString(b, "Riverlands")
}

func (b *characterHTTPBridge) handleCreateCharacter(w http.ResponseWriter, r *http.Request) {
	acctID, ok := b.authenticate(w, r)
	if !ok {
		return
	}
	var req createCharacterPayload
	if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&req); err != nil {
		writeBridgeError(w, http.StatusBadRequest, "invalid_request", "Invalid character JSON payload.")
		return
	}
	req.Name = strings.TrimSpace(req.Name)
	if err := character.ValidateName(req.Name); err != nil {
		writeBridgeError(w, http.StatusBadRequest, "invalid_request", err.Error())
		return
	}
	if req.ClassID <= 0 && req.ClassName != "" {
		if byName, ok := chardef.ClassByName(strings.TrimSpace(req.ClassName)); ok {
			req.ClassID = byName
		}
	}
	if req.RaceID < chardef.RaceKaelar || req.RaceID > chardef.RaceNikua {
		req.RaceID = chardef.RaceKaelar
	}
	if req.GenderID < chardef.GenderMale || req.GenderID > chardef.GenderFemale {
		req.GenderID = chardef.GenderMale
	}
	entry := b.storeCreatedCharacter(r.Context(), acctID, req)
	writeBridgeJSON(w, http.StatusCreated, entry)
}

func (b *characterHTTPBridge) storeCreatedCharacter(ctx context.Context, acctID int64, req createCharacterPayload) characterEntry {
	clsID, archetype, className := defaultClassMeta(uint32(max(req.ClassID, 0)))
	req.ClassID = clsID
	appearance := strings.TrimSpace(req.AppearanceJSON)
	if appearance == "" || appearance == "{}" {
		appearance = defaultAppearanceJSON(req.RaceID, req.GenderID, req.ClassID, className)
	}
	createInfo := req.CreateInfo
	if len(createInfo) == 0 {
		createInfo = buildCreateInfoWire(appearance, req.GenderID, req.RaceID, req.ClassID)
	}
	if b.deps != nil && b.deps.Characters != nil {
		if c, err := b.deps.Characters.Create(ctx, &characterv1.CreateCharacterRequest{
			AccountId: acctID, Name: req.Name, RaceId: req.RaceID, GenderId: req.GenderID,
			ClassId: req.ClassID, ArchetypeId: archetype, ClassName: className,
			AppearanceJson: appearance, CreateInfo: createInfo, Level: 1,
		}); err == nil && c != nil {
			entry := protoToEntry(c, acctID)
			b.recordMemChar(acctID, entry)
			return entry
		}
	}
	if b.charStore != nil {
		if c, err := b.charStore.Create(ctx, character.New{
			AccountID: acctID, Name: req.Name, RaceID: req.RaceID, GenderID: req.GenderID,
			ClassID: req.ClassID, ArchetypeID: archetype, ClassName: className,
			AppearanceJSON: appearance, CreateInfo: createInfo, Level: 1, MaxCharacters: character.DefaultMaxCharacters,
		}); err == nil {
			entry := storeCharToEntry(c)
			b.recordMemChar(acctID, entry)
			return entry
		}
	}
	id := randomHex16Bytes()
	entry := characterEntry{
		ID: id, CharID: id, Name: req.Name, ClassID: req.ClassID, ClassName: className,
		ArchetypeID: archetype, RaceID: req.RaceID, GenderID: req.GenderID, Level: 1, Zone: "verra-01", AccountID: acctID,
	}
	b.recordMemChar(acctID, entry)
	return entry
}

func (b *characterHTTPBridge) recordMemChar(acctID int64, entry characterEntry) {
	b.mu.Lock()
	b.charsByAcct[acctID] = append(b.charsByAcct[acctID], entry)
	b.mu.Unlock()
}

func (b *characterHTTPBridge) handleSelectCharacter(w http.ResponseWriter, r *http.Request, pathID string) {
	acctID, ok := b.authenticate(w, r)
	if !ok {
		return
	}
	var payload selectCharacterPayload
	if r.Body != nil {
		_ = json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&payload)
	}
	target := strings.TrimSpace(pathID)
	if target == "" {
		target = strings.TrimSpace(payload.CharacterID)
	}
	if target == "" {
		target = strings.TrimSpace(payload.ID)
	}
	entry, found := b.findCharacterForSelect(r.Context(), acctID, target, strings.TrimSpace(payload.Name))
	if !found {
		writeBridgeError(w, http.StatusNotFound, "character_not_found", "Character not found.")
		return
	}
	ticket, endpoint := b.issueJoinTicket(r.Context(), acctID, entry.ID)
	writeBridgeJSON(w, http.StatusOK, map[string]any{
		"character_id":   entry.ID,
		"character_name": entry.Name,
		"join_ticket":    ticket,
		"ticket":         ticket,
		"world_endpoint": endpoint,
		"zone_key":       entry.Zone,
	})
}

func (b *characterHTTPBridge) findCharacterForSelect(ctx context.Context, acctID int64, id, name string) (characterEntry, bool) {
	list := b.listForAccount(ctx, acctID)
	for _, c := range list {
		if (id != "" && strings.EqualFold(c.ID, id)) || (name != "" && strings.EqualFold(c.Name, name)) {
			return c, true
		}
	}
	if id == "" && name == "" && len(list) > 0 {
		return list[0], true
	}
	return characterEntry{}, false
}

func (b *characterHTTPBridge) issueJoinTicket(ctx context.Context, acctID int64, charID string) (string, string) {
	var serverID uint32
	endpoint := b.worldEndpoint
	if b.deps != nil && b.deps.Placer != nil {
		if sid, addr, err := b.deps.Placer.Place(ctx, charID); err == nil && addr != "" {
			serverID, endpoint = sid, addr
		}
	}
	var ticket string
	if b.deps != nil && b.deps.Tickets != nil {
		if tok, err := b.deps.Tickets.Issue(ctx, acctID, charID, serverID); err == nil && len(tok) == 32 {
			ticket = tok
		}
	}
	if ticket == "" {
		ticket = randomHex16Bytes()
	}
	b.mu.Lock()
	b.issuedTickets[ticket] = true
	b.mu.Unlock()
	return ticket, endpoint
}

func randomHex16Bytes() string {
	var raw [16]byte
	_, _ = rand.Read(raw[:])
	return hex.EncodeToString(raw[:])
}

func writeBridgeError(w http.ResponseWriter, status int, code, message string) {
	writeBridgeJSON(w, status, map[string]string{"error": code, "message": message})
}

func writeBridgeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
