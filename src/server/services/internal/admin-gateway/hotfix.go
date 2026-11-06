// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"unicode"
)

var (
	allowedHotfixTables = map[string]struct{}{
		"world_hotfix_overlays": {}, "spawns": {}, "loot_tables": {}, "abilities": {}, "items": {}, "npcs": {},
	}
	forbiddenSQLKeywords = map[string]struct{}{
		"DROP": {}, "TRUNCATE": {}, "ALTER": {}, "ATTACH": {}, "DETACH": {},
		"PRAGMA": {}, "VACUUM": {}, "CREATE": {}, "GRANT": {}, "REVOKE": {},
	}
)

type HotfixRowDiff struct {
	Table      string `json:"table"`
	PrimaryKey string `json:"primary_key"`
	Column     string `json:"column"`
	OldValue   string `json:"old_value"`
	NewValue   string `json:"new_value"`
}

type HotfixDryRunResult struct {
	Valid            bool            `json:"valid"`
	StatementType    string          `json:"statement_type"`
	TargetTable      string          `json:"target_table"`
	ExplainPlan      []string        `json:"explain_plan"`
	EstimatedRows    int             `json:"estimated_rows"`
	RowDiffs         []HotfixRowDiff `json:"row_diffs"`
	GeneratedUndoSQL string          `json:"generated_undo_sql"`
	SnapshotEpoch    uint64          `json:"snapshot_epoch"`
}

type HotfixRequest struct {
	SQL          string `json:"sql"`
	SQLText      string `json:"sql_text,omitempty"`
	Reason       string `json:"reason,omitempty"`
	TargetEntity string `json:"target_entity,omitempty"`
	ActorSub     string `json:"actor_sub,omitempty"`
}

type HotfixApplyReceipt struct {
	AuditID          string          `json:"audit_id"`
	SnapshotEpoch    uint64          `json:"snapshot_epoch"`
	UndoSQL          string          `json:"undo_sql"`
	DiffSummary      string          `json:"diff_summary"`
	ReloadTriggered  bool            `json:"reload_triggered"`
	AppliedAt        string          `json:"applied_at"`
	RowDiffs         []HotfixRowDiff `json:"row_diffs"`
	IdempotentReplay bool            `json:"idempotent_replay"`
}

type AuditRollbackRequest struct {
	AuditID         string `json:"audit_id"`
	ExpectedUndoSQL string `json:"expected_undo_sql,omitempty"`
	Reason          string `json:"reason,omitempty"`
	ActorSub        string `json:"actor_sub,omitempty"`
}

type AuditRollbackReceipt struct {
	AuditID           string `json:"audit_id"`
	RollbackAuditID   string `json:"rollback_audit_id"`
	RolledBackAuditID string `json:"rolled_back_audit_id"`
	UndoSQL           string `json:"undo_sql"`
	ExecutedUndoSQL   string `json:"executed_undo_sql"`
	SnapshotEpoch     uint64 `json:"snapshot_epoch"`
	AppliedAt         string `json:"applied_at"`
	RolledBack        bool   `json:"rolled_back"`
	IdempotentReplay  bool   `json:"idempotent_replay"`
}

type parsedHotfixSQL struct {
	stmtType, table, pkColumn, pkValue, colName, newValue, normalized string
}

// ValidateHotfixSQL enforces single-statement DML bounds against the allowlisted hotfix overlay tables.
func ValidateHotfixSQL(sqlText string) error {
	_, err := parseHotfixSQL(sqlText)
	return err
}

func parseHotfixSQL(sqlText string) (parsedHotfixSQL, error) {
	trimmed := strings.TrimSpace(strings.TrimSuffix(strings.TrimSpace(sqlText), ";"))
	if trimmed == "" || strings.Contains(trimmed, ";") || strings.Contains(trimmed, "--") || strings.Contains(trimmed, "/*") {
		return parsedHotfixSQL{}, errors.New("hotfix SQL must be a non-empty single statement without comments")
	}
	words := strings.FieldsFunc(trimmed, func(r rune) bool { return !unicode.IsLetter(r) && !unicode.IsDigit(r) && r != '_' })
	if len(words) < 3 {
		return parsedHotfixSQL{}, errors.New("incomplete SQL statement")
	}
	for _, w := range words {
		if _, bad := forbiddenSQLKeywords[strings.ToUpper(w)]; bad {
			return parsedHotfixSQL{}, fmt.Errorf("forbidden SQL keyword %q is not permitted", strings.ToUpper(w))
		}
	}
	return extractHotfixTarget(trimmed, words)
}

