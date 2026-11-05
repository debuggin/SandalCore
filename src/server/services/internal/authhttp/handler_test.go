// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package authhttp

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"aocrop/services/internal/account"
)

// fakeAccounts is an in-memory Accounts for handler tests.
type fakeAccounts struct {
	users       map[string]string // lower(username) -> password
	tokens      map[string]string // token -> lower(username)
	lastUser    string
	registerErr error // when set, Register fails with it after validation
	authErr     error // when set, Authenticate fails with it
	tokenErr    error // when set, IssueToken fails with it
	deleteErr   error // when set, DeleteAccount / DeleteAccountByToken fails with it
}

func (f *fakeAccounts) Register(_ context.Context, u, p string) (account.Account, error) {
	if err := account.ValidateCredentials(u, p); err != nil {
		return account.Account{}, err
	}
	if f.registerErr != nil {
		return account.Account{}, f.registerErr
	}
	key := strings.ToLower(u)
	if _, ok := f.users[key]; ok {
		return account.Account{}, account.ErrUsernameTaken
	}
	f.users[key] = p
	f.lastUser = key
	return account.Account{ID: int64(len(f.users)), Username: u}, nil
}

// C | O | A.C | Y | B

func (f *fakeAccounts) Authenticate(_ context.Context, u, p string) (account.Account, error) {
	if f.authErr != nil {
		return account.Account{}, f.authErr
	}
	key := strings.ToLower(u)
	if pw, ok := f.users[key]; ok && pw == p {
		f.lastUser = key
		return account.Account{ID: 1, Username: u}, nil
	}
	return account.Account{}, account.ErrInvalidCredentials
}

func (f *fakeAccounts) IssueToken(_ context.Context, _ int64) (account.Token, error) {
	if f.tokenErr != nil {
		return account.Token{}, f.tokenErr
	}
	if f.tokens == nil {
		f.tokens = make(map[string]string)
	}
	if f.lastUser != "" {
		f.tokens["aoc_test"] = f.lastUser
	}
	return account.Token{Value: "aoc_test", ExpiresAt: time.Now().Add(time.Hour)}, nil
}

func (f *fakeAccounts) TokenTTL() time.Duration { return 24 * time.Hour }

func (f *fakeAccounts) DeleteAccount(_ context.Context, u, p string) error {
	if f.deleteErr != nil {
		return f.deleteErr
	}
	key := strings.ToLower(u)
	pw, ok := f.users[key]
	if !ok || pw != p {
		return account.ErrInvalidCredentials
	}
	delete(f.users, key)
	for tok, owner := range f.tokens {
		if owner == key {
			delete(f.tokens, tok)
		}
	}
	return nil
}

func (f *fakeAccounts) DeleteAccountByToken(_ context.Context, rawToken string) error {
	if f.deleteErr != nil {
		return f.deleteErr
	}
	tok := strings.TrimSpace(rawToken)
	key, ok := f.tokens[tok]
	if !ok {
		return account.ErrInvalidToken
	}
	if _, exists := f.users[key]; !exists {
		delete(f.tokens, tok)
		return account.ErrInvalidToken
	}
	delete(f.users, key)
	delete(f.tokens, tok)
	return nil
}

// postFull posts body to path and returns the recorder (status, headers, raw
// body) along with the decoded JSON body.
func postFull(t *testing.T, h http.Handler, path, body string) (*httptest.ResponseRecorder, map[string]any) {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	var out map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &out); err != nil {
		t.Fatalf("%s: response is not JSON: %q", path, rec.Body.String())
	}
	return rec, out
}

func post(t *testing.T, h http.Handler, path, body string) (int, map[string]any) {
	t.Helper()
	rec, out := postFull(t, h, path, body)
	return rec.Code, out
}

