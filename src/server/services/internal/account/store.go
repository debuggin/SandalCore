// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package account owns player accounts and the launcher's login tokens.
package account

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

var (
	ErrUsernameTaken      = errors.New("username taken")
	ErrInvalidCredentials = errors.New("invalid username or password")
	ErrInvalidToken       = errors.New("invalid or expired token")
	ErrAccountNotFound    = errors.New("account not found")
	ErrInvalidInput       = errors.New("invalid input")
)

type Account struct {
	ID       int64
	Username string
}

type Token struct {
	Value     string
	ExpiresAt time.Time
}

var usernamePattern = regexp.MustCompile(`^[A-Za-z0-9_.-]{3,32}$`)

// maxPasswordBytes is the longest password accepted (bcrypt's 72-byte limit, preserved for legacy hashes).
const maxPasswordBytes = 72

// dummyHash equalises Argon2id timing for unknown usernames; computed lazily and panics only if CSPRNG fails.
var dummyHash = sync.OnceValue(func() string {
	h, err := hashPassword("timing-equaliser")
	if err != nil {
		panic(err)
	}
	return h
})

func ValidateCredentials(username, password string) error {
	if !usernamePattern.MatchString(username) {
		return fmt.Errorf("%w: username must be 3-32 letters, digits, '.', '_' or '-'", ErrInvalidInput)
	}
	if len(password) < 4 || len(password) > maxPasswordBytes {
		return fmt.Errorf("%w: password must be 4-%d bytes", ErrInvalidInput, maxPasswordBytes)
	}
	return nil
}

type Store struct {
	pool  *pgxpool.Pool
	ttl   time.Duration
	now   func() time.Time
	check func(stored, password string) (match, upgrade bool, err error) // compares a password with a stored hash: checkPassword (a test counts the calls)
	mem   *memStore
}

func NewStore(pool *pgxpool.Pool, tokenTTL time.Duration) *Store {
	return &Store{
		pool:  pool,
		ttl:   tokenTTL,
		now:   time.Now,
		check: checkPassword,
		mem:   newMemStore(),
	}
}

var errInvalidToken = fmt.Errorf("%w: %w", ErrInvalidToken, ErrInvalidCredentials)

func trimBearerPrefix(raw string) string {
	tok := strings.TrimSpace(raw)
	if len(tok) >= 7 && strings.EqualFold(tok[:7], "Bearer ") {
		return strings.TrimSpace(tok[7:])
	}
	return tok
}

func (s *Store) TokenTTL() time.Duration { return s.ttl }

func (s *Store) Register(ctx context.Context, username, password string) (Account, error) {
	if err := ValidateCredentials(username, password); err != nil {
		return Account{}, err
	}
	hash, err := hashPassword(password)
	if err != nil {
		return Account{}, err
	}
	if s.pool == nil {
		return s.mem.register(username, hash)
	}
	var id int64
	err = s.pool.QueryRow(ctx,
		"INSERT INTO accounts (username, password_hash) VALUES ($1, $2) RETURNING id",
		username, hash).Scan(&id)
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && pgErr.Code == "23505" && pgErr.ConstraintName == "accounts_username_lower" {
		return Account{}, ErrUsernameTaken
	}
	if err != nil {
		return Account{}, err
	}
	return Account{ID: id, Username: username}, nil
}

// Authenticate Equalises unknown-user timing via dummyHash and upgrades legacy bcrypt hashes after sign-in.
func (s *Store) Authenticate(ctx context.Context, username, password string) (Account, error) {
	if !usernamePattern.MatchString(username) || len(password) > maxPasswordBytes {
		return Account{}, ErrInvalidCredentials
	}
	var acc Account
	var hash string
	var err error
	if s.pool == nil {
		acc, hash, err = s.mem.lookupByUsername(username)
	} else {
		err = s.pool.QueryRow(ctx,
			"SELECT id, username, password_hash FROM accounts WHERE lower(username) = lower($1)",
			username).Scan(&acc.ID, &acc.Username, &hash)
	}
	if errors.Is(err, pgx.ErrNoRows) {
		_, _, _ = s.check(dummyHash(), password)
		return Account{}, ErrInvalidCredentials
	}
	if err != nil {
		return Account{}, err
	}
	match, upgrade, err := s.check(hash, password)
	if err != nil {
		return Account{}, fmt.Errorf("account %d: password hash: %w", acc.ID, err)
	}
	if !match {
		return Account{}, ErrInvalidCredentials
	}
	if upgrade {
		s.upgradeHash(ctx, acc.ID, hash, password)
	}
	return acc, nil
}