func extractHotfixTarget(trimmed string, words []string) (parsedHotfixSQL, error) {
	verb, table := strings.ToUpper(words[0]), ""
	switch verb {
	case "UPDATE":
		if !hasWordFold(words, "SET") || !hasWordFold(words, "WHERE") {
			return parsedHotfixSQL{}, errors.New("UPDATE hotfix requires explicit SET and WHERE clauses")
		}
		table = strings.ToLower(words[1])
	case "INSERT", "REPLACE":
		idx := 1
		if strings.EqualFold(words[1], "OR") && len(words) > 4 {
			idx = 3
		}
		if idx+1 >= len(words) || !strings.EqualFold(words[idx], "INTO") {
			return parsedHotfixSQL{}, errors.New("INSERT/REPLACE hotfix requires INTO <table>")
		}
		table = strings.ToLower(words[idx+1])
	case "DELETE":
		if !strings.EqualFold(words[1], "FROM") || !hasWordFold(words, "WHERE") || len(words) < 4 {
			return parsedHotfixSQL{}, errors.New("DELETE hotfix requires FROM <table> and WHERE clause")
		}
		table = strings.ToLower(words[2])
	default:
		return parsedHotfixSQL{}, fmt.Errorf("unsupported SQL statement verb %q", verb)
	}
	if _, ok := allowedHotfixTables[table]; !ok {
		return parsedHotfixSQL{}, fmt.Errorf("table %q is not in the allowed hotfix overlay allowlist", table)
	}
	pkCol, pkVal, colName, newVal := parseClauseAssignments(trimmed, table)
	return parsedHotfixSQL{verb, table, pkCol, pkVal, colName, newVal, trimmed}, nil
}

func hasWordFold(words []string, target string) bool {
	return strings.Contains(" "+strings.ToUpper(strings.Join(words, " "))+" ", " "+strings.ToUpper(target)+" ")
}

func parseClauseAssignments(sqlText, table string) (string, string, string, string) {
	pkCol, pkVal, colName, newVal := "record_guid", table+":default", "overlay_value", "APPLIED"
	upper := strings.ToUpper(sqlText)
	if wIdx := strings.Index(upper, "WHERE "); wIdx >= 0 {
		pkCol, pkVal = splitFirstEq(sqlText[wIdx+6:], pkCol, pkVal)
	}
	if sIdx := strings.Index(upper, "SET "); sIdx >= 0 {
		end := len(sqlText)
		if wIdx := strings.Index(upper, "WHERE "); wIdx > sIdx {
			end = wIdx
		}
		colName, newVal = splitFirstEq(sqlText[sIdx+4:end], colName, newVal)
	}
	return pkCol, pkVal, colName, newVal
}

func splitFirstEq(seg, defK, defV string) (string, string) {
	for _, sep := range []string{" AND ", " and ", ","} {
		if i := strings.Index(seg, sep); i >= 0 {
			seg = seg[:i]
		}
	}
	if p := strings.SplitN(seg, "=", 2); len(p) == 2 && strings.TrimSpace(p[0]) != "" {
		return strings.TrimSpace(p[0]), strings.Trim(strings.TrimSpace(p[1]), "'\"")
	}
	return defK, defV
}

func (s *Server) simulateDryRunLocked(p parsedHotfixSQL) HotfixDryRunResult {
	oldVal, newVal := "BASELINE", p.newValue
	if prev, ok := s.overlayState[p.table+":"+p.pkValue+":"+p.colName]; ok {
		oldVal = prev.NewValue
	}
	undo := fmt.Sprintf("UPDATE %s SET %s = '%s' WHERE %s = '%s';", p.table, p.colName, oldVal, p.pkColumn, p.pkValue)
	if p.stmtType == "INSERT" || p.stmtType == "REPLACE" {
		undo = fmt.Sprintf("DELETE FROM %s WHERE %s = '%s';", p.table, p.pkColumn, p.pkValue)
	} else if p.stmtType == "DELETE" {
		newVal = "DELETED"
		undo = fmt.Sprintf("INSERT INTO %s (%s, %s) VALUES ('%s', '%s');", p.table, p.pkColumn, p.colName, p.pkValue, oldVal)
	}
	diff := HotfixRowDiff{Table: p.table, PrimaryKey: p.pkValue, Column: p.colName, OldValue: oldVal, NewValue: newVal}
	plan := []string{"BEGIN IMMEDIATE", "EXPLAIN QUERY PLAN " + p.normalized, fmt.Sprintf("SEARCH TABLE %s USING PRIMARY KEY (%s=?)", p.table, p.pkColumn), "ROLLBACK"}
	return HotfixDryRunResult{Valid: true, StatementType: p.stmtType, TargetTable: p.table, ExplainPlan: plan, EstimatedRows: 1, RowDiffs: []HotfixRowDiff{diff}, GeneratedUndoSQL: undo, SnapshotEpoch: s.snapshotEpoch.Load()}
}

