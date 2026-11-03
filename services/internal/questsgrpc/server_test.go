// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package questsgrpc

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	questv1 "aocrop/services/gen/aoc/quest/v1"
	"aocrop/services/internal/quest"
)

type colDef struct {
	name string
	oid   uint32
}

func pgFrame(typ byte, body []byte) []byte {
	out := make([]byte, 5+len(body))
	out[0] = typ
	binary.BigEndian.PutUint32(out[1:5], uint32(4+len(body)))
	copy(out[5:], body)
	return out
}

func pgRowDesc(cols []colDef) []byte {
	var buf bytes.Buffer
	_ = binary.Write(&buf, binary.BigEndian, int16(len(cols)))
	for _, c := range cols {
		buf.WriteString(c.name)
		buf.WriteByte(0)
		_ = binary.Write(&buf, binary.BigEndian, int32(0))
		_ = binary.Write(&buf, binary.BigEndian, int16(0))
		_ = binary.Write(&buf, binary.BigEndian, int32(c.oid))
		_ = binary.Write(&buf, binary.BigEndian, int16(-1))
		_ = binary.Write(&buf, binary.BigEndian, int32(-1))
		_ = binary.Write(&buf, binary.BigEndian, int16(0))
	}
	return pgFrame('T', buf.Bytes())
}

func pgDataRow(vals ...string) []byte {
	var buf bytes.Buffer
	_ = binary.Write(&buf, binary.BigEndian, int16(len(vals)))
	for _, v := range vals {
		_ = binary.Write(&buf, binary.BigEndian, int32(len(v)))
		buf.WriteString(v)
	}
	return pgFrame('D', buf.Bytes())
}

func pgReplyRows(cols []colDef, rows ...[]string) []byte {
	var out bytes.Buffer
	out.Write(pgRowDesc(cols))
	for _, r := range rows {
		out.Write(pgDataRow(r...))
	}
	out.Write(pgFrame('C', []byte("SELECT 1\x00")))
	out.Write(pgFrame('Z', []byte{'I'}))
	return out.Bytes()
}

func pgReplyQuery(sql string) []byte {
	if strings.Contains(sql, "db-fail") {
		errBody := []byte("SERROR\x00CXX000\x00Mboom\x00\x00")
		return append(pgFrame('E', errBody), pgFrame('Z', []byte{'I'})...)
	}
	if strings.Contains(sql, "FROM characters") {
		cols := []colDef{{name: "id", oid: 25}}
		if strings.Contains(sql, "missing") {
			return pgReplyRows(cols)
		}
		return pgReplyRows(cols, []string{"char-1"})
	}
	if strings.Contains(sql, "FROM character_quests ") {
		cols := []colDef{{"instance_guid", 25}, {"record_id", 20}, {"status", 21}, {"start_time", 701}, {"source_guid", 20}, {"objectives", 25}}
		return pgReplyRows(cols, []string{"00000000000000000000000000000001", "101", "1", "12.5", "0", `[{"guid":"00000000000000000000000000000002","record":201,"count":3}]`})
	}
	if strings.Contains(sql, "FROM character_quests_completed") {
		return pgReplyRows([]colDef{{"record_id", 20}, {"completions", 23}}, []string{"301", "2"})
	}
	if strings.Contains(sql, "FROM character_quest_variables") {
		return pgReplyRows([]colDef{{"name", 25}, {"value", 23}}, []string{"stage", "4"})
	}
	if strings.Contains(sql, "SELECT count(*) FROM character_quest_commands") {
		return pgReplyRows([]colDef{{"count", 20}}, []string{"0"})
	}
	if strings.Contains(sql, "INSERT INTO character_quest_commands") {
		return pgReplyRows([]colDef{{"id", 20}}, []string{"77"})
	}
	if strings.Contains(sql, "DELETE FROM character_quest_commands") {
		cols := []colDef{{"id", 20}, {"kind", 25}, {"quest", 25}, {"name", 25}, {"value", 23}}
		return pgReplyRows(cols, []string{"2", "finish", "q2", "", "0"}, []string{"1", "give", "q1", "", "0"})
	}
	return append(pgFrame('C', []byte("OK\x00")), pgFrame('Z', []byte{'I'})...)
}

