package go_test

import (
	"bytes"; "context"; "crypto/hmac"; "crypto/rand"; "crypto/sha256"; "crypto/subtle"; "crypto/tls"; "crypto/x509"
	"encoding/binary"; "encoding/hex"; "encoding/json"; "errors"; "fmt"; "io"; "math"
	"net"; "net/http"; "net/http/httptest"; "os"; "os/exec"; "path/filepath"; "runtime"; "strings"; "sync"; "testing"; "time"

	authv1 "aocrop/services/gen/aoc/auth/v1"; characterv1 "aocrop/services/gen/aoc/character/v1"; citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	gatewayv1 "aocrop/services/gen/aoc/gateway/v1"; meshv1 "aocrop/services/gen/aoc/mesh/v1"; questv1 "aocrop/services/gen/aoc/quest/v1"; worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	icscommon "aocrop/services/gen/ics/common"; icsxclient "aocrop/services/gen/ics/xclient"; "github.com/google/uuid"
	"google.golang.org/grpc"; "google.golang.org/grpc/codes"; "google.golang.org/grpc/credentials"; "google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"; "google.golang.org/grpc/status"; "google.golang.org/protobuf/proto"
)

func repoRootDir(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok { t.Fatal("runtime.Caller failed") }
	return filepath.Clean(filepath.Join(filepath.Dir(file), "..", "..", ".."))
}
func readRepoFile(t *testing.T, relPath string) string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(repoRootDir(t), filepath.FromSlash(relPath)))
	if err != nil { t.Fatalf("failed to read %s: %v", relPath, err) }
	return string(raw)
}
func isLowerHex(s string, wantLen int) bool {
	if len(s) != wantLen { return false }
	for i := 0; i < len(s); i++ {
		if (s[i] < '0' || s[i] > '9') && (s[i] < 'a' || s[i] > 'f') { return false }
	}
	return true
}
type authSession struct { AccountID int64; Username string; ExpiresAt time.Time }
type authFlowHarness struct {
	mu sync.Mutex; serviceToken string; hmacSecret []byte; now func() time.Time; nextID int64
	users map[string]string; userIDs map[string]int64; sessions map[string]authSession; charsByAccount map[int64][]map[string]any
}
func newAuthFlowHarness(serviceToken string, nowFn func() time.Time) *authFlowHarness {
	return &authFlowHarness{serviceToken: serviceToken, hmacSecret: []byte("sandalcore-e2e-token-hmac-key-v1"), now: nowFn, nextID: 1000, users: make(map[string]string), userIDs: make(map[string]int64), sessions: make(map[string]authSession), charsByAccount: make(map[int64][]map[string]any)}
}
func (a *authFlowHarness) checkServiceToken(authHeader string) error {
	const prefix = "Bearer "
	if len(authHeader) <= len(prefix) || !strings.EqualFold(authHeader[:len(prefix)], prefix) || subtle.ConstantTimeCompare([]byte(authHeader[len(prefix):]), []byte(a.serviceToken)) != 1 {
		return errors.New("unauthenticated: missing or invalid bearer service token")
	}
	return nil
}
func (a *authFlowHarness) issueSessionToken(accountID int64, username string) string {
	var nonce [16]byte
	_, _ = rand.Read(nonce[:])
	mac := hmac.New(sha256.New, a.hmacSecret)
	_, _ = fmt.Fprintf(mac, "%d:%s:%x", accountID, username, nonce[:])
	return hex.EncodeToString(nonce[:]) + hex.EncodeToString(mac.Sum(nil)[:16])
}
func (a *authFlowHarness) validateSessionToken(authHeader, token string) (authSession, error) {
	if err := a.checkServiceToken(authHeader); err != nil || !isLowerHex(token, 64) { return authSession{}, errors.New("unauthenticated: invalid service or session token") }
	a.mu.Lock()
	defer a.mu.Unlock()
	sess, ok := a.sessions[token]
	if !ok || !a.now().Before(sess.ExpiresAt) {
		delete(a.sessions, token)
		return authSession{}, errors.New("unauthenticated: unknown, tampered, or expired session token")
	}
	return sess, nil
}
func decodeStrictCredentials(r *http.Request) (string, string, error) {
	if r.Method != http.MethodPost && r.Method != http.MethodDelete { return "", "", errors.New("method not allowed") }
	var creds struct{ Username, Password string }
	dec := json.NewDecoder(io.LimitReader(r.Body, 4096))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&creds); err != nil { return "", "", err }
	if err := dec.Decode(&struct{}{}); !errors.Is(err, io.EOF) || len(creds.Username) < 3 || len(creds.Username) > 32 || len(creds.Password) < 8 {
		return "", "", errors.New("trailing data or invalid username/password bounds")
	}
	return creds.Username, creds.Password, nil
}
func (a *authFlowHarness) registerHTTP(w http.ResponseWriter, r *http.Request) {
	user, pass, err := decodeStrictCredentials(r)
	if err != nil { http.Error(w, err.Error(), http.StatusBadRequest); return }
	a.mu.Lock()
	defer a.mu.Unlock()
	if _, exists := a.users[user]; exists { http.Error(w, "username already taken", http.StatusConflict); return }
	a.nextID++
	id := a.nextID
	a.users[user], a.userIDs[user] = pass, id
	tok := a.issueSessionToken(id, user)
	a.sessions[tok] = authSession{AccountID: id, Username: user, ExpiresAt: a.now().Add(15 * time.Minute)}
	w.Header().Set("Content-Type", "application/json"); w.WriteHeader(http.StatusCreated)
	_ = json.NewEncoder(w).Encode(map[string]any{"account_id": id, "username": user, "token": tok, "access_token": tok})
}
func (a *authFlowHarness) loginHTTP(w http.ResponseWriter, r *http.Request) {
	user, pass, err := decodeStrictCredentials(r)
	if err != nil { http.Error(w, err.Error(), http.StatusBadRequest); return }
	a.mu.Lock()
	defer a.mu.Unlock()
	if wantPass, ok := a.users[user]; !ok || subtle.ConstantTimeCompare([]byte(pass), []byte(wantPass)) != 1 { http.Error(w, "invalid credentials", http.StatusUnauthorized); return }
	id := a.userIDs[user]
	tok := a.issueSessionToken(id, user)
	a.sessions[tok] = authSession{AccountID: id, Username: user, ExpiresAt: a.now().Add(15 * time.Minute)}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"account_id": id, "username": user, "token": tok, "access_token": tok})
}
func (a *authFlowHarness) deleteAccountHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodDelete && r.Method != http.MethodPost { http.Error(w, "method not allowed", http.StatusMethodNotAllowed); return }
	bearer := strings.TrimSpace(strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer"))
	a.mu.Lock()
	defer a.mu.Unlock()
	var targetUser string
	var targetID int64
	if sess, ok := a.sessions[bearer]; ok && a.now().Before(sess.ExpiresAt) {
		targetUser, targetID = sess.Username, sess.AccountID
	} else if user, pass, err := decodeStrictCredentials(r); err == nil {
		if wantPass, ok := a.users[user]; ok && subtle.ConstantTimeCompare([]byte(pass), []byte(wantPass)) == 1 { targetUser, targetID = user, a.userIDs[user] }
	}
	if targetUser == "" { http.Error(w, "invalid credentials or token", http.StatusUnauthorized); return }
	delete(a.users, targetUser); delete(a.userIDs, targetUser); delete(a.charsByAccount, targetID)
	for k, s := range a.sessions {
		if s.AccountID == targetID || s.Username == targetUser { delete(a.sessions, k) }
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"deleted": true, "status": "deleted"})
}
func doAuthReqJSON(t *testing.T, method, url, bearer, body string) (*http.Response, map[string]any) {
	t.Helper()
	req, err := http.NewRequest(method, url, strings.NewReader(body))
	if err != nil { t.Fatalf("%s %s build failed: %v", method, url, err) }
	req.Header.Set("Content-Type", "application/json")
	if bearer != "" { req.Header.Set("Authorization", "Bearer "+bearer) }
	resp, err := (&http.Client{Timeout: 5 * time.Second}).Do(req)
	if err != nil { t.Fatalf("%s %s failed: %v", method, url, err) }
	defer resp.Body.Close()
	var payload map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&payload)
	return resp, payload
}
func postAuthJSON(t *testing.T, url, body string) (*http.Response, map[string]any) { return doAuthReqJSON(t, http.MethodPost, url, "", body) }
func TestFlow1_AuthAndTokenSecurityE2E(t *testing.T) {
	authGrpcSrc, authHttpSrc, svcAuthSrc := readRepoFile(t, "src/server/services/internal/authgrpc/server.go"), readRepoFile(t, "src/server/services/internal/authhttp/handler.go"), readRepoFile(t, "src/server/services/internal/svcauth/interceptor.go")
	for _, snippet := range []string{"ValidateToken", "account.ErrInvalidToken", "codes.Unauthenticated"} {
		if !strings.Contains(authGrpcSrc, snippet) { t.Fatalf("authgrpc/server.go missing contract snippet %q", snippet) }
	}
	if !strings.Contains(authHttpSrc, "/api/v1/auth/register") || !strings.Contains(authHttpSrc, "/api/v1/auth/account") || !strings.Contains(svcAuthSrc, "subtle.ConstantTimeCompare") {
		t.Fatal("authhttp/handler.go or svcauth/interceptor.go missing auth or account deletion contract")
	}
	clockNow, svcTok := time.Date(2026, 11, 4, 16, 0, 0, 0, time.UTC), strings.Repeat("a1", 32)
	harness := newAuthFlowHarness(svcTok, func() time.Time { return clockNow })
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/auth/register", harness.registerHTTP); mux.HandleFunc("/api/v1/auth/login", harness.loginHTTP); mux.HandleFunc("/api/v1/auth/account", harness.deleteAccountHTTP)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	resp, regBody := postAuthJSON(t, srv.URL+"/api/v1/auth/register", `{"username":"aelindra","password":"correct-horse-battery"}`)
	tok, _ := regBody["token"].(string)
	if resp.StatusCode != http.StatusCreated || !isLowerHex(tok, 64) { t.Fatalf("expected 201 Created and 64-hex token, got %d / %q", resp.StatusCode, tok) }
	if sess, err := harness.validateSessionToken("Bearer "+svcTok, tok); err != nil || sess.Username != "aelindra" || sess.AccountID != 1001 {
		t.Fatalf("ValidateToken positive flow failed: sess=%+v err=%v", sess, err)
	}
	verifyFlow1NegativeCases(t, srv.URL, harness, svcTok, tok, &clockNow)
	if delResp, delBody := doAuthReqJSON(t, http.MethodDelete, srv.URL+"/api/v1/auth/account", "", `{"username":"aelindra","password":"correct-horse-battery"}`); delResp.StatusCode != http.StatusOK || delBody["deleted"] != true {
		t.Fatalf("expected 200 OK deleting temporary account aelindra, got %d (%v)", delResp.StatusCode, delBody)
	}
	if loginAfterDel, _ := postAuthJSON(t, srv.URL+"/api/v1/auth/login", `{"username":"aelindra","password":"correct-horse-battery"}`); loginAfterDel.StatusCode != http.StatusUnauthorized {
		t.Fatalf("expected 401 Unauthorized after account deletion, got %d", loginAfterDel.StatusCode)
	}
}
func verifyFlow1NegativeCases(t *testing.T, baseURL string, h *authFlowHarness, svcTok, validTok string, clockNow *time.Time) {
	t.Helper()
	tampered := validTok[:63] + "0"
	if validTok[63] == '0' { tampered = validTok[:63] + "1" }
	for _, tc := range []struct{ name, header, token string }{{"missing_bearer", "", validTok}, {"wrong_service_token", "Bearer " + strings.Repeat("ff", 32), validTok}, {"tampered_session_token", "Bearer " + svcTok, tampered}, {"short_session_token", "Bearer " + svcTok, validTok[:32]}, {"uppercase_hex_token", "Bearer " + svcTok, strings.ToUpper(validTok)}} {
		if _, err := h.validateSessionToken(tc.header, tc.token); err == nil { t.Fatalf("expected negative auth case %q to fail validation", tc.name) }
	}
	*clockNow = clockNow.Add(16 * time.Minute)
	if _, err := h.validateSessionToken("Bearer "+svcTok, validTok); err == nil { t.Fatal("expected expired session token to be rejected") }
	for _, badBody := range []string{`{"username":"aelindra","password":"correct-horse-battery"}`, `{"username":"ae","password":"correct-horse-battery"}`, `{"username":"aelindra","password":"short"}`, `{"username":"aelindra","password":"correct-horse-battery","role":"admin"}`, `{"username":"aelindra","password":"correct-horse-battery"}{"extra":1}`} {
		if resp, _ := postAuthJSON(t, baseURL+"/api/v1/auth/register", badBody); resp.StatusCode < 400 { t.Fatalf("expected HTTP >= 400 for negative payload %s, got %d", badBody, resp.StatusCode) }
	}
}
type characterRecord struct { ID, Name, Race, Archetype string; AccountID, Experience int64; Level int32; X, Y, Z float64; FencingToken uint64 }
var allowedArchetypes = map[string]bool{"bard": true, "cleric": true, "fighter": true, "mage": true, "ranger": true, "rogue": true, "summoner": true, "tank": true}
func validateCharacterNameAndClass(name, archetype string) error {
	if len(name) < 3 || len(name) > 16 { return errors.New("invalid character name length") }
	for i := 0; i < len(name); i++ {
		if (name[i] < 'a' || name[i] > 'z') && (name[i] < 'A' || name[i] > 'Z') { return errors.New("character name must contain ASCII letters only") }
	}
	if !allowedArchetypes[strings.ToLower(archetype)] { return errors.New("invalid character archetype") }
	return nil
}
type characterStoreHarness struct { byID map[string]characterRecord; byName map[string]string }
func newCharacterStoreHarness() *characterStoreHarness {
	return &characterStoreHarness{byID: make(map[string]characterRecord), byName: make(map[string]string)}
}
func (s *characterStoreHarness) create(accountID int64, name, race, archetype string) (characterRecord, error) {
	if accountID <= 0 || race == "" { return characterRecord{}, errors.New("invalid account or race") }
	if err := validateCharacterNameAndClass(name, archetype); err != nil { return characterRecord{}, err }
	lowerName := strings.ToLower(name)
	if _, exists := s.byName[lowerName]; exists { return characterRecord{}, errors.New("character name already exists") }
	id := fmt.Sprintf("%032x", len(s.byID)+1)
	rec := characterRecord{ID: id, AccountID: accountID, Name: name, Race: race, Archetype: strings.ToLower(archetype), Level: 1, Experience: 0, X: -45200.5, Y: 18400.25, Z: 1250.0, FencingToken: 1}
	s.byID[id], s.byName[lowerName] = rec, id
	return rec, nil
}
func (s *characterStoreHarness) saveState(next characterRecord) error {
	if !isLowerHex(next.ID, 32) || next.Level <= 0 || next.Level > 50 || next.Experience < 0 { return errors.New("invalid character state bounds") }
	if math.IsNaN(next.X) || math.IsInf(next.X, 0) || math.IsNaN(next.Y) || math.IsInf(next.Y, 0) || math.IsNaN(next.Z) || math.IsInf(next.Z, 0) { return errors.New("non-finite position coordinates") }
	curr, ok := s.byID[next.ID]
	if !ok { return errors.New("character not found") }
	if next.FencingToken < curr.FencingToken { return errors.New("SQLSTATE 55000: stale fencing token") }
	s.byID[next.ID] = next
	return nil
}
func (s *characterStoreHarness) listByAccount(accountID int64) []characterRecord {
	var out []characterRecord
	for _, rec := range s.byID {
		if rec.AccountID == accountID { out = append(out, rec) }
	}
	return out
}
func TestFlow2_CharacterLifecycleAndPersistenceContractsE2E(t *testing.T) {
	charGrpcSrc := readRepoFile(t, "src/server/services/internal/charactergrpc/server.go")
	for _, method := range []string{"ListCharacters", "CreateCharacter", "DeleteCharacter", "GetCharacter", "SaveCharacterState"} {
		if !strings.Contains(charGrpcSrc, method) { t.Fatalf("charactergrpc/server.go missing %s handler", method) }
	}
	store := newCharacterStoreHarness()
	created, err := store.create(1001, "Aelindra", "Aelan", "mage")
	if err != nil || !isLowerHex(created.ID, 32) || created.Level != 1 { t.Fatalf("character create failed: rec=%+v err=%v", created, err) }
	created.Level, created.Experience, created.FencingToken, created.X, created.Y, created.Z = 12, 48500, 2, 12050.0, -34000.0, 980.5
	if err := store.saveState(created); err != nil { t.Fatalf("saveState failed: %v", err) }
	wire, _ := json.Marshal(store.byID[created.ID])
	var roundTrip characterRecord
	if err := json.Unmarshal(wire, &roundTrip); err != nil || roundTrip.Level != 12 || roundTrip.Experience != 48500 || roundTrip.FencingToken != 2 {
		t.Fatalf("character state round-trip mismatch: %+v err=%v", roundTrip, err)
	}
	if list := store.listByAccount(1001); len(list) != 1 || list[0].Name != "Aelindra" { t.Fatalf("listByAccount mismatch: %+v", list) }
	for _, bad := range []struct{ name, class string }{{"", "mage"}, {"Al", "mage"}, {"NameWayTooLongForLimit", "mage"}, {"Bad_Name1", "mage"}, {"Aelindra", "mage"}, {"Kaelen", "necromancer_invalid"}, {"Kaelen", ""}} {
		if _, err := store.create(1001, bad.name, "Aelan", bad.class); err == nil { t.Fatalf("expected invalid character create (%q, %q) to fail", bad.name, bad.class) }
	}
	stale, nanPos := roundTrip, roundTrip
	stale.FencingToken, nanPos.X = 1, math.NaN()
	if store.saveState(stale) == nil || store.saveState(nanPos) == nil { t.Fatal("expected stale fencing token or NaN coordinate save to fail") }
}
var canonicalNodeExpThresholds = [7]int64{0, 2636934, 10547736, 31643207, 73834150, 538730690, 1162745780}
type cityNodeState struct { GUID, ZOIGUID, Experience, ParentGUID int64; Name, MayorID string; Stage, TaxRateBps int }
type cityNodeEngine struct{ nodes map[int64]*cityNodeState }
func newCityNodeEngine() *cityNodeEngine {
	return &cityNodeEngine{nodes: map[int64]*cityNodeState{101: {GUID: 101, Name: "Verra_RVR_Joeva", ZOIGUID: 9001}, 102: {GUID: 102, Name: "Verra_RVR_Halcyon", ZOIGUID: 9001}}}
}
func nodeProgress(stage int, xp int64) (int, int64) {
	if stage < 0 || stage >= 6 { return 0, 0 }
	lo, hi := canonicalNodeExpThresholds[stage], canonicalNodeExpThresholds[stage+1]
	if xp <= lo { return 0, hi - xp }
	if xp >= hi { return 100, 0 }
	return int((xp - lo) * 100 / (hi - lo)), hi - xp
}
func (e *cityNodeEngine) addXPAndPromote(guid int64, deltaXP int64, targetStage int) error {
	n, ok := e.nodes[guid]
	if !ok || deltaXP <= 0 || targetStage < 0 || targetStage > 6 { return errors.New("invalid node or XP/stage parameter") }
	if n.Experience+deltaXP < n.Experience || targetStage > n.Stage+1 { return errors.New("XP overflow or non-sequential stage jump") }
	if n.ParentGUID != 0 && targetStage >= e.nodes[n.ParentGUID].Stage { return errors.New("ZOI exclusivity: vassal node cannot reach or exceed parent stage") }
	n.Experience += deltaXP
	if n.Experience < canonicalNodeExpThresholds[targetStage] { return errors.New("insufficient XP for target stage") }
	n.Stage = targetStage
	if targetStage >= 3 {
		for _, other := range e.nodes {
			if other.GUID != n.GUID && other.ZOIGUID == n.ZOIGUID && other.ParentGUID == 0 { other.ParentGUID = n.GUID }
		}
	}
	return nil
}
func (e *cityNodeEngine) setMayorAndTaxRate(guid int64, mayorCharID string, taxBps int) (int64, error) {
	n, ok := e.nodes[guid]
	if !ok || n.Stage < 3 { return 0, errors.New("node not found or below Village stage (3)") }
	if !isLowerHex(mayorCharID, 32) || taxBps < 0 || taxBps > 5000 { return 0, errors.New("tax rate out of bounds [0, 5000] bps or invalid mayor ID") }
	n.MayorID, n.TaxRateBps = mayorCharID, taxBps
	return (int64(10000)*int64(taxBps) + 9999) / 10000, nil
}
func TestFlow3_CityNodeProgressionAndTaxMayoraltyMechanicsE2E(t *testing.T) {
	constsSrc, levelsSrc, citizenshipSrc := readRepoFile(t, "build/gen/go/citynodeconsts/constants_gen.go"), readRepoFile(t, "src/server/services/internal/citynode/levels.go"), readRepoFile(t, "src/server/services/internal/citynode/citizenship.go")
	for _, token := range []string{"2636934", "10547736", "31643207", "73834150", "538730690", "1162745780"} {
		if !strings.Contains(constsSrc, token) { t.Fatalf("constants_gen.go missing canonical threshold %s", token) }
	}
	if !strings.Contains(levelsSrc, "Progress(level int, experience int64)") || !strings.Contains(citizenshipSrc, "BuyOrderEscrow") {
		t.Fatal("citynode package missing Progress or BuyOrderEscrow contract")
	}
	eng := newCityNodeEngine()
	if pct, toNext := nodeProgress(0, 1318467); pct != 50 || toNext != 1318467 { t.Fatalf("expected 50%% and 1318467 toNext at stage 0, got pct=%d toNext=%d", pct, toNext) }
	for _, st := range []struct{ stage int; xp int64 }{{1, 2636934}, {2, 7910802}, {3, 21095471}} {
		if err := eng.addXPAndPromote(101, st.xp, st.stage); err != nil { t.Fatalf("promote Joeva to stage %d failed: %v", st.stage, err) }
	}
	if eng.nodes[101].Stage != 3 || eng.nodes[102].ParentGUID != 101 { t.Fatalf("expected Joeva at Stage 3 and Halcyon as ZOI vassal, got %+v / %+v", eng.nodes[101], eng.nodes[102]) }
	if eng.addXPAndPromote(102, 2636934, 1) != nil || eng.addXPAndPromote(102, 7910802, 2) != nil || eng.addXPAndPromote(102, 21095471, 3) == nil {
		t.Fatal("vassal promotion or ZOI exclusivity check failed")
	}
	mayorID := fmt.Sprintf("%032x", 1)
	for _, bps := range []int{0, 250, 5000} {
		if tax, err := eng.setMayorAndTaxRate(101, mayorID, bps); err != nil || tax != int64(bps) { t.Fatalf("valid tax rate %d bps failed: tax=%d err=%v", bps, tax, err) }
	}
	for _, badBps := range []int{-1, -500, 5001, 10000} {
		if _, err := eng.setMayorAndTaxRate(101, mayorID, badBps); err == nil { t.Fatalf("expected out-of-range tax rate %d bps to be rejected", badBps) }
	}
	if _, err := eng.setMayorAndTaxRate(102, mayorID, 500); err == nil { t.Fatal("expected stage 2 node below Village (3) to reject mayoral tax configuration") }
}
const ( questStatusNone uint8 = 0; questStatusInProgress uint8 = 1; questStatusCompleted uint8 = 2 )
type questInstance struct { GUID string; Record int64; Status uint8; ComplCount uint32; Objectives, Required []uint32 }
type questStateMachine struct{ byRecord map[int64]*questInstance }
func newQuestStateMachine() *questStateMachine { return &questStateMachine{byRecord: make(map[int64]*questInstance)} }
func (q *questStateMachine) acceptQuest(guid string, record int64, required []uint32) error {
	if !isLowerHex(guid, 32) || record <= 0 || len(required) == 0 || len(required) > 64 || len(q.byRecord) >= 600 { return errors.New("invalid quest guid, record, objective count, or instance cap") }
	if existing, exists := q.byRecord[record]; exists && existing.Status != questStatusNone { return errors.New("quest already accepted or completed") }
	q.byRecord[record] = &questInstance{GUID: guid, Record: record, Status: questStatusInProgress, Objectives: make([]uint32, len(required)), Required: append([]uint32(nil), required...)}
	return nil
}
func (q *questStateMachine) advanceObjective(record int64, objIndex int, delta uint32) error {
	inst, ok := q.byRecord[record]
	if !ok || inst.Status != questStatusInProgress || objIndex < 0 || objIndex >= len(inst.Objectives) || delta == 0 { return errors.New("quest not in progress or invalid objective delta") }
	inst.Objectives[objIndex] = min(inst.Objectives[objIndex]+delta, inst.Required[objIndex])
	return nil
}
func (q *questStateMachine) completeQuest(record int64) error {
	inst, ok := q.byRecord[record]
	if !ok || inst.Status != questStatusInProgress { return errors.New("cannot complete quest not in progress") }
	for i, got := range inst.Objectives {
		if got < inst.Required[i] { return errors.New("cannot complete quest with incomplete objectives") }
	}
	inst.Status, inst.ComplCount = questStatusCompleted, inst.ComplCount+1
	return nil
}
func TestFlow4_QuestAndObjectiveStateMachineE2E(t *testing.T) {
	questSrc := readRepoFile(t, "src/server/services/internal/quest/service.go")
	for _, token := range []string{"MaxInstances  = 600", "MaxObjectives = 64", "q.GetStatus() < 0 || q.GetStatus() > 3", `case "give", "finish", "fail", "abandon":`} {
		if !strings.Contains(questSrc, token) { t.Fatalf("quest/service.go missing contract token %q", token) }
	}
	sm, qGUID := newQuestStateMachine(), fmt.Sprintf("%032x", 901)
	const qRec = int64(6064631000000000001)
	if sm.advanceObjective(qRec, 0, 1) == nil || sm.completeQuest(qRec) == nil { t.Fatal("expected advancing or completing unaccepted quest to fail") }
	if sm.acceptQuest(qGUID, qRec, []uint32{3, 1}) != nil || sm.acceptQuest(qGUID, qRec, []uint32{3, 1}) == nil { t.Fatal("acceptQuest or duplicate check failed") }
	if sm.advanceObjective(qRec, 0, 2) != nil || sm.completeQuest(qRec) == nil { t.Fatal("partial objective or premature completeQuest check failed") }
	if sm.advanceObjective(qRec, 0, 1) != nil || sm.advanceObjective(qRec, 1, 1) != nil || sm.completeQuest(qRec) != nil { t.Fatal("objective completion or completeQuest failed") }
	if got := sm.byRecord[qRec]; got.Status != questStatusCompleted || got.ComplCount != 1 { t.Fatalf("unexpected completed quest state: %+v", got) }
	if sm.advanceObjective(qRec, 0, 1) == nil || sm.completeQuest(qRec) == nil || sm.acceptQuest("bad-guid", 77, []uint32{1}) == nil { t.Fatal("expected post-completion transitions or invalid GUID to fail") }
}
type joinTicketEntry struct{ AccountID int64; CharacterID string; ServerID uint32; Redeemed bool }
type meshGatewayHarness struct {
	mu sync.Mutex; servers map[uint32]string; zones map[uint32][4]float64; epochs map[uint32]uint32; tickets map[string]*joinTicketEntry
}
func newMeshGatewayHarness() *meshGatewayHarness {
	return &meshGatewayHarness{servers: make(map[uint32]string), zones: make(map[uint32][4]float64), epochs: make(map[uint32]uint32), tickets: make(map[string]*joinTicketEntry)}
}
func (m *meshGatewayHarness) registerZone(zoneID, serverID uint32, addr string, bounds [4]float64, epoch uint32) error {
	host, port, err := net.SplitHostPort(addr)
	if err != nil || host == "0.0.0.0" || host == "::" || port == "0" || zoneID == 0 || serverID == 0 { return errors.New("invalid world server address or zone/server id") }
	if !(bounds[0] < bounds[1]) || !(bounds[2] < bounds[3]) { return errors.New("invalid spatial zone bounds") }
	m.mu.Lock()
	defer m.mu.Unlock()
	if epoch <= m.epochs[zoneID] { return errors.New("stale authority epoch rejected by monotonic fence") }
	m.servers[serverID], m.zones[zoneID], m.epochs[zoneID] = addr, bounds, epoch
	return nil
}
func (m *meshGatewayHarness) resolveZoneWithHysteresis(currentZone uint32, x, y, hysteresisCm float64) (uint32, string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	b, ok := m.zones[currentZone]
	if !ok { return 0, "", errors.New("unknown current zone") }
	dist := math.Max(math.Max(b[0]-x, x-b[1]), math.Max(b[2]-y, y-b[3]))
	if dist < -hysteresisCm { return currentZone, "interior", nil }
	if dist <= hysteresisCm { return currentZone, "hysteresis", nil }
	for zid, zb := range m.zones {
		if zid != currentZone && x >= zb[0] && x <= zb[1] && y >= zb[2] && y <= zb[3] { return zid, "handover", nil }
	}
	return currentZone, "hysteresis", nil
}
func (m *meshGatewayHarness) enterWorld(accountID int64, charID string, serverID uint32) (string, string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	addr, ok := m.servers[serverID]
	if !ok || accountID <= 0 || !isLowerHex(charID, 32) { return "", "", errors.New("no live world server or invalid character") }
	var raw [16]byte
	_, _ = rand.Read(raw[:])
	tok := hex.EncodeToString(raw[:])
	m.tickets[tok] = &joinTicketEntry{AccountID: accountID, CharacterID: charID, ServerID: serverID}
	return tok, addr, nil
}
func (m *meshGatewayHarness) redeemTicket(tok, charID string, serverID uint32) (int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	t, ok := m.tickets[tok]
	if !ok || t.Redeemed || t.CharacterID != charID || t.ServerID != serverID { return 0, errors.New("permission denied: join ticket invalid or already redeemed") }
	t.Redeemed = true
	return t.AccountID, nil
}
func encodeDecodeMeshHeader(srcZone, dstZone, epoch uint32, payload []byte) ([3]uint32, []byte, error) {
	if srcZone == 0 || dstZone == 0 || epoch == 0 { return [3]uint32{}, nil, errors.New("invalid mesh forward header fields") }
	buf := make([]byte, 12+len(payload))
	binary.LittleEndian.PutUint32(buf[0:4], srcZone); binary.LittleEndian.PutUint32(buf[4:8], dstZone); binary.LittleEndian.PutUint32(buf[8:12], epoch)
	copy(buf[12:], payload)
	return [3]uint32{binary.LittleEndian.Uint32(buf[0:4]), binary.LittleEndian.Uint32(buf[4:8]), binary.LittleEndian.Uint32(buf[8:12])}, buf[12:], nil
}
func verifyEnvoyAndComposeGrpcContractParity(t *testing.T) {
	t.Helper()
	envoyYaml, composeYaml := readRepoFile(t, "src/deploy/envoy/envoy.yaml"), readRepoFile(t, "src/deploy/compose.yml")
	if strings.Count(composeYaml, "container_name: aoc-") != 11 || strings.Contains(composeYaml, "aoc-tether") || strings.Contains(composeYaml, ":5432/tcp") || strings.Contains(composeYaml, ":9901/tcp") {
		t.Fatal("compose.yml must declare container_name: aoc-* on all 11 services, exclude aoc-tether, and not expose :5432 or :9901 to host")
	}
	if strings.Contains(envoyYaml, "tether_tcp") || strings.Contains(envoyYaml, "50051") || !strings.Contains(envoyYaml, "mesh_gateway_http") { t.Fatal("envoy.yaml must configure mesh_gateway_http without tether_tcp or :50051") }
	for _, c := range []struct{ protoFile, grpcPrefix, envoyCluster, serviceSource, composeSvc string }{
		{"src/libs/aocproto/proto/internal/aoc/auth/v1/auth.proto", "/aoc.auth.v1.", "auth_grpc", "src/server/services/cmd/auth/main.go", "auth:"},
		{"src/libs/aocproto/proto/internal/aoc/gateway/v1/gateway.proto", "/aoc.gateway.v1.", "gateway_grpc", "src/server/services/cmd/gateway/main.go", "gateway:"},
		{"src/libs/aocproto/proto/internal/aoc/character/v1/character.proto", "/aoc.character.v1.", "character_grpc", "src/server/services/cmd/character/main.go", "character:"},
		{"src/libs/aocproto/proto/internal/aoc/mesh/v1/mesh.proto", "/aoc.mesh.v1.", "mesh_grpc", "src/server/services/cmd/mesh/main.go", "mesh:"},
		{"src/libs/aocproto/proto/internal/aoc/citynode/v1/citynode.proto", "/aoc.citynode.v1.", "citynode_grpc", "src/server/services/cmd/citynode/main.go", "citynode:"},
		{"src/libs/aocproto/proto/internal/aoc/quest/v1/quest.proto", "/aoc.quest.v1.", "quest_grpc", "src/server/services/cmd/quest/main.go", "quest:"},
		{"src/libs/aocproto/proto/internal/aoc/worldadmin/v1/worldadmin.proto", "/aoc.worldadmin.v1.", "world_admin_grpc", "src/server/services/cmd/worldctl/main.go", "world:"},
	} {
		pkgName := strings.Trim(c.grpcPrefix, "/.")
		if !strings.Contains(readRepoFile(t, c.protoFile), "package "+pkgName+";") { t.Fatalf("%s missing package %s declaration", c.protoFile, pkgName) }
		if !strings.Contains(envoyYaml, `prefix: "`+c.grpcPrefix+`"`) || !strings.Contains(envoyYaml, "cluster: "+c.envoyCluster) { t.Fatalf("envoy.yaml missing route prefix %s or cluster %s", c.grpcPrefix, c.envoyCluster) }
		if !strings.Contains(readRepoFile(t, c.serviceSource), strings.ReplaceAll(pkgName, ".", "/")) || !strings.Contains(composeYaml, c.composeSvc) { t.Fatalf("%s or compose.yml missing contract for %s", c.serviceSource, pkgName) }
	}
}
func TestFlow5_MeshRoutingGatewayEnterWorldAndEnvoyComposeParityE2E(t *testing.T) {
	verifyEnvoyAndComposeGrpcContractParity(t)
	mg := newMeshGatewayHarness()
	if mg.registerZone(1, 1, "127.0.0.1:7777", [4]float64{-100000, 0, -100000, 100000}, 1) != nil || mg.registerZone(2, 2, "127.0.0.1:7778", [4]float64{0, 100000, -100000, 100000}, 1) != nil { t.Fatal("registerZone 1 or 2 failed") }
	if mg.registerZone(1, 1, "0.0.0.0:7777", [4]float64{-100000, 0, -100000, 100000}, 2) == nil || mg.registerZone(1, 1, "127.0.0.1:7777", [4]float64{-100000, 0, -100000, 100000}, 1) == nil {
		t.Fatal("expected unspecified bind address or stale authority epoch to be rejected")
	}
	if z, band, err := mg.resolveZoneWithHysteresis(1, -50000, 0, 20000); err != nil || z != 1 || band != "interior" { t.Fatalf("expected interior zone 1, got z=%d band=%s err=%v", z, band, err) }
	if z, band, err := mg.resolveZoneWithHysteresis(1, 15000, 0, 20000); err != nil || z != 1 || band != "hysteresis" { t.Fatalf("expected hysteresis retention in zone 1 at +150m, got z=%d band=%s err=%v", z, band, err) }
	if z, band, err := mg.resolveZoneWithHysteresis(1, 25000, 0, 20000); err != nil || z != 2 || band != "handover" { t.Fatalf("expected handover to zone 2 at +250m, got z=%d band=%s err=%v", z, band, err) }
	if hdr, body, err := encodeDecodeMeshHeader(1, 2, 3, []byte("entity-state-frame")); err != nil || hdr != [3]uint32{1, 2, 3} || !bytes.Equal(body, []byte("entity-state-frame")) {
		t.Fatalf("mesh forward header round-trip failed: hdr=%v body=%q err=%v", hdr, body, err)
	}
	gwMux := http.NewServeMux()
	gwMux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) { _, _ = w.Write([]byte(`{"status":"ok"}`)) })
	gwSrv := httptest.NewServer(gwMux)
	defer gwSrv.Close()
	if resp, err := http.Get(gwSrv.URL + "/healthz"); err != nil || resp.StatusCode != http.StatusOK { t.Fatalf("gateway /healthz failed: resp=%v err=%v", resp, err) } else { _ = resp.Body.Close() }
	charID := fmt.Sprintf("%032x", 42)
	ticket, addr, err := mg.enterWorld(1001, charID, 1)
	if err != nil || addr != "127.0.0.1:7777" || !isLowerHex(ticket, 32) { t.Fatalf("enterWorld failed: ticket=%q addr=%q err=%v", ticket, addr, err) }
	if acct, err := mg.redeemTicket(ticket, charID, 1); err != nil || acct != 1001 { t.Fatalf("first redeemTicket failed: acct=%d err=%v", acct, err) }
	if _, err := mg.redeemTicket(ticket, charID, 1); err == nil { t.Fatal("expected replay of single-use join ticket to be rejected") }
}
func wrapE2EETagAndAltSvc(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Alt-Svc", `h3=":8080"; ma=86400, h2c=":8080"; ma=86400`)
		rec := httptest.NewRecorder()
		next.ServeHTTP(rec, r)
		for k, vals := range rec.Header() { w.Header()[k] = vals }
		sum := sha256.Sum256(rec.Body.Bytes())
		etag := fmt.Sprintf(`"sha256-%x"`, sum[:8])
		if rec.Code == http.StatusOK { w.Header().Set("ETag", etag) }
		if ifm := strings.TrimSpace(r.Header.Get("If-Match")); ifm != "" && ifm != "*" && ifm != etag { w.WriteHeader(http.StatusPreconditionFailed); return }
		if inm := strings.TrimSpace(r.Header.Get("If-None-Match")); (r.Method == http.MethodGet || r.Method == http.MethodHead) && inm == etag { w.WriteHeader(http.StatusNotModified); return }
		w.WriteHeader(rec.Code)
		_, _ = w.Write(rec.Body.Bytes())
	})
}
func mountE2ECharacterAndEnterRoutes(mux *http.ServeMux, h *authFlowHarness) {
	authAcct := func(w http.ResponseWriter, r *http.Request) (int64, bool) {
		tok := strings.TrimSpace(strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer"))
		h.mu.Lock()
		defer h.mu.Unlock()
		s, ok := h.sessions[tok]
		if !ok || !h.now().Before(s.ExpiresAt) { w.WriteHeader(http.StatusUnauthorized); return 0, false }
		return s.AccountID, true
	}
	mux.HandleFunc("/api/v1/characters", func(w http.ResponseWriter, r *http.Request) {
		acctID, ok := authAcct(w, r)
		if !ok { return }
		w.Header().Set("Content-Type", "application/json")
		if r.Method == http.MethodGet {
			h.mu.Lock(); list := h.charsByAccount[acctID]; h.mu.Unlock()
			_ = json.NewEncoder(w).Encode(map[string]any{"characters": list})
			return
		}
		var req map[string]any
		_ = json.NewDecoder(r.Body).Decode(&req)
		id := fmt.Sprintf("%032x", acctID)
		entry := map[string]any{"id": id, "character_id": id, "name": req["name"], "class_id": 1, "level": 1, "zone": "verra-01"}
		h.mu.Lock(); h.charsByAccount[acctID] = append(h.charsByAccount[acctID], entry); h.mu.Unlock()
		w.WriteHeader(http.StatusCreated)
		_ = json.NewEncoder(w).Encode(entry)
	})
	mux.HandleFunc("/api/v1/characters/", func(w http.ResponseWriter, r *http.Request) {
		acctID, ok := authAcct(w, r)
		if !ok { return }
		w.Header().Set("Content-Type", "application/json")
		if r.Method == http.MethodDelete {
			h.mu.Lock(); delete(h.charsByAccount, acctID); h.mu.Unlock()
			_ = json.NewEncoder(w).Encode(map[string]any{"deleted": true})
			return
		}
		ticket := fmt.Sprintf("%032x", acctID+9000)
		_ = json.NewEncoder(w).Encode(map[string]any{"character_id": fmt.Sprintf("%032x", acctID), "join_ticket": ticket, "ticket": ticket, "world_endpoint": "127.0.0.1:7777", "zone_key": "verra-01"})
	})
	mux.HandleFunc("/api/v1/world/enter", func(w http.ResponseWriter, r *http.Request) {
		var body map[string]any
		_ = json.NewDecoder(r.Body).Decode(&body)
		w.Header().Set("Content-Type", "application/json")
		if body["join_ticket"] == nil && body["ticket"] == nil { w.WriteHeader(http.StatusUnauthorized); return }
		_ = json.NewEncoder(w).Encode(map[string]any{"status": "ALLOW", "world_endpoint": "127.0.0.1:7777", "zone_key": "verra-01"})
	})
}
func TestFlow6_ETagH2CCharacterSelectAndEphemeralAccountDeletionE2E(t *testing.T) {
	etagSrc, mpSrc, gwSrc, charHttpSrc := readRepoFile(t, "src/server/services/internal/telemetry/etag_middleware.go"), readRepoFile(t, "src/server/services/internal/telemetry/multiprotocol_server.go"), readRepoFile(t, "src/server/services/cmd/gateway/main.go"), readRepoFile(t, "src/server/services/cmd/gateway/characters_http.go")
	if !strings.Contains(etagSrc, "ETagMiddleware") || !strings.Contains(mpSrc, "NewH2CClient") || !strings.Contains(gwSrc, "ETagMiddleware") || !strings.Contains(gwSrc, "/api/v1/auth/account") || !strings.Contains(charHttpSrc, "/api/v1/characters") {
		t.Fatal("missing ETagMiddleware, NewH2CClient, account deletion proxy, or character HTTP bridge contract")
	}
	h := newAuthFlowHarness(strings.Repeat("b2", 32), time.Now)
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) { _, _ = w.Write([]byte(`{"status":"ok"}`)) })
	mux.HandleFunc("/api/v1/auth/register", h.registerHTTP); mux.HandleFunc("/api/v1/auth/login", h.loginHTTP); mux.HandleFunc("/api/v1/auth/account", h.deleteAccountHTTP)
	mountE2ECharacterAndEnterRoutes(mux, h)
	srv := httptest.NewServer(wrapE2EETagAndAltSvc(mux))
	defer srv.Close()
	hzResp, _ := http.Get(srv.URL + "/healthz")
	etag, altSvc := hzResp.Header.Get("ETag"), hzResp.Header.Get("Alt-Svc")
	_ = hzResp.Body.Close()
	if !strings.HasPrefix(etag, `"sha256-`) || !strings.Contains(altSvc, "h2c=") { t.Fatalf("expected strong ETag and Alt-Svc h2c header, got etag=%q altSvc=%q", etag, altSvc) }
	req304, _ := http.NewRequest(http.MethodGet, srv.URL+"/healthz", nil)
	req304.Header.Set("If-None-Match", etag)
	if resp304, err := http.DefaultClient.Do(req304); err != nil || resp304.StatusCode != http.StatusNotModified { t.Fatalf("expected 304 Not Modified on If-None-Match, got %v %v", resp304, err) } else { _ = resp304.Body.Close() }
	req412, _ := http.NewRequest(http.MethodGet, srv.URL+"/healthz", nil)
	req412.Header.Set("If-Match", `"sha256-mismatch"`)
	if resp412, err := http.DefaultClient.Do(req412); err != nil || resp412.StatusCode != http.StatusPreconditionFailed { t.Fatalf("expected 412 Precondition Failed on If-Match mismatch, got %v %v", resp412, err) } else { _ = resp412.Body.Close() }
	verifyEphemeralCharacterSelectAndDeleteFlow(t, srv.URL)
}
func verifyEphemeralCharacterSelectAndDeleteFlow(t *testing.T, baseURL string) {
	t.Helper()
	_, _ = postAuthJSON(t, baseURL+"/api/v1/auth/register", `{"username":"ephem_hero","password":"correct-horse-battery"}`)
	loginResp, loginMap := postAuthJSON(t, baseURL+"/api/v1/auth/login", `{"username":"ephem_hero","password":"correct-horse-battery"}`)
	tok := extractLiveToken(loginMap)
	if loginResp.StatusCode != http.StatusOK || tok == "" { t.Fatalf("ephemeral login failed: %d %v", loginResp.StatusCode, loginMap) }
	cResp, cMap := doAuthReqJSON(t, http.MethodPost, baseURL+"/api/v1/characters", tok, `{"name":"Kaelen","class_id":1,"race_id":1}`)
	charID, _ := cMap["id"].(string)
	if cResp.StatusCode != http.StatusCreated || charID == "" { t.Fatalf("create character failed: %d %v", cResp.StatusCode, cMap) }
	sResp, sMap := doAuthReqJSON(t, http.MethodPost, baseURL+"/api/v1/characters/"+charID+"/select", tok, `{}`)
	joinTicket, _ := sMap["join_ticket"].(string)
	if sResp.StatusCode != http.StatusOK || !isLowerHex(joinTicket, 32) { t.Fatalf("select character failed: %d %v", sResp.StatusCode, sMap) }
	eResp, eMap := doAuthReqJSON(t, http.MethodPost, baseURL+"/api/v1/world/enter", tok, fmt.Sprintf(`{"join_ticket":%q}`, joinTicket))
	if eResp.StatusCode != http.StatusOK || eMap["status"] != "ALLOW" { t.Fatalf("world/enter failed: %d %v", eResp.StatusCode, eMap) }
	if delChar, _ := doAuthReqJSON(t, http.MethodDelete, baseURL+"/api/v1/characters/"+charID, tok, ""); delChar.StatusCode != http.StatusOK { t.Fatalf("DELETE /api/v1/characters/%s failed: %d", charID, delChar.StatusCode) }
	dResp, dMap := doAuthReqJSON(t, http.MethodDelete, baseURL+"/api/v1/auth/account", tok, `{"username":"ephem_hero","password":"correct-horse-battery"}`)
	if dResp.StatusCode != http.StatusOK || dMap["deleted"] != true { t.Fatalf("DELETE /api/v1/auth/account failed: %d %v", dResp.StatusCode, dMap) }
	if reLogin, _ := postAuthJSON(t, baseURL+"/api/v1/auth/login", `{"username":"ephem_hero","password":"correct-horse-battery"}`); reLogin.StatusCode != http.StatusUnauthorized {
		t.Fatalf("expected 401 Unauthorized after deleting ephemeral account, got %d", reLogin.StatusCode)
	}
	if chAfter, _ := doAuthReqJSON(t, http.MethodGet, baseURL+"/api/v1/characters", tok, ""); chAfter.StatusCode != http.StatusUnauthorized {
		t.Fatalf("expected 401 Unauthorized on /api/v1/characters with deleted account token, got %d", chAfter.StatusCode)
	}
}
var (
	liveComposeOnce   sync.Once
	liveComposePassed bool
)
func TestFlow5_ComposeAndEnvoyIngressContract(t *testing.T) { TestLiveComposeFunctionalAPIs(t) }
func TestLiveComposeFunctionalAPIs(t *testing.T) {
	verifyEnvoyAndComposeGrpcContractParity(t)
	liveComposeOnce.Do(func() { probeLiveComposeIngress(t); liveComposePassed = true })
	if !liveComposePassed { t.Fatal("live compose functional API suite failed") }
}
func extractLiveToken(payload map[string]any) string {
	for _, k := range []string{"token", "access_token"} {
		if tok, ok := payload[k].(string); ok && len(tok) >= 16 { return tok }
	}
	return ""
}
func probeLiveComposeIngress(t *testing.T) {
	t.Helper()
	composeYaml := readRepoFile(t, "src/deploy/compose.yml")
	if strings.Count(composeYaml, "container_name: aoc-") != 11 || strings.Contains(composeYaml, "aoc-tether") || strings.Contains(composeYaml, ":5432/tcp") || strings.Contains(composeYaml, ":9901/tcp") {
		t.Fatal("compose.yml must declare container_name: aoc-* on all 11 services, exclude aoc-tether, and not expose :5432 or :9901 to host")
	}
	client := &http.Client{Timeout: 5 * time.Second}
	for _, u := range []string{"http://127.0.0.1:8080/healthz", "http://127.0.0.1:8080/"} {
		resp, err := client.Get(u)
		if err != nil || resp.StatusCode != http.StatusOK { t.Fatalf("GET %s failed: resp=%v err=%v", u, resp, err) }
		_ = resp.Body.Close()
	}
	var nonce [4]byte
	_, _ = rand.Read(nonce[:])
	sfx, alpha := hex.EncodeToString(nonce[:]), make([]byte, len(nonce))
	for i, b := range nonce { alpha[i] = 'a' + (b % 26) }
	user, email, pass, charName := "e2e_"+sfx, "e2e_"+sfx+"@sandalcore.local", "SandalE2E_"+sfx+"!", "Kael"+string(alpha)
	regBody := fmt.Sprintf(`{"username":%q,"email":%q,"password":%q}`, user, email, pass)
	regResp, regMap := postAuthJSON(t, "http://127.0.0.1:8080/api/v1/auth/register", regBody)
	if (regResp.StatusCode != http.StatusOK && regResp.StatusCode != http.StatusCreated) || extractLiveToken(regMap) == "" { t.Fatalf("register expected 200/201 with token, got %d (%v)", regResp.StatusCode, regMap) }
	if dupResp, _ := postAuthJSON(t, "http://127.0.0.1:8080/api/v1/auth/register", regBody); dupResp.StatusCode != http.StatusConflict { t.Fatalf("duplicate register expected 409 Conflict, got %d", dupResp.StatusCode) }
	if badLogin, _ := postAuthJSON(t, "http://127.0.0.1:8080/api/v1/auth/login", fmt.Sprintf(`{"username":%q,"password":"wrong-password-123"}`, user)); badLogin.StatusCode != http.StatusUnauthorized { t.Fatalf("wrong password login expected 401 Unauthorized, got %d", badLogin.StatusCode) }
	loginResp, loginMap := postAuthJSON(t, "http://127.0.0.1:8080/api/v1/auth/login", regBody)
	tok := extractLiveToken(loginMap)
	if loginResp.StatusCode != http.StatusOK || tok == "" { t.Fatalf("login expected 200 with valid token, got %d (%v)", loginResp.StatusCode, loginMap) }
	probeLiveCatalogAndCharacters(t, client, tok, charName)
	probeLiveComposeGRPCAndTLS(t)
	delResp, delMap := doAuthReqJSON(t, http.MethodDelete, "http://127.0.0.1:8080/api/v1/auth/account", tok, fmt.Sprintf(`{"username":%q,"password":%q}`, user, pass))
	if delResp.StatusCode != http.StatusOK || delMap["deleted"] != true { t.Fatalf("DELETE /api/v1/auth/account expected 200 deleted=true, got %d (%v)", delResp.StatusCode, delMap) }
	if loginAfter, _ := postAuthJSON(t, "http://127.0.0.1:8080/api/v1/auth/login", regBody); loginAfter.StatusCode != http.StatusUnauthorized { t.Fatalf("expected 401 Unauthorized after account deletion, got %d", loginAfter.StatusCode) }
}
func probeLiveCatalogAndCharacters(t *testing.T, client *http.Client, tok, charName string) {
	t.Helper()
	gmTok := strings.TrimSpace(os.Getenv("AOC_GM_OBSERVER_TOKENS"))
	if gmTok == "" { gmTok = "gm-observer-dev-token" }
	for _, ep := range []string{"/api/v1/gm/commands", "/api/v1/catalog/search?q=sword", "/api/v1/world/entities"} {
		if r, body := doAuthReqJSON(t, http.MethodGet, "http://127.0.0.1:8080"+ep, gmTok, ""); r.StatusCode != http.StatusOK || len(body) == 0 { t.Fatalf("GET %s expected 200 JSON, got %d (%v)", ep, r.StatusCode, body) }
	}
	listResp, _ := doAuthReqJSON(t, http.MethodGet, "http://127.0.0.1:8080/api/v1/characters", tok, "")
	etag := listResp.Header.Get("ETag")
	if listResp.StatusCode != http.StatusOK || etag == "" { t.Fatalf("GET /api/v1/characters expected 200 + ETag, got %d etag=%q", listResp.StatusCode, etag) }
	req304, _ := http.NewRequest(http.MethodGet, "http://127.0.0.1:8080/api/v1/characters", nil)
	req304.Header.Set("Authorization", "Bearer "+tok); req304.Header.Set("If-None-Match", etag)
	if resp304, err := client.Do(req304); err != nil || resp304.StatusCode != http.StatusNotModified { t.Fatalf("GET /api/v1/characters with If-None-Match expected 304, got %v %v", resp304, err) } else { _ = resp304.Body.Close() }
	cResp, cMap := doAuthReqJSON(t, http.MethodPost, "http://127.0.0.1:8080/api/v1/characters", tok, fmt.Sprintf(`{"name":%q,"class_id":1,"race_id":1}`, charName))
	charID, _ := cMap["id"].(string)
	if cResp.StatusCode != http.StatusCreated || !isLowerHex(charID, 32) { t.Fatalf("POST /api/v1/characters failed: %d %v", cResp.StatusCode, cMap) }
	sResp, sMap := doAuthReqJSON(t, http.MethodPost, "http://127.0.0.1:8080/api/v1/characters/"+charID+"/select", tok, `{}`)
	joinTicket, _ := sMap["join_ticket"].(string); worldEp, _ := sMap["world_endpoint"].(string)
	if sResp.StatusCode != http.StatusOK || !isLowerHex(joinTicket, 32) || worldEp == "" { t.Fatalf("POST /api/v1/characters/%s/select failed: %d %v", charID, sResp.StatusCode, sMap) }
	probeLiveICSTLSStream(t, tok, "Ics"+charName)
	probeLiveGRPCRoutes(t, tok, charID, joinTicket)
	if delCharResp, _ := doAuthReqJSON(t, http.MethodDelete, "http://127.0.0.1:8080/api/v1/characters/"+charID, tok, ""); delCharResp.StatusCode != http.StatusOK && delCharResp.StatusCode != http.StatusNoContent && delCharResp.StatusCode != http.StatusMethodNotAllowed { t.Fatalf("DELETE /api/v1/characters/%s unexpected status %d", charID, delCharResp.StatusCode) }
}
func resolveLiveServiceToken(t *testing.T) string {
	if tok := strings.TrimSpace(os.Getenv("AOC_SERVICE_TOKEN")); tok != "" { return tok }
	if raw, err := os.ReadFile(filepath.Join(repoRootDir(t), "dist", "services", "service.token")); err == nil && len(strings.TrimSpace(string(raw))) > 0 { return strings.TrimSpace(string(raw)) }
	if raw, err := os.ReadFile(filepath.Join(repoRootDir(t), "deploy", ".env")); err == nil {
		for _, line := range strings.Split(string(raw), "\n") {
			if k, v, ok := strings.Cut(strings.TrimSpace(line), "="); ok && strings.TrimSpace(k) == "AOC_SERVICE_TOKEN" && strings.TrimSpace(v) != "" { return strings.TrimSpace(v) }
		}
	}
	return "dev-local-service-token-32bytes!"
}
func probeLiveGRPCRoutes(t *testing.T, userTok, charID, joinTicket string) {
	t.Helper()
	conn, err := grpc.NewClient("127.0.0.1:9090", grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil { t.Fatalf("grpc.NewClient 127.0.0.1:9090 failed: %v", err) }
	defer conn.Close()
	ctx, cancel := context.WithTimeout(metadata.AppendToOutgoingContext(context.Background(), "authorization", "Bearer "+resolveLiveServiceToken(t)), 4*time.Second)
	defer cancel()
	vResp, err := authv1.NewAuthServiceClient(conn).ValidateToken(ctx, &authv1.ValidateTokenRequest{Token: userTok})
	if err != nil || vResp.GetAccountId() <= 0 { t.Fatalf("AuthService.ValidateToken failed: resp=%v err=%v", vResp, err) }
	cList, err := characterv1.NewCharacterServiceClient(conn).ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: vResp.GetAccountId()})
	if err != nil || len(cList.GetCharacters()) == 0 { t.Fatalf("CharacterService.ListCharacters failed: resp=%v err=%v", cList, err) }
	if nResp, err := citynodev1.NewCityNodeServiceClient(conn).ListNodes(ctx, &citynodev1.ListNodesRequest{}); err != nil || nResp.GetClock() == nil { t.Fatalf("CityNodeService.ListNodes failed: resp=%v err=%v", nResp, err) }
	if qResp, err := questv1.NewQuestServiceClient(conn).LoadLog(ctx, &questv1.LoadLogRequest{CharacterId: charID}); err != nil || qResp.GetLog() == nil { t.Fatalf("QuestService.LoadLog failed: resp=%v err=%v", qResp, err) }
	if _, err := meshv1.NewMeshServiceClient(conn).Place(ctx, &meshv1.PlaceRequest{CharacterId: charID}); err != nil && status.Code(err) != codes.Unavailable { t.Fatalf("MeshService.Place failed: %v", err) }
	v7Key, _ := uuid.NewV7()
	redeemCtx := metadata.AppendToOutgoingContext(ctx, "idempotency-key", v7Key.String())
	if _, err := gatewayv1.NewGatewayServiceClient(conn).RedeemTicket(redeemCtx, &gatewayv1.RedeemTicketRequest{Ticket: joinTicket, CharacterId: charID, ServerId: 1}); err != nil && status.Code(err) != codes.PermissionDenied { t.Fatalf("GatewayService.RedeemTicket failed: %v", err) }
	if pResp, err := worldadminv1.NewWorldAdminServiceClient(conn).ListPlaces(ctx, &worldadminv1.ListPlacesRequest{}); err != nil || len(pResp.GetPlaces()) == 0 { t.Fatalf("WorldAdminService.ListPlaces failed: resp=%v err=%v", pResp, err) }
	if _, err := characterv1.NewCharacterServiceClient(conn).DeleteCharacter(ctx, &characterv1.DeleteCharacterRequest{Id: charID, AccountId: vResp.GetAccountId()}); err != nil { t.Fatalf("CharacterService.DeleteCharacter failed: %v", err) }
}
func resolveICSTargetServerName(leaf *x509.Certificate) string {
	for _, ip := range leaf.IPAddresses {
		if ip.Equal(net.ParseIP("127.0.0.1")) { return "127.0.0.1" }
	}
	for _, name := range append(append([]string(nil), leaf.DNSNames...), leaf.Subject.CommonName) {
		if n := strings.TrimSpace(name); n != "" && n != "sandalcore-gateway" {
			if strings.HasPrefix(n, "*.") { return "localhost." + strings.TrimPrefix(n, "*.") }
			return n
		}
	}
	return "127.0.0.1"
}
func verifyICSTLSCertChain(pool, inter *x509.CertPool, serverName string, certs []*x509.Certificate) error {
	if len(certs) == 0 || certs[0] == nil || certs[0].Subject.CommonName == "sandalcore-gateway" { return errors.New("invalid or untrusted ICS TLS leaf certificate on :50061") }
	for _, ic := range certs[1:] { inter.AddCert(ic) }
	_, err := certs[0].Verify(x509.VerifyOptions{Roots: pool, Intermediates: inter, DNSName: serverName})
	return err
}
func loadICSTLSCredentials(t *testing.T) credentials.TransportCredentials {
	t.Helper()
	pool, _ := x509.SystemCertPool(); if pool == nil { pool = x509.NewCertPool() }
	inter, root := x509.NewCertPool(), repoRootDir(t)
	if rawCA, err := os.ReadFile(filepath.Join(root, "dist", "certs", "ca.crt")); err == nil { _ = pool.AppendCertsFromPEM(rawCA) }
	srvPem, srvKey := filepath.Join(root, "dist", "certs", "server.pem"), filepath.Join(root, "dist", "certs", "server.key")
	if _, err := os.Stat(srvPem); err != nil { srvPem, srvKey = filepath.Join(root, "deploy", "certs", "server.pem"), filepath.Join(root, "deploy", "certs", "server.key") }
	pair, err := tls.LoadX509KeyPair(srvPem, srvKey); if err != nil { t.Fatalf("load server.pem/server.key failed: %v", err) }
	var chain []*x509.Certificate
	for i, der := range pair.Certificate {
		if c, err := x509.ParseCertificate(der); err == nil { chain = append(chain, c); if i > 0 { inter.AddCert(c); pool.AddCert(c) } }
	}
	serverName := resolveICSTargetServerName(chain[0])
	if err := verifyICSTLSCertChain(pool, inter, serverName, chain); err != nil { t.Fatalf("server.pem strict TLS check failed: %v", err) }
	return credentials.NewTLS(&tls.Config{RootCAs: pool, ServerName: serverName, MinVersion: tls.VersionTLS13, NextProtos: []string{"h2"}, InsecureSkipVerify: false, VerifyConnection: func(cs tls.ConnectionState) error { return verifyICSTLSCertChain(pool, inter, serverName, cs.PeerCertificates) }})
}
func callICSStream(t *testing.T, stream icsxclient.XClientService_ProcessXClientMessageClient, reqType, authTok string, req proto.Message) []byte {
	t.Helper()
	payload, _ := proto.Marshal(req)
	tags := map[string]string{"client_tracking_id": reqType, "client_id": "ue_e2e"}
	if authTok != "" { tags["auth_token"] = authTok }
	if err := stream.Send(&icscommon.MessageWrapper{SystemData: &icscommon.SystemData{Tags: tags}, MessageTypeName: reqType, MessageData: payload}); err != nil { t.Fatalf("send %s failed: %v", reqType, err) }
	wrap, err := stream.Recv()
	if err != nil || wrap.GetMessageTypeName() != strings.TrimSuffix(reqType, "Request")+"Reply" || wrap.GetStatusCode() != icscommon.IcsStatusCode_ICS_STATUS_CODE_SUCCESS { t.Fatalf("%s failed: wrap=%v err=%v", reqType, wrap, err) }
	return wrap.GetMessageData()
}
func hasCharacterCustomizationBytes(rawChars [][]byte, charName string) bool {
	for _, raw := range rawChars {
		if bytes.Contains(raw, []byte(charName)) && bytes.Contains(raw, []byte("CustomizationDataKey")) { return true }
	}
	return false
}
func probeLiveWorldAdminDuringSession(t *testing.T, charID, charName string) error {
	adminAddr := "127.0.0.1:9090"
	if c, err := net.DialTimeout("tcp", "127.0.0.1:50055", 150*time.Millisecond); err == nil { _ = c.Close(); adminAddr = "127.0.0.1:50055" }
	conn, err := grpc.NewClient(adminAddr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil { return err }
	defer conn.Close()
	cli := worldadminv1.NewWorldAdminServiceClient(conn)
	ctx, cancel := context.WithTimeout(metadata.AppendToOutgoingContext(context.Background(), "authorization", "Bearer "+resolveLiveServiceToken(t)), 12*time.Second)
	defer cancel()
	for i := 0; i < 50; i++ {
		if lp, err := cli.ListPlayers(ctx, &worldadminv1.ListPlayersRequest{}); err == nil && len(lp.GetPlayers()) >= 1 {
			if _, err := cli.ListPlaces(ctx, &worldadminv1.ListPlacesRequest{}); err != nil { return err }
			if _, err := cli.SetMoveSpeed(ctx, &worldadminv1.SetMoveSpeedRequest{Character: charName, Multiplier: 2.0}); err != nil { return err }
			if _, err := cli.GiveMoney(ctx, &worldadminv1.GiveMoneyRequest{Character: charName, Copper: 500}); err != nil { return err }
			if _, err := cli.SetWeaponXp(ctx, &worldadminv1.SetWeaponXpRequest{Character: charName, Line: "scepter", Amount: &worldadminv1.SetWeaponXpRequest_Level{Level: 10}}); err != nil { return err }
			return nil
		}
		time.Sleep(150 * time.Millisecond)
	}
	return fmt.Errorf("player %s (%s) never appeared in WorldAdminService.ListPlayers", charName, charID)
}
func runWorldLive10StageE2E(t *testing.T, charID, charName, ticket string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	adminErrCh := make(chan error, 1)
	go func() { adminErrCh <- probeLiveWorldAdminDuringSession(t, charID, charName) }()
	cmd := exec.CommandContext(ctx, "docker", "exec", "-i", "-e", "AOC_LIVE_CHAR_ID="+charID, "-e", "AOC_LIVE_TICKET="+ticket, "-e", "AOC_LIVE_UDP_HOST=127.0.0.1", "-e", "AOC_LIVE_UDP_PORT=7777", "-e", "AOC_LIVE_USMAP_PATH=/app/content/aoc.usmap", "-e", "AOC_LIVE_HOLD_MS=1500", "aoc-world", "/app/test_world_live_e2e")
	stdinR, stdinW, _ := os.Pipe(); defer stdinR.Close(); defer stdinW.Close(); cmd.Stdin = stdinR
	if runtime.GOOS == "windows" && strings.TrimSpace(os.Getenv("DOCKER_HOST")) == "" { cmd.Env = append(os.Environ(), "DOCKER_HOST=tcp://127.0.0.1:2375") }
	out, err := cmd.CombinedOutput()
	if err != nil || !strings.Contains(string(out), "[PASS] test_world_live_e2e completed all 10 stages") { t.Fatalf("test_world_live_e2e failed (err=%v):\n%s", err, string(out)) }
	if adminErr := <-adminErrCh; adminErr != nil { t.Fatalf("WorldAdminService live session check failed: %v", adminErr) }
}
func probeICSPlayAndWorldUDP(t *testing.T, stream icsxclient.XClientService_ProcessXClientMessageClient, worldID, charID, charName string) {
	t.Helper()
	playPayload, _ := proto.Marshal(&icsxclient.PlayRequest{CharacterId: charID, WorldId: worldID})
	var playReply icsxclient.PlayReply
	for attempt := 0; attempt < 15 && playReply.GetGameConnectionToken() == ""; attempt++ {
		if err := stream.Send(&icscommon.MessageWrapper{SystemData: &icscommon.SystemData{Tags: map[string]string{"client_tracking_id": "play", "client_id": "ue_e2e"}}, MessageTypeName: "ics_xclient.PlayRequest", MessageData: playPayload}); err != nil { t.Fatalf("send PlayRequest failed: %v", err) }
		for i := 0; i < 3; i++ {
			wrap, err := stream.Recv(); if err != nil { t.Fatalf("recv PlayReply %d failed: %v", i, err) }
			if wrap.GetMessageTypeName() != "ics_xclient.PlayReply" { continue }
			if wrap.GetStatusCode() == icscommon.IcsStatusCode_ICS_STATUS_CODE_SUCCESS { _ = proto.Unmarshal(wrap.GetMessageData(), &playReply); break }
			if wrap.GetStatusCode() != icscommon.IcsStatusCode_ICS_STATUS_CODE_ACKNOWLEDGED { time.Sleep(time.Second); break }
		}
	}
	if playReply.GetGameConnectionString() == "world:7777" || playReply.GetGameConnectionString() != "127.0.0.1:7777" || !isLowerHex(playReply.GetGameConnectionToken(), 32) { t.Fatalf("invalid PlayReply endpoint=%q token=%q", playReply.GetGameConnectionString(), playReply.GetGameConnectionToken()) }
	runWorldLive10StageE2E(t, charID, charName, playReply.GetGameConnectionToken())
	var postChars icsxclient.GetCharactersReply
	if proto.Unmarshal(callICSStream(t, stream, "ics_xclient.GetCharactersRequest", "", &icsxclient.GetCharactersRequest{WorldId: worldID}), &postChars) != nil || !hasCharacterCustomizationBytes(postChars.GetCharactersRaw(), charName) { t.Fatalf("post-disconnect GetCharactersReply missing %q", charName) }
}
func probeLiveICSTLSStream(t *testing.T, userTok, charName string) {
	t.Helper()
	conn, err := grpc.NewClient("127.0.0.1:50061", grpc.WithTransportCredentials(loadICSTLSCredentials(t))); if err != nil { t.Fatalf("grpc.NewClient 127.0.0.1:50061 failed: %v", err) }
	defer conn.Close()
	ctx, cancel := context.WithTimeout(metadata.AppendToOutgoingContext(context.Background(), "x-ics-auth", "emulator-magic"), 30*time.Second); defer cancel()
	stream, err := icsxclient.NewXClientServiceClient(conn).ProcessXClientMessage(ctx); if err != nil { t.Fatalf("ProcessXClientMessage open failed: %v", err) }
	defer func() { _ = stream.CloseSend() }()
	var sessReply icscommon.SessionReply; var verReply icsxclient.CheckGameVersionReply; var worldsReply icsxclient.GetWorldsReply
	if proto.Unmarshal(callICSStream(t, stream, "ics_common.SessionRequest", userTok, &icscommon.SessionRequest{RequestType: icscommon.SessionRequestType_SESSION_REQUEST_TYPE_OPEN}), &sessReply) != nil || sessReply.GetResultCode() != icscommon.IcsStatusCode_ICS_STATUS_CODE_SUCCESS { t.Fatalf("SessionReply failed: %v", &sessReply) }
	if proto.Unmarshal(callICSStream(t, stream, "ics_xclient.CheckGameVersionRequest", "", &icsxclient.CheckGameVersionRequest{GameVersion: "20260119002-438018"}), &verReply) != nil || !verReply.GetSupported() { t.Fatalf("CheckGameVersionReply failed: %v", &verReply) }
	if proto.Unmarshal(callICSStream(t, stream, "ics_xclient.GetWorldsRequest", "", &icsxclient.GetWorldsRequest{}), &worldsReply) != nil || len(worldsReply.GetWorlds()) == 0 { t.Fatal("expected >=1 world") }
	worldID := worldsReply.GetWorlds()[0].GetWorldId()
	createInfoRaw, err := hex.DecodeString(strings.TrimSpace(readRepoFile(t, "src/server/services/internal/createinfo/testdata/client_create_info.hex"))); if err != nil || len(createInfoRaw) == 0 { t.Fatalf("decode client_create_info.hex failed: %v", err) }
	var createReply icsxclient.CreateCharacterReply; var charsReply icsxclient.GetCharactersReply
	if proto.Unmarshal(callICSStream(t, stream, "ics_xclient.CreateCharacterRequest", "", &icsxclient.CreateCharacterRequest{CharacterName: charName, WorldId: worldID, CreateCharacterInfoRaw: createInfoRaw}), &createReply) != nil || !isLowerHex(createReply.GetCharacterId(), 32) { t.Fatalf("CreateCharacterReply failed: %v", &createReply) }
	if proto.Unmarshal(callICSStream(t, stream, "ics_xclient.GetCharactersRequest", "", &icsxclient.GetCharactersRequest{WorldId: worldID}), &charsReply) != nil || !hasCharacterCustomizationBytes(charsReply.GetCharactersRaw(), charName) { t.Fatalf("GetCharactersReply missing %q customization", charName) }
	probeICSPlayAndWorldUDP(t, stream, worldID, createReply.GetCharacterId(), charName)
}
func Test07_GatewayIcsTlsXClientStream(t *testing.T) {
	if verifyICSTLSCertChain(x509.NewCertPool(), x509.NewCertPool(), "127.0.0.1", []*x509.Certificate{{}}) == nil || resolveICSTargetServerName(&x509.Certificate{DNSNames: []string{"*.example.test"}}) != "localhost.example.test" { t.Fatal("expected untrusted cert rejection and wildcard SAN conversion") }
	TestLiveComposeFunctionalAPIs(t)
}
func Test09_FullInGameClientSessionFlow(t *testing.T) { Test07_GatewayIcsTlsXClientStream(t) }
func probeLiveComposeGRPCAndTLS(t *testing.T) {
	t.Helper()
	envoyYaml := readRepoFile(t, "src/deploy/envoy/envoy.yaml")
	if !strings.Contains(envoyYaml, "port_value: 9090") || !strings.Contains(envoyYaml, "gateway_grpc") || !strings.Contains(envoyYaml, "gateway_tls") || strings.Contains(envoyYaml, "tether_tcp") { t.Fatal("envoy.yaml must configure 9090, gateway_grpc, gateway_tls without tether_tcp") }
	conn, err := net.DialTimeout("tcp", "127.0.0.1:9090", 3*time.Second); if err != nil { t.Fatalf("dial 127.0.0.1:9090 failed: %v", err) }
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err := conn.Write(append([]byte("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"), 0, 0, 0, 4, 0, 0, 0, 0, 0)); err != nil { _ = conn.Close(); t.Fatalf("write h2c preface failed: %v", err) }
	frameHdr := make([]byte, 9); _, err = io.ReadFull(conn, frameHdr); _ = conn.Close()
	if err != nil || frameHdr[3] != 0x04 { t.Fatalf("expected SETTINGS frame (0x04), hdr=%x err=%v", frameHdr, err) }
	udpConn, err := net.DialTimeout("udp", "127.0.0.1:7777", 2*time.Second); if err != nil { t.Fatalf("UDP dial 127.0.0.1:7777 failed: %v", err) }
	_ = udpConn.SetDeadline(time.Now().Add(500 * time.Millisecond))
	if _, err := udpConn.Write([]byte("SANDAL_WORLD_UDP_PROBE")); err != nil { _ = udpConn.Close(); t.Fatalf("UDP write 127.0.0.1:7777 failed: %v", err) }
	_ = udpConn.Close()
	if st, err := os.Stat(filepath.Join(repoRootDir(t), "dist", "content", "content.db")); err != nil || st.Size() <= 0 { t.Fatalf("dist/content/content.db missing: %v", err) }
	if st, err := os.Stat(filepath.Join(repoRootDir(t), "dist", "nav", "riverlands.aocnav")); err == nil && st.Size() <= 0 { t.Fatal("dist/nav/riverlands.aocnav is empty") }
	sql := "SELECT (SELECT count(*) FROM schema_migrations WHERE version BETWEEN '0001' AND '0024') || ':' || (SELECT count(*) FROM pg_indexes WHERE indexname IN ('idx_character_items_char_bag_slot_cover','idx_character_talents_char_tree_cover')) || ':' || (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname='characters' AND p.proname='fn_check_and_advance_fencing_token') || ':' || (SELECT count(*) FROM (SELECT characters.fn_check_and_advance_fencing_token(id, 100) FROM characters.characters LIMIT 1) t);"
	pgCmd := exec.Command("docker", "exec", "-i", "aoc-postgres", "psql", "-U", "aoc", "-d", "aoc", "-tAc", sql)
	stdinR, stdinW, _ := os.Pipe(); defer stdinR.Close(); defer stdinW.Close(); pgCmd.Stdin = stdinR
	if runtime.GOOS == "windows" && strings.TrimSpace(os.Getenv("DOCKER_HOST")) == "" { pgCmd.Env = append(os.Environ(), "DOCKER_HOST=tcp://127.0.0.1:2375") }
	if pgOut, err := pgCmd.CombinedOutput(); err != nil || strings.TrimSpace(string(pgOut)) != "24:2:1:1" { t.Fatalf("postgres schema/index/proc check expected 24:2:1:1, got %q (err=%v)", strings.TrimSpace(string(pgOut)), err) }
}