func TestRegisterThenLogin(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})

	rec, out := postFull(t, h, "/api/v1/auth/register", `{"username":"cyb","password":"secret1"}`)
	if rec.Code != http.StatusOK || out["access_token"] != "aoc_test" || out["username"] != "cyb" ||
		out["token_type"] != "Bearer" || out["expires_in"] != float64(86400) {
		t.Fatalf("register: %d %v", rec.Code, out)
	}
	// RFC 6749 section 5.1: token responses must not be cached.
	if got := rec.Header().Get("Cache-Control"); got != "no-store" {
		t.Errorf("register: Cache-Control = %q, want no-store", got)
	}

	rec, out = postFull(t, h, "/api/v1/auth/login", `{"username":"cyb","password":"secret1"}`)
	if rec.Code != http.StatusOK || out["access_token"] != "aoc_test" {
		t.Fatalf("login: %d %v", rec.Code, out)
	}
	if got := rec.Header().Get("Cache-Control"); got != "no-store" {
		t.Errorf("login: Cache-Control = %q, want no-store", got)
	}
}

func TestErrorsCarryMessageAndNoToken(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{"cyb": "secret1"}})
	for _, tc := range []struct {
		path, body string
		code       int
		errCode    string
	}{
		{"/api/v1/auth/login", `{"username":"cyb","password":"wrong"}`, 401, "invalid_credentials"},
		{"/api/v1/auth/register", `{"username":"CYB","password":"secret2"}`, 409, "username_taken"},
		{"/api/v1/auth/register", `{"username":"x","password":"secret2"}`, 400, "invalid_request"},
		{"/api/v1/auth/login", `not json`, 400, "invalid_request"},
	} {
		code, out := post(t, h, tc.path, tc.body)
		msg, _ := out["message"].(string)
		if code != tc.code || out["error"] != tc.errCode || msg == "" || out["access_token"] != nil {
			t.Errorf("%s %s: %d %v, want %d %s", tc.path, tc.body, code, out, tc.code, tc.errCode)
		}
	}
}

// A Register that rejects its input despite the handler's own validation
// (the two could drift apart) is still the caller's mistake, not a 500.
func TestRegisterInvalidInputFromStoreIs400(t *testing.T) {
	h := Handler(&fakeAccounts{
		users:       map[string]string{},
		registerErr: fmt.Errorf("%w: username is reserved", account.ErrInvalidInput),
	})
	code, out := post(t, h, "/api/v1/auth/register", `{"username":"cyb","password":"secret1"}`)
	if code != http.StatusBadRequest || out["error"] != "invalid_request" ||
		out["message"] != "invalid input: username is reserved" || out["access_token"] != nil {
		t.Errorf("register: %d %v", code, out)
	}
}

// [C-O-A :: C-Y-B]

// A store failure is a 500 with the launcher's message contract intact, and
// the underlying error (which may name tables or hosts) never reaches the wire.
func TestStoreFailuresAre500WithoutLeakingDetail(t *testing.T) {
	secret := errors.New("pq: secret detail")
	for _, tc := range []struct {
		name, path, body string
		accounts         *fakeAccounts
	}{
		{
			"login authenticate fails", "/api/v1/auth/login", `{"username":"cyb","password":"secret1"}`,
			&fakeAccounts{users: map[string]string{"cyb": "secret1"}, authErr: secret},
		},
		{
			"login issue token fails", "/api/v1/auth/login", `{"username":"cyb","password":"secret1"}`,
			&fakeAccounts{users: map[string]string{"cyb": "secret1"}, tokenErr: secret},
		},
		{
			"register issue token fails", "/api/v1/auth/register", `{"username":"cyb","password":"secret1"}`,
			&fakeAccounts{users: map[string]string{}, tokenErr: secret},
		},
		{
			"register store fails", "/api/v1/auth/register", `{"username":"cyb","password":"secret1"}`,
			&fakeAccounts{users: map[string]string{}, registerErr: secret},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			rec, out := postFull(t, Handler(tc.accounts), tc.path, tc.body)
			msg, _ := out["message"].(string)
			if rec.Code != http.StatusInternalServerError || out["error"] != "server_error" ||
				msg == "" || out["access_token"] != nil {
				t.Errorf("%d %v, want 500 server_error with a message and no token", rec.Code, out)
			}
			if strings.Contains(rec.Body.String(), "secret detail") {
				t.Errorf("response leaks the internal error: %q", rec.Body.String())
			}
		})
	}
}

