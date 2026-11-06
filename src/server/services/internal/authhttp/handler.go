// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package authhttp serves the launcher's login API: POST
// /api/v1/auth/login and /api/v1/auth/register with {"username","password"}.
// The launcher treats a JSON body with access_token as success and shows
// "message" otherwise (src/launcher_main.cpp).
package authhttp

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"aocrop/services/internal/account"
	"aocrop/services/pkg/telemetry"
)

// Accounts is the part of account.Store the handlers use.
type Accounts interface {
	Register(ctx context.Context, username, password string) (account.Account, error)
	Authenticate(ctx context.Context, username, password string) (account.Account, error)
	IssueToken(ctx context.Context, accountID int64) (account.Token, error)
	TokenTTL() time.Duration
	DeleteAccount(ctx context.Context, username, password string) error
	DeleteAccountByToken(ctx context.Context, rawToken string) error
}

type credentials struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

func Handler(accounts Accounts) http.Handler {
	mux := http.NewServeMux()
	gate := telemetry.NewReadinessGate("auth", 1)
	gate.SetReady(true, true)
	probeHandler := gate.Handler()
	for _, p := range []string{"GET /livez", "GET /readyz", "GET /healthz", "GET /metrics"} {
		mux.Handle(p, probeHandler)
	}
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
	})
	login := func(w http.ResponseWriter, r *http.Request) { handleLogin(w, r, accounts) }
	register := func(w http.ResponseWriter, r *http.Request) { handleRegister(w, r, accounts) }
	del := func(w http.ResponseWriter, r *http.Request) { handleDeleteAccount(w, r, accounts) }
	mux.HandleFunc("POST /api/v1/auth/login", login)
	mux.HandleFunc("POST /v1/login", login)
	mux.HandleFunc("POST /api/v1/auth/register", register)
	mux.HandleFunc("POST /v1/register", register)
	mux.HandleFunc("DELETE /api/v1/auth/account", del)
	mux.HandleFunc("DELETE /v1/account", del)
	mux.HandleFunc("POST /api/v1/auth/account/delete", del)
	mux.HandleFunc("POST /v1/account/delete", del)
	return mux
}

func handleLogin(w http.ResponseWriter, r *http.Request, accounts Accounts) {
	c, ok := readCredentials(w, r)
	if !ok {
		return
	}
	acc, err := accounts.Authenticate(r.Context(), c.Username, c.Password)
	if errors.Is(err, account.ErrInvalidCredentials) {
		writeError(w, http.StatusUnauthorized, "invalid_credentials", "Wrong username or password.")
		return
	}
	if err != nil {
		serverError(w, r, err)
		return
	}
	respondWithToken(w, r, accounts, acc, "login")
}

func handleRegister(w http.ResponseWriter, r *http.Request, accounts Accounts) {
	c, ok := readCredentials(w, r)
	if !ok {
		return
	}
	if err := account.ValidateCredentials(c.Username, c.Password); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_request", err.Error())
		return
	}
	acc, err := accounts.Register(r.Context(), c.Username, c.Password)
	if errors.Is(err, account.ErrUsernameTaken) {
		writeError(w, http.StatusConflict, "username_taken", "That username is taken.")
		return
	}
	if errors.Is(err, account.ErrInvalidInput) {
		writeError(w, http.StatusBadRequest, "invalid_request", err.Error())
		return
	}
	if err != nil {
		serverError(w, r, err)
		return
	}
	respondWithToken(w, r, accounts, acc, "register")
}

func handleDeleteAccount(w http.ResponseWriter, r *http.Request, accounts Accounts) {
	if authHdr := strings.TrimSpace(r.Header.Get("Authorization")); authHdr != "" {
		handleDeleteByBearer(w, r, accounts, authHdr)
		return
	}
	handleDeleteByCredentials(w, r, accounts)
}

func handleDeleteByBearer(w http.ResponseWriter, r *http.Request, accounts Accounts, authHdr string) {
	if len(authHdr) < 7 || !strings.EqualFold(authHdr[:7], "Bearer ") {
		if !checkIfMatch(w, r, "", "") {
			return
		}
		writeError(w, http.StatusUnauthorized, "invalid_token", "Missing or invalid bearer token.")
		return
	}
	tok := strings.TrimSpace(authHdr[7:])
	if tok == "" {
		if !checkIfMatch(w, r, "", "") {
			return
		}
		writeError(w, http.StatusUnauthorized, "invalid_token", "Missing or invalid bearer token.")
		return
	}
	if !checkIfMatch(w, r, "", tok) {
		return
	}
	err := accounts.DeleteAccountByToken(r.Context(), tok)
	if errors.Is(err, account.ErrInvalidToken) || errors.Is(err, account.ErrInvalidCredentials) || errors.Is(err, account.ErrAccountNotFound) {
		writeError(w, http.StatusUnauthorized, "invalid_token", "Invalid or expired token.")
		return
	}
	if err != nil {
		serverError(w, r, err)
		return
	}
	writeDeleted(w, r, "", tok)
}