func (s *Server) HandleHotfixDryRun(w http.ResponseWriter, r *http.Request) {
	var req HotfixRequest
	if !decodeBoundedJSON(w, r, &req) {
		return
	}
	parsed, err := parseHotfixSQL(firstNonEmpty(req.SQL, req.SQLText))
	if err != nil {
		writeProblem(w, r, http.StatusUnprocessableEntity, "invalid-hotfix-sql", err.Error())
		return
	}
	s.mu.Lock()
	res := s.simulateDryRunLocked(parsed)
	s.mu.Unlock()
	writeJSON(w, http.StatusOK, res)
}

func (s *Server) HandleHotfixApply(w http.ResponseWriter, r *http.Request) {
	idemKey, ok := extractIdempotencyKey(r)
	if !ok {
		writeProblem(w, r, http.StatusBadRequest, "missing-idempotency-key", "Header 'Idempotency-Key' or 'X-Idempotency-Key' is required.")
		return
	}
	var req HotfixRequest
	if !decodeBoundedJSON(w, r, &req) {
		return
	}
	parsed, err := parseHotfixSQL(firstNonEmpty(req.SQL, req.SQLText))
	if err != nil {
		writeProblem(w, r, http.StatusUnprocessableEntity, "invalid-hotfix-sql", err.Error())
		return
	}
	pHash := sha256.Sum256([]byte("apply:" + parsed.normalized))
	s.mu.Lock()
	defer s.mu.Unlock()
	if existing, exists := s.hotfixIdem[idemKey]; exists {
		if existing.payloadHash != pHash || existing.applyReceipt == nil {
			writeProblem(w, r, http.StatusConflict, "idempotency-key-conflict", "Idempotency key conflict.")
			return
		}
		replay := *existing.applyReceipt
		replay.IdempotentReplay = true
		writeJSON(w, http.StatusOK, replay)
		return
	}
	writeJSON(w, http.StatusOK, s.commitHotfixApplyLocked(parsed, req.ActorSub, idemKey, pHash))
}

func (s *Server) commitHotfixApplyLocked(p parsedHotfixSQL, actorSub, idemKey string, pHash [32]byte) HotfixApplyReceipt {
	sim := s.simulateDryRunLocked(p)
	diff := sim.RowDiffs[0]
	s.overlayState[p.table+":"+p.pkValue+":"+p.colName] = diff
	epoch, auditID, appliedAt := s.snapshotEpoch.Add(1), fmt.Sprintf("audit-%d-%s", s.auditSeq.Add(1), hex.EncodeToString(pHash[:4])), s.now().UTC().Format("2006-01-02T15:04:05Z")
	s.reloadCount.Add(1)
	summary := fmt.Sprintf("%s.%s (%s): %s -> %s", diff.Table, diff.Column, diff.PrimaryKey, diff.OldValue, diff.NewValue)
	s.auditLog = append(s.auditLog, AuditEntry{AuditID: auditID, ActorSub: firstNonEmpty(actorSub, "gm-architect"), ActorRole: RoleArchitect, CommandDomain: "Hotfix/SQL", ActionName: "HotfixApply:" + p.stmtType, TargetEntityID: diff.PrimaryKey, SQLText: p.normalized, UndoSQL: sim.GeneratedUndoSQL, DiffSummary: summary, SnapshotEpoch: epoch, AppliedAt: appliedAt})
	receipt := HotfixApplyReceipt{AuditID: auditID, SnapshotEpoch: epoch, UndoSQL: sim.GeneratedUndoSQL, DiffSummary: summary, ReloadTriggered: true, AppliedAt: appliedAt, RowDiffs: sim.RowDiffs}
	s.hotfixIdem[idemKey] = hotfixIdemRecord{payloadHash: pHash, applyReceipt: &receipt}
	return receipt
}