// Bodies over the 4096-byte limit are rejected as malformed rather than read
// in full.
func TestOversizedBodyIs400(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})
	body := `{"username":"cyb","password":"` + strings.Repeat("a", 5000) + `"}`
	code, out := post(t, h, "/api/v1/auth/login", body)
	msg, _ := out["message"].(string)
	if code != http.StatusBadRequest || out["error"] != "invalid_request" || msg == "" || out["access_token"] != nil {
		t.Errorf("oversized login: %d %v, want 400 invalid_request with a message", code, out)
	}
}

func TestCredentialsRequireOneBoundedJSONValue(t *testing.T) {
	valid := `{"username":"cyb","password":"secret1"}`
	for _, path := range []string{"/api/v1/auth/login", "/api/v1/auth/register"} {
		for name, tail := range map[string]string{
			"second object":        `{}`,
			"trailing junk":        `junk`,
			"oversized whitespace": strings.Repeat(" ", 4096),
		} {
			t.Run(path+"/"+name, func(t *testing.T) {
				accounts := &fakeAccounts{users: map[string]string{"cyb": "secret1"}}
				rec, out := postFull(t, Handler(accounts), path, valid+tail)
				if rec.Code != http.StatusBadRequest || out["error"] != "invalid_request" || out["access_token"] != nil {
					t.Errorf("%d %v, want 400 invalid_request and no token", rec.Code, out)
				}
			})
		}
	}
	// A complete document may still end with JSON whitespace.
	rec, _ := postFull(t, Handler(&fakeAccounts{users: map[string]string{"cyb": "secret1"}}), "/api/v1/auth/login", valid+" \r\n\t")
	if rec.Code != http.StatusOK {
		t.Errorf("JSON with trailing whitespace: %d, want 200", rec.Code)
	}
}

func TestHealth(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/health", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"ok"`) {
		t.Errorf("health: %d %q", rec.Code, rec.Body.String())
	}
}

type deleteReq struct {
	path, body, bearer, ifMatch string
}

func doDeleteAccount(t *testing.T, h http.Handler, d deleteReq) *httptest.ResponseRecorder {
	t.Helper()
	var req *http.Request
	if d.body != "" {
		req = httptest.NewRequest(http.MethodDelete, d.path, strings.NewReader(d.body))
		req.Header.Set("Content-Type", "application/json")
	} else {
		req = httptest.NewRequest(http.MethodDelete, d.path, nil)
	}
	if d.bearer != "" {
		req.Header.Set("Authorization", d.bearer)
	}
	if d.ifMatch != "" {
		req.Header.Set("If-Match", d.ifMatch)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func TestRegisterLoginDeleteAccountByToken(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})

	regRec, regOut := postFull(t, h, "/v1/register", `{"username":"temp_user","password":"secret1"}`)
	if regRec.Code != http.StatusOK || regOut["access_token"] != "aoc_test" {
		t.Fatalf("POST /v1/register: %d %v", regRec.Code, regOut)
	}
	if etag := regRec.Header().Get("ETag"); !strings.HasPrefix(etag, `"acct-`) {
		t.Fatalf("register ETag = %q, want strong ETag", etag)
	}

	loginRec, loginOut := postFull(t, h, "/v1/login", `{"username":"temp_user","password":"secret1"}`)
	if loginRec.Code != http.StatusOK || loginOut["access_token"] != "aoc_test" {
		t.Fatalf("POST /v1/login: %d %v", loginRec.Code, loginOut)
	}
	etag := loginRec.Header().Get("ETag")

	delRec := doDeleteAccount(t, h, deleteReq{path: "/v1/account", bearer: "Bearer aoc_test", ifMatch: etag})
	if delRec.Code != http.StatusOK || !strings.Contains(delRec.Body.String(), `"deleted":true`) {
		t.Fatalf("DELETE /v1/account (Bearer): %d %q", delRec.Code, delRec.Body.String())
	}

	code, out := post(t, h, "/v1/login", `{"username":"temp_user","password":"secret1"}`)
	if code != http.StatusUnauthorized || out["error"] != "invalid_credentials" {
		t.Fatalf("POST /v1/login after delete: %d %v, want 401 invalid_credentials", code, out)
	}
	delAgain := doDeleteAccount(t, h, deleteReq{path: "/v1/account", bearer: "Bearer aoc_test"})
	if delAgain.Code != http.StatusUnauthorized {
		t.Fatalf("second DELETE /v1/account: %d, want 401", delAgain.Code)
	}
}