func handleDeleteByCredentials(w http.ResponseWriter, r *http.Request, accounts Accounts) {
	if (r.Body == nil || r.ContentLength == 0) && strings.TrimSpace(r.Header.Get("If-Match")) != "" {
		if !checkIfMatch(w, r, "", "") {
			return
		}
	}
	c, ok := readCredentials(w, r)
	if !ok {
		return
	}
	if !checkIfMatch(w, r, c.Username, "") {
		return
	}
	if strings.TrimSpace(c.Username) == "" || c.Password == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "Body must be JSON with username and password.")
		return
	}
	err := accounts.DeleteAccount(r.Context(), c.Username, c.Password)
	if errors.Is(err, account.ErrInvalidCredentials) || errors.Is(err, account.ErrAccountNotFound) {
		writeError(w, http.StatusUnauthorized, "invalid_credentials", "Wrong username or password.")
		return
	}
	if errors.Is(err, account.ErrInvalidInput) {
		writeError(w, http.StatusBadRequest, "invalid_request", err.Error())
		return
	}
	if err != nil {
		serverError(w, r, err)
		return
	}
	writeDeleted(w, r, c.Username, "")
}

func shortHexHash(s string) string {
	sum := sha256.Sum256([]byte(s))
	return hex.EncodeToString(sum[:8])
}

func accountTokenETag(username, token string) string {
	uHash := shortHexHash(strings.ToLower(strings.TrimSpace(username)))
	tHash := shortHexHash(strings.TrimSpace(token))
	return fmt.Sprintf(`"acct-%s-%s"`, uHash, tHash)
}

func matchesSingleETag(tag, username, token string) bool {
	if tag == "*" || tag == `"*"` {
		return true
	}
	if len(tag) < 2 || !strings.HasPrefix(tag, `"`) || !strings.HasSuffix(tag, `"`) {
		return false
	}
	if token != "" {
		tHash := shortHexHash(strings.TrimSpace(token))
		if tag == fmt.Sprintf(`"tok-%s"`, tHash) || strings.HasSuffix(tag, "-"+tHash+`"`) {
			return true
		}
	}
	if username != "" {
		uHash := shortHexHash(strings.ToLower(strings.TrimSpace(username)))
		if tag == fmt.Sprintf(`"acct-%s"`, uHash) || strings.HasPrefix(tag, `"acct-`+uHash+"-") {
			return true
		}
	}
	return false
}

func checkIfMatch(w http.ResponseWriter, r *http.Request, username, token string) bool {
	raw := strings.TrimSpace(r.Header.Get("If-Match"))
	if raw == "" {
		return true
	}
	for _, part := range strings.Split(raw, ",") {
		if matchesSingleETag(strings.TrimSpace(part), username, token) {
			return true
		}
	}
	writePreconditionFailed(w, r)
	return false
}

func writeDeleted(w http.ResponseWriter, r *http.Request, username, token string) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("ETag", accountTokenETag(username, token))
	q := r.URL.Query().Get("no_content")
	if q == "1" || q == "true" || strings.Contains(r.Header.Get("Prefer"), "return=minimal") {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"deleted": true})
}

func writePreconditionFailed(w http.ResponseWriter, r *http.Request) {
	const detail = "If-Match precondition failed."
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(http.StatusPreconditionFailed)
	_ = json.NewEncoder(w).Encode(map[string]any{
		"type":     "urn:sandalcore:problem:precondition-failed",
		"title":    "Precondition Failed",
		"status":   http.StatusPreconditionFailed,
		"detail":   detail,
		"instance": r.URL.Path,
		"error":    "precondition_failed",
		"message":  detail,
	})
}

// Cyb with C | O | A

func readCredentials(w http.ResponseWriter, r *http.Request) (credentials, bool) {
	var c credentials
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096))
	if err := dec.Decode(&c); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_request", "Body must be JSON with username and password.")
		return c, false
	}
	// Consume the whole bounded body: Decode alone accepts a valid prefix and
	// never notices another document, trailing junk or an oversized tail.
	if err := dec.Decode(new(any)); err != io.EOF {
		writeError(w, http.StatusBadRequest, "invalid_request", "Body must be JSON with username and password.")
		return c, false
	}
	return c, true
}

// serverError logs err and answers 500 without exposing it: store errors can
// name tables or hosts.
func serverError(w http.ResponseWriter, r *http.Request, err error) {
	slog.Error("auth request failed", "path", r.URL.Path, "err", err)
	writeError(w, http.StatusInternalServerError, "server_error", "The login server failed; try again.")
}

// respondWithToken answers a successful login or registration (event names
// which) with a new token.
func respondWithToken(w http.ResponseWriter, r *http.Request, accounts Accounts, acc account.Account, event string) {
	tok, err := accounts.IssueToken(r.Context(), acc.ID)
	if err != nil {
		serverError(w, r, err)
		return
	}
	slog.Info(event, "username", acc.Username)
	// RFC 6749 section 5.1: a token response must not be cached.
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("ETag", accountTokenETag(acc.Username, tok.Value))
	writeJSON(w, http.StatusOK, map[string]any{
		"access_token": tok.Value,
		"token_type":   "Bearer",
		"expires_in":   int(accounts.TokenTTL().Seconds()),
		"username":     acc.Username,
	})
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, map[string]string{"error": code, "message": message})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