func servePGConn(c net.Conn) {
	defer c.Close()
	var lenBuf [4]byte
	if _, err := io.ReadFull(c, lenBuf[:]); err != nil {
		return
	}
	n := binary.BigEndian.Uint32(lenBuf[:])
	if _, err := io.ReadFull(c, make([]byte, n-4)); err != nil {
		return
	}
	authOk := pgFrame('R', []byte{0, 0, 0, 0})
	stdStr := pgFrame('S', []byte("standard_conforming_strings\x00on\x00"))
	encUTF8 := pgFrame('S', []byte("client_encoding\x00UTF8\x00"))
	hello := append(append(append(authOk, stdStr...), encUTF8...), pgFrame('Z', []byte{'I'})...)
	if _, err := c.Write(hello); err != nil {
		return
	}
	var hdr [5]byte
	for {
		if _, err := io.ReadFull(c, hdr[:]); err != nil {
			return
		}
		size := binary.BigEndian.Uint32(hdr[1:5])
		body := make([]byte, size-4)
		if _, err := io.ReadFull(c, body); err != nil {
			return
		}
		if hdr[0] == 'X' {
			return
		}
		if hdr[0] == 'Q' {
			sql := strings.TrimRight(string(body), "\x00")
			_, _ = c.Write(pgReplyQuery(sql))
		} else {
			errBody := []byte("SERROR\x00CXX000\x00Munsupported frame\x00\x00")
			_, _ = c.Write(append(pgFrame('E', errBody), pgFrame('Z', []byte{'I'})...))
		}
	}
}

func newHermeticServer(t *testing.T) *Server {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("net.Listen: %v", err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go servePGConn(conn)
		}
	}()
	dsn := fmt.Sprintf("postgres://u:p@%s/aoc?sslmode=disable&default_query_exec_mode=simple_protocol", ln.Addr().String())
	pool, err := pgxpool.New(context.Background(), dsn)
	if err != nil {
		t.Fatalf("pgxpool.New: %v", err)
	}
	t.Cleanup(pool.Close)
	return New(quest.New(pool))
}

func TestLoadAndSaveLogHappyAndErrorPaths(t *testing.T) {
	srv := newHermeticServer(t)
	ctx := context.Background()

	loadResp, err := srv.LoadLog(ctx, &questv1.LoadLogRequest{CharacterId: "char-1"})
	if err != nil {
		t.Fatalf("LoadLog happy path failed: %v", err)
	}
	if len(loadResp.GetLog().GetInstances()) != 1 || len(loadResp.GetLog().GetCompleted()) != 1 || len(loadResp.GetLog().GetVariables()) != 1 {
		t.Fatalf("unexpected loaded log: %+v", loadResp.GetLog())
	}

	if _, err := srv.SaveLog(ctx, &questv1.SaveLogRequest{CharacterId: "char-1", Log: loadResp.GetLog()}); err != nil {
		t.Fatalf("SaveLog happy path failed: %v", err)
	}

	if _, err := srv.LoadLog(ctx, &questv1.LoadLogRequest{CharacterId: "missing"}); status.Code(err) != codes.NotFound {
		t.Fatalf("expected NotFound for missing character, got %v", err)
	}
	if _, err := srv.SaveLog(ctx, &questv1.SaveLogRequest{CharacterId: ""}); status.Code(err) != codes.InvalidArgument {
		t.Fatalf("expected InvalidArgument for empty character on SaveLog, got %v", err)
	}
}

func TestQueueAndTakeCommandsAndStatusMapping(t *testing.T) {
	srv := newHermeticServer(t)
	ctx := context.Background()

	qResp, err := srv.QueueCommand(ctx, &questv1.QueueCommandRequest{CharacterId: "char-1", Kind: "give", Quest: "q1"})
	if err != nil || qResp.GetId() != 77 {
		t.Fatalf("QueueCommand got (%+v, %v), want id=77", qResp, err)
	}
	if _, err := srv.QueueCommand(ctx, &questv1.QueueCommandRequest{CharacterId: "char-1", Kind: "bogus"}); status.Code(err) != codes.InvalidArgument {
		t.Fatalf("expected InvalidArgument for bogus command kind, got %v", err)
	}

	takeResp, err := srv.TakeCommands(ctx, &questv1.TakeCommandsRequest{CharacterId: "char-1"})
	if err != nil || len(takeResp.GetCommands()) != 2 {
		t.Fatalf("TakeCommands got (%+v, %v)", takeResp, err)
	}
	if takeResp.GetCommands()[0].GetId() != 1 || takeResp.GetCommands()[1].GetId() != 2 {
		t.Fatalf("expected commands sorted oldest-first [1,2], got %+v", takeResp.GetCommands())
	}
	if _, err := srv.TakeCommands(ctx, &questv1.TakeCommandsRequest{CharacterId: "db-fail"}); status.Code(err) != codes.Internal {
		t.Fatalf("expected Internal for db failure, got %v", err)
	}

	cctx, cancel := context.WithCancel(ctx)
	cancel()
	if got := status.Code(toStatus(cctx, errors.New("canceled io"))); got != codes.Canceled {
		t.Fatalf("expected Canceled from toStatus, got %v", got)
	}
}