func TestRegisterLoginDeleteAccountByCredentials(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})

	if code, _ := post(t, h, "/v1/register", `{"username":"cred_user","password":"secret1"}`); code != http.StatusOK {
		t.Fatalf("POST /v1/register: %d", code)
	}
	loginRec, _ := postFull(t, h, "/v1/login", `{"username":"cred_user","password":"secret1"}`)
	if loginRec.Code != http.StatusOK {
		t.Fatalf("POST /v1/login: %d", loginRec.Code)
	}

	delRec := doDeleteAccount(t, h, deleteReq{
		path:    "/api/v1/auth/account",
		body:    `{"username":"cred_user","password":"secret1"}`,
		ifMatch: `"*"`,
	})
	if delRec.Code != http.StatusOK || !strings.Contains(delRec.Body.String(), `"deleted":true`) {
		t.Fatalf("DELETE /api/v1/auth/account (JSON): %d %q", delRec.Code, delRec.Body.String())
	}

	if code, _ := post(t, h, "/v1/login", `{"username":"cred_user","password":"secret1"}`); code != http.StatusUnauthorized {
		t.Fatalf("POST /v1/login after credential delete: %d, want 401", code)
	}

	if code, _ := post(t, h, "/v1/register", `{"username":"nocontent_user","password":"secret1"}`); code != http.StatusOK {
		t.Fatalf("POST /v1/register nocontent_user: %d", code)
	}
	ncRec := doDeleteAccount(t, h, deleteReq{
		path: "/v1/account?no_content=1",
		body: `{"username":"nocontent_user","password":"secret1"}`,
	})
	if ncRec.Code != http.StatusNoContent || ncRec.Body.Len() != 0 {
		t.Fatalf("DELETE /v1/account?no_content=1: %d %q, want 204 empty", ncRec.Code, ncRec.Body.String())
	}
}

func TestDeleteAccountPreconditionAndAuthErrors(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})
	if code, _ := post(t, h, "/v1/register", `{"username":"guard_user","password":"secret1"}`); code != http.StatusOK {
		t.Fatalf("POST /v1/register: %d", code)
	}

	preRec := doDeleteAccount(t, h, deleteReq{path: "/v1/account", bearer: "Bearer aoc_test", ifMatch: `"wrong-etag"`})
	if preRec.Code != http.StatusPreconditionFailed || preRec.Header().Get("Content-Type") != "application/problem+json" {
		t.Fatalf("mismatched If-Match (Bearer): %d %q %q, want 412 application/problem+json",
			preRec.Code, preRec.Header().Get("Content-Type"), preRec.Body.String())
	}
	preCredRec := doDeleteAccount(t, h, deleteReq{
		path:    "/v1/account",
		body:    `{"username":"guard_user","password":"secret1"}`,
		ifMatch: `W/"weak-etag"`,
	})
	if preCredRec.Code != http.StatusPreconditionFailed || preCredRec.Header().Get("Content-Type") != "application/problem+json" {
		t.Fatalf("mismatched If-Match (JSON): %d, want 412", preCredRec.Code)
	}

	badTokRec := doDeleteAccount(t, h, deleteReq{path: "/v1/account", bearer: "Bearer aoc_invalid"})
	if badTokRec.Code != http.StatusUnauthorized {
		t.Fatalf("invalid bearer token: %d, want 401", badTokRec.Code)
	}
	badCredRec := doDeleteAccount(t, h, deleteReq{path: "/v1/account", body: `{"username":"guard_user","password":"wrong"}`})
	if badCredRec.Code != http.StatusUnauthorized {
		t.Fatalf("wrong password: %d, want 401", badCredRec.Code)
	}
	badJSONRec := doDeleteAccount(t, h, deleteReq{path: "/v1/account", body: `not json`})
	if badJSONRec.Code != http.StatusBadRequest {
		t.Fatalf("invalid JSON: %d, want 400", badJSONRec.Code)
	}

	if code, _ := post(t, h, "/v1/login", `{"username":"guard_user","password":"secret1"}`); code != http.StatusOK {
		t.Fatalf("guard_user should still exist after failed deletes, got %d", code)
	}
}