// upgradeHash CAS-updates a matched legacy hash; failures are logged without failing the completed sign-in.
func (s *Store) upgradeHash(ctx context.Context, accountID int64, old, password string) {
	fresh, err := hashPassword(password)
	if err == nil {
		if s.pool == nil {
			s.mem.upgradeHash(accountID, old, fresh)
			return
		}
		_, err = s.pool.Exec(ctx, "UPDATE accounts SET password_hash = $1 WHERE id = $2 AND password_hash = $3", fresh, accountID, old)
	}
	if err != nil {
		slog.Warn("account: password hash not upgraded", "account_id", accountID, "error", err)
	}
}

func (s *Store) FindByUsername(ctx context.Context, username string) (Account, error) {
	if !usernamePattern.MatchString(username) {
		return Account{}, ErrAccountNotFound
	}
	if s.pool == nil {
		acc, _, err := s.mem.lookupByUsername(username)
		if errors.Is(err, pgx.ErrNoRows) {
			return Account{}, ErrAccountNotFound
		}
		return acc, err
	}
	var acc Account
	err := s.pool.QueryRow(ctx,
		"SELECT id, username FROM accounts WHERE lower(username) = lower($1)",
		username).Scan(&acc.ID, &acc.Username)
	if errors.Is(err, pgx.ErrNoRows) {
		return Account{}, ErrAccountNotFound
	}
	return acc, err
}

// IssueToken creates a login token "aoc_" + 64 hex digits; only its SHA-256
// is stored.
func (s *Store) IssueToken(ctx context.Context, accountID int64) (Token, error) {
	var raw [32]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return Token{}, err
	}
	tok := Token{Value: "aoc_" + hex.EncodeToString(raw[:]), ExpiresAt: s.now().Add(s.ttl)}
	sum := sha256.Sum256([]byte(tok.Value))
	if s.pool == nil {
		if err := s.mem.insertToken(sum, accountID, tok.ExpiresAt); err != nil {
			return Token{}, err
		}
		return tok, nil
	}
	if _, err := s.pool.Exec(ctx,
		"INSERT INTO login_tokens (token_hash, account_id, expires_at) VALUES ($1, $2, $3)",
		sum[:], accountID, tok.ExpiresAt); err != nil {
		return Token{}, err
	}
	return tok, nil
}

func (s *Store) ValidateToken(ctx context.Context, token string) (Account, error) {
	tok := trimBearerPrefix(token)
	if tok == "" {
		return Account{}, errInvalidToken
	}
	sum := sha256.Sum256([]byte(tok))
	if s.pool == nil {
		return s.mem.validateToken(sum, s.now())
	}
	var acc Account
	err := s.pool.QueryRow(ctx, `
		SELECT a.id, a.username FROM login_tokens t JOIN accounts a ON a.id = t.account_id
		WHERE t.token_hash = $1 AND t.expires_at > $2`, sum[:], s.now()).Scan(&acc.ID, &acc.Username)
	if errors.Is(err, pgx.ErrNoRows) {
		return Account{}, errInvalidToken
	}
	return acc, err
}

// Login authenticates a username and password.
func (s *Store) Login(ctx context.Context, username, password string) (Account, error) {
	return s.Authenticate(ctx, username, password)
}

// VerifyToken validates a raw or Bearer-prefixed login token.
func (s *Store) VerifyToken(ctx context.Context, rawToken string) (Account, error) {
	return s.ValidateToken(ctx, rawToken)
}

// DeleteAccount verifies username and password via Authenticate and deletes the
// account row (cascading to login_tokens, characters, and sessions).
func (s *Store) DeleteAccount(ctx context.Context, username, password string) error {
	acc, err := s.Authenticate(ctx, username, password)
	if err != nil {
		return err
	}
	if s.pool == nil {
		return s.mem.deleteAccount(acc.ID, ErrInvalidCredentials)
	}
	tag, err := s.pool.Exec(ctx, "DELETE FROM accounts WHERE id = $1", acc.ID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrInvalidCredentials
	}
	return nil
}

// DeleteAccountByToken validates rawToken via VerifyToken and deletes the
// owning account row (cascading to login_tokens, characters, and sessions).
func (s *Store) DeleteAccountByToken(ctx context.Context, rawToken string) error {
	acc, err := s.VerifyToken(ctx, rawToken)
	if err != nil {
		return err
	}
	if s.pool == nil {
		return s.mem.deleteAccount(acc.ID, errInvalidToken)
	}
	tag, err := s.pool.Exec(ctx, "DELETE FROM accounts WHERE id = $1", acc.ID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return errInvalidToken
	}
	return nil
}

// PurgeExpiredTokens deletes login tokens past their expiry (the ones
// ValidateToken already rejects) and returns how many it removed.
func (s *Store) PurgeExpiredTokens(ctx context.Context) (int64, error) {
	if s.pool == nil {
		return s.mem.purgeExpired(s.now()), nil
	}
	tag, err := s.pool.Exec(ctx, "DELETE FROM login_tokens WHERE expires_at <= $1", s.now())
	return tag.RowsAffected(), err
}