func (s *Server) HandleAuditRollback(w http.ResponseWriter, r *http.Request) {
	idemKey, ok := extractIdempotencyKey(r)
	if !ok {
		writeProblem(w, r, http.StatusBadRequest, "missing-idempotency-key", "Header 'Idempotency-Key' or 'X-Idempotency-Key' is required.")
		return
	}
	var req AuditRollbackRequest
	if !decodeBoundedJSON(w, r, &req) {
		return
	}
	if strings.TrimSpace(req.AuditID) == "" {
		writeProblem(w, r, http.StatusBadRequest, "missing-audit-id", "Field 'audit_id' is required.")
		return
	}
	pHash := sha256.Sum256([]byte("rollback:" + strings.TrimSpace(req.AuditID) + ":" + strings.TrimSpace(req.ExpectedUndoSQL)))
	s.mu.Lock()
	defer s.mu.Unlock()
	if existing, exists := s.hotfixIdem[idemKey]; exists {
		if existing.payloadHash != pHash || existing.rollbackReceipt == nil {
			writeProblem(w, r, http.StatusConflict, "idempotency-key-conflict", "Idempotency key conflict.")
			return
		}
		replay := *existing.rollbackReceipt
		replay.IdempotentReplay = true
		writeJSON(w, http.StatusOK, replay)
		return
	}
	if receipt, status, code, detail := s.commitAuditRollbackLocked(req, idemKey, pHash); status != http.StatusOK {
		writeProblem(w, r, status, code, detail)
	} else {
		writeJSON(w, http.StatusOK, receipt)
	}
}

func (s *Server) commitAuditRollbackLocked(req AuditRollbackRequest, idemKey string, pHash [32]byte) (AuditRollbackReceipt, int, string, string) {
	var target *AuditEntry
	for i := range s.auditLog {
		if s.auditLog[i].AuditID == strings.TrimSpace(req.AuditID) {
			target = &s.auditLog[i]
			break
		}
	}
	if target == nil {
		return AuditRollbackReceipt{}, http.StatusNotFound, "audit-not-found", "Target audit_id was not found."
	}
	if target.RolledBack || strings.TrimSpace(target.UndoSQL) == "" {
		return AuditRollbackReceipt{}, http.StatusConflict, "rollback-rejected", "Audit entry is already rolled back or missing undo_sql."
	}
	if exp := strings.TrimSpace(req.ExpectedUndoSQL); exp != "" && exp != target.UndoSQL {
		return AuditRollbackReceipt{}, http.StatusConflict, "undo-sql-mismatch", "expected_undo_sql mismatch."
	}
	parsedUndo, err := parseHotfixSQL(target.UndoSQL)
	if err != nil {
		return AuditRollbackReceipt{}, http.StatusUnprocessableEntity, "invalid-undo-sql", err.Error()
	}
	sim := s.simulateDryRunLocked(parsedUndo)
	s.overlayState[parsedUndo.table+":"+parsedUndo.pkValue+":"+parsedUndo.colName] = sim.RowDiffs[0]
	target.RolledBack = true
	epoch, rbID, appliedAt := s.snapshotEpoch.Add(1), fmt.Sprintf("audit-rb-%d-%s", s.auditSeq.Add(1), hex.EncodeToString(pHash[:4])), s.now().UTC().Format("2006-01-02T15:04:05Z")
	s.reloadCount.Add(1)
	s.auditLog = append(s.auditLog, AuditEntry{AuditID: rbID, ActorSub: "gm-architect", ActorRole: RoleArchitect, CommandDomain: "Hotfix/Rollback", ActionName: "AuditRollback", TargetEntityID: target.TargetEntityID, SQLText: target.UndoSQL, DiffSummary: "Rolled back " + target.AuditID, SnapshotEpoch: epoch, AppliedAt: appliedAt, RolledBackAuditID: target.AuditID})
	receipt := AuditRollbackReceipt{AuditID: rbID, RollbackAuditID: rbID, RolledBackAuditID: target.AuditID, UndoSQL: target.UndoSQL, ExecutedUndoSQL: target.UndoSQL, SnapshotEpoch: epoch, AppliedAt: appliedAt, RolledBack: true}
	s.hotfixIdem[idemKey] = hotfixIdemRecord{payloadHash: pHash, rollbackReceipt: &receipt}
	return receipt, http.StatusOK, "", ""
}