// PurgeStaleTemporaryAccounts deletes leftover temporary/test accounts
// (tmp_*, tst_*, temp_*, e2e_*, ephem_hero) and their cascading characters/tokens.
func (s *Store) PurgeStaleTemporaryAccounts(ctx context.Context) (int64, error) {
	if s.pool == nil {
		return s.mem.purgeTemporaryAccounts(), nil
	}
	const qChars = `DELETE FROM characters
		WHERE lower(name) LIKE 'tst\_%' ESCAPE '\'
		   OR lower(name) LIKE 'tmp\_%' ESCAPE '\'
		   OR lower(name) LIKE 'temp\_%' ESCAPE '\'
		   OR lower(name) LIKE 'e2e\_%' ESCAPE '\'
		   OR name ~ '^TST[A-Z][a-z]+[A-Z][a-z]+$'`
	_, _ = s.pool.Exec(ctx, qChars)
	const qAccounts = `DELETE FROM accounts
		WHERE lower(username) LIKE 'tmp\_%' ESCAPE '\'
		   OR lower(username) LIKE 'tst\_%' ESCAPE '\'
		   OR lower(username) LIKE 'temp\_%' ESCAPE '\'
		   OR lower(username) LIKE 'e2e\_%' ESCAPE '\'
		   OR lower(username) = 'ephem_hero'`
	tag, err := s.pool.Exec(ctx, qAccounts)
	return tag.RowsAffected(), err
}

func isTemporaryUsername(username string) bool {
	u := strings.ToLower(strings.TrimSpace(username))
	return strings.HasPrefix(u, "tmp_") ||
		strings.HasPrefix(u, "tst_") ||
		strings.HasPrefix(u, "temp_") ||
		strings.HasPrefix(u, "e2e_") ||
		u == "ephem_hero"
}

type memAccount struct {
	Account
	hash string
}

type memToken struct {
	accountID int64
	expiresAt time.Time
}

type memStore struct {
	mu     sync.Mutex
	nextID int64
	users  map[int64]memAccount
	tokens map[[32]byte]memToken
}

func newMemStore() *memStore {
	return &memStore{
		users:  make(map[int64]memAccount),
		tokens: make(map[[32]byte]memToken),
	}
}

func (m *memStore) register(username, hash string) (Account, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, u := range m.users {
		if strings.EqualFold(u.Username, username) {
			return Account{}, ErrUsernameTaken
		}
	}
	m.nextID++
	acc := Account{ID: m.nextID, Username: username}
	m.users[acc.ID] = memAccount{Account: acc, hash: hash}
	return acc, nil
}

func (m *memStore) lookupByUsername(username string) (Account, string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, u := range m.users {
		if strings.EqualFold(u.Username, username) {
			return u.Account, u.hash, nil
		}
	}
	return Account{}, "", pgx.ErrNoRows
}

func (m *memStore) upgradeHash(accountID int64, old, fresh string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if u, ok := m.users[accountID]; ok && u.hash == old {
		u.hash = fresh
		m.users[accountID] = u
	}
}

func (m *memStore) insertToken(sum [32]byte, accountID int64, expiresAt time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.users[accountID]; !ok {
		return ErrAccountNotFound
	}
	m.tokens[sum] = memToken{accountID: accountID, expiresAt: expiresAt}
	return nil
}

func (m *memStore) validateToken(sum [32]byte, now time.Time) (Account, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	tok, ok := m.tokens[sum]
	if !ok || !tok.expiresAt.After(now) {
		return Account{}, errInvalidToken
	}
	u, ok := m.users[tok.accountID]
	if !ok {
		return Account{}, errInvalidToken
	}
	return u.Account, nil
}

func (m *memStore) deleteAccount(accountID int64, notFoundErr error) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.users[accountID]; !ok {
		return notFoundErr
	}
	delete(m.users, accountID)
	for sum, tok := range m.tokens {
		if tok.accountID == accountID {
			delete(m.tokens, sum)
		}
	}
	return nil
}

func (m *memStore) purgeExpired(now time.Time) int64 {
	m.mu.Lock()
	defer m.mu.Unlock()
	var n int64
	for sum, tok := range m.tokens {
		if !tok.expiresAt.After(now) {
			delete(m.tokens, sum)
			n++
		}
	}
	return n
}

func (m *memStore) purgeTemporaryAccounts() int64 {
	m.mu.Lock()
	defer m.mu.Unlock()
	var n int64
	for id, u := range m.users {
		if isTemporaryUsername(u.Username) {
			delete(m.users, id)
			for sum, tok := range m.tokens {
				if tok.accountID == id {
					delete(m.tokens, sum)
				}
			}
			n++
		}
	}
	return n
}
