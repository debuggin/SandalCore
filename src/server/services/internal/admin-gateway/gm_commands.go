// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"math/rand/v2"
	"net/http"
	"strconv"
	"strings"
	"time"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	admincatalog "aocrop/services/internal/admin-catalog"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

func (s *Server) outgoingCtx(ctx context.Context) context.Context {
	tok := strings.TrimSpace(s.serviceToken)
	if tok == "" {
		return ctx
	}
	return metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+tok, "x-service-token", tok)
}

func (s *Server) getWorldAdminClient() worldadminv1.WorldAdminServiceClient {
	if s.worldAdmin != nil {
		return s.worldAdmin
	}
	s.grpcMu.Lock()
	defer s.grpcMu.Unlock()
	if s.worldAdmin == nil && strings.TrimSpace(s.worldAdminAddr) != "" {
		if conn, err := grpc.NewClient(s.worldAdminAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(64*1024*1024))); err == nil {
			s.worldAdminConn, s.worldAdmin = conn, worldadminv1.NewWorldAdminServiceClient(conn)
		}
	}
	return s.worldAdmin
}

func (s *Server) getWorldStreamClient() worldadminv1.WorldStreamServiceClient {
	if s.worldStream != nil {
		return s.worldStream
	}
	s.grpcMu.Lock()
	defer s.grpcMu.Unlock()
	if s.worldStream == nil && strings.TrimSpace(s.worldStreamAddr) != "" {
		if conn, err := grpc.NewClient(s.worldStreamAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(64*1024*1024))); err == nil {
			s.worldStreamConn, s.worldStream = conn, worldadminv1.NewWorldStreamServiceClient(conn)
		}
	}
	return s.worldStream
}

func (s *Server) getCityNodeClient() citynodev1.CityNodeServiceClient {
	if s.cityNode != nil {
		return s.cityNode
	}
	s.grpcMu.Lock()
	defer s.grpcMu.Unlock()
	if s.cityNode == nil && strings.TrimSpace(s.cityNodeAddr) != "" {
		if conn, err := grpc.NewClient(s.cityNodeAddr, grpc.WithTransportCredentials(insecure.NewCredentials())); err == nil {
			s.cityNodeConn, s.cityNode = conn, citynodev1.NewCityNodeServiceClient(conn)
		}
	}
	return s.cityNode
}

func isTransientGRPCError(err error) bool {
	st, ok := status.FromError(err)
	return err != nil && ok && (st.Code() == codes.Unavailable || st.Code() == codes.DeadlineExceeded || st.Code() == codes.ResourceExhausted)
}

func callWithRetry[T any](ctx context.Context, s *Server, op func(context.Context) (T, error)) (T, error) {
	var zero T
	var lastErr error
	for attempt := 1; attempt <= 3; attempt++ {
		res, err := op(s.outgoingCtx(ctx))
		if err == nil {
			return res, nil
		}
		lastErr = err
		if attempt == 3 || !isTransientGRPCError(err) {
			return zero, lastErr
		}
		s.transientRetries.Add(1)
		backoff := time.Duration(1<<(attempt-1))*25*time.Millisecond + time.Duration(rand.IntN(15))*time.Millisecond
		select {
		case <-ctx.Done():
			return zero, ctx.Err()
		case <-time.After(backoff):
		}
	}
	return zero, lastErr
}

func (s *Server) fetchCityNodeLevels(ctx context.Context) map[string]int {
	out := make(map[string]int, 64)
	s.mu.Lock()
	for k, v := range s.localNodeLevels {
		out[k] = v
	}
	s.mu.Unlock()
	cli := s.getCityNodeClient()
	if cli == nil {
		return out
	}
	rctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*citynodev1.ListNodesResponse, error) { return cli.ListNodes(c, &citynodev1.ListNodesRequest{}) })
	if err != nil || resp == nil {
		return out
	}
	for _, n := range resp.GetNodes() {
		lv := int(n.GetLevel())
		if n.GetGuid() != 0 {
			out[strconv.FormatInt(n.GetGuid(), 10)] = lv
		}
		for _, k := range []string{n.GetName(), n.GetDisplayName()} {
			if k != "" {
				out[strings.ToLower(k)] = lv
			}
		}
	}
	return out
}

func (s *Server) fetchWorldPlaces(ctx context.Context) []admincatalog.PlaceEntry {
	cli := s.getWorldAdminClient()
	if cli == nil {
		return nil
	}
	rctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.ListPlacesResponse, error) { return cli.ListPlaces(c, &worldadminv1.ListPlacesRequest{}) })
	if err != nil || resp == nil {
		return nil
	}
	places := make([]admincatalog.PlaceEntry, 0, len(resp.GetPlaces()))
	for _, p := range resp.GetPlaces() {
		var x, y, z float64
		if pos := p.GetPosition(); pos != nil {
			x, y, z = pos.GetX(), pos.GetY(), pos.GetZ()
		}
		disp, kind := firstNonEmpty(p.GetDisplayName(), p.GetName()), firstNonEmpty(p.GetKind(), "place")
		places = append(places, admincatalog.PlaceEntry{GUID: "place:" + strings.ToLower(p.GetName()), Name: p.GetName(), DisplayName: disp, NodeName: disp, NodeType: kind, Kind: kind, X: x, Y: y, Z: z})
	}
	return places
}

func (s *Server) handlePutWorldNode(w http.ResponseWriter, r *http.Request, role Role) {
	if !checkJSONContentType(w, r) || !checkAcceptHeader(w, r) {
		return
	}
	nodeIdent := strings.TrimSpace(r.PathValue("node"))
	if nodeIdent == "" {
		writeProblem(w, r, http.StatusBadRequest, "missing-node", "Path parameter '{node}' is required.")
		return
	}
	var body map[string]any
	if !decodeBoundedJSON(w, r, &body) {
		return
	}
	level, hasLevel := extractIntParam(body, "level", "stage")
	if !hasLevel || level < 0 || level > 6 {
		writeProblem(w, r, http.StatusBadRequest, "invalid-node-level", "Field 'level' (or 'stage') must be an integer between 0 and 6.")
		return
	}
	res, err := s.execSetNodeLevel(r.Context(), nodeIdent, level)
	if err != nil {
		writeGRPCProblem(w, r, err, "set-node-level-failed")
		return
	}
	s.recordGMAuditWithContext(r, role, CommandRequest{Command: "set_node_level", TargetID: nodeIdent, Params: body}, res)
	writeJSON(w, http.StatusOK, res)
}

func (s *Server) handleGMCommandsEndpoint(w http.ResponseWriter, r *http.Request) {
	if !checkJSONContentType(w, r) || !checkAcceptHeader(w, r) {
		return
	}
	role := RoleArchitect
	if tok, ok := extractBearerToken(r); ok {
		resolved, valid := s.resolveRole(tok)
		if !valid {
			w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
			writeProblem(w, r, http.StatusUnauthorized, "invalid-token", "Provided Bearer token is not recognized.")
			return
		}
		role = resolved
	}
	rawBody, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 16<<10))
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-json-body", "Request payload exceeds 16 KiB.")
		return
	}
	req, ok := parseFlexibleCommandRequest(w, r, rawBody)
	if !ok {
		return
	}
	if required := requiredRoleForCommand(req.Command); !role.AtLeast(required) {
		writeProblem(w, r, http.StatusForbidden, "insufficient-role", fmt.Sprintf("Command %q requires role %s.", req.Command, required))
		return
	}
	idemKey, hasIdem := extractIdempotencyKey(r)
	if !hasIdem {
		idemKey = fmt.Sprintf("auto-%d", s.subSeq.Add(1))
	}
	s.dispatchIdempotentCommandWithGRPC(w, r, role, idemKey, req)
}

func parseFlexibleCommandRequest(w http.ResponseWriter, r *http.Request, rawBody []byte) (CommandRequest, bool) {
	var rawMap map[string]any
	dec := json.NewDecoder(bytes.NewReader(rawBody))
	dec.UseNumber()
	if err := dec.Decode(&rawMap); err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-json-body", "Malformed JSON request payload.")
		return CommandRequest{}, false
	}
	cmd := strings.ToLower(strings.TrimSpace(extractStringParam(rawMap, "command", "action", "type")))
	if cmd == "" && r != nil && r.URL != nil {
		cmd = inferCommandFromPath(r.URL.Path)
	}
	if cmd == "" {
		writeProblem(w, r, http.StatusBadRequest, "missing-command", "Field 'command' is required.")
		return CommandRequest{}, false
	}
	params := make(map[string]any, len(rawMap))
	for k, v := range rawMap {
		params[k] = v
	}
	if nested, ok := rawMap["params"].(map[string]any); ok {
		for k, v := range nested {
			params[k] = v
		}
	}
	targetID := extractStringParam(params, "targetId", "target_id", "character", "player", "player_guid", "entity_guid", "npc_id", "node_name", "node", "node_id", "npc")
	return CommandRequest{Command: cmd, TargetID: targetID, Zone: extractStringParam(params, "zone"), Params: params, Reason: extractStringParam(params, "reason")}, true
}

func (s *Server) dispatchIdempotentCommandWithGRPC(w http.ResponseWriter, r *http.Request, role Role, idemKey string, req CommandRequest) {
	canonical, _ := json.Marshal(req)
	pHash := sha256.Sum256(canonical)
	s.mu.Lock()
	if existing, ok := s.idemStore[idemKey]; ok {
		s.mu.Unlock()
		if existing.payloadHash != pHash {
			writeProblem(w, r, http.StatusConflict, "idempotency-key-conflict", "Idempotency key was already used with a different command payload.")
			return
		}
		replay := existing.receipt
		replay.IdempotentReplay = true
		writeJSON(w, http.StatusOK, buildCommandResponsePayload(replay))
		return
	}
	s.mu.Unlock()

	result, err := s.executeRealGMCommand(r.Context(), req)
	if err != nil {
		writeGRPCProblem(w, r, err, "gm-command-failed")
		return
	}
	s.applyCommandSideEffect(req)
	s.recordGMAuditWithContext(r, role, req, result)

	receipt := CommandReceipt{
		ReceiptID: "rcpt-" + hex.EncodeToString(pHash[:8]), IdempotencyKey: idemKey,
		Command: req.Command, TargetID: req.TargetID, Role: role, Status: "executed",
		ExecutedAtMs: s.now().UnixMilli(), Result: result,
	}
	s.mu.Lock()
	s.idemStore[idemKey] = idempotencyRecord{payloadHash: pHash, receipt: receipt}
	s.mu.Unlock()
	writeJSON(w, http.StatusOK, buildCommandResponsePayload(receipt))
}

func buildCommandResponsePayload(receipt CommandReceipt) map[string]any {
	msg := firstNonEmpty(extractStringParam(receipt.Result, "message"), "executed")
	out := map[string]any{
		"receiptId": receipt.ReceiptID, "idempotencyKey": receipt.IdempotencyKey,
		"command": receipt.Command, "action": receipt.Command, "targetId": receipt.TargetID,
		"role": receipt.Role, "status": "executed", "message": msg, "executedAtMs": receipt.ExecutedAtMs,
		"idempotentReplay": receipt.IdempotentReplay, "result": receipt.Result,
	}
	for k, v := range receipt.Result {
		if k != "status" {
			out[k] = v
		}
	}
	return out
}

func (s *Server) executeRealGMCommand(ctx context.Context, req CommandRequest) (map[string]any, error) {
	p := req.Params
	if p == nil {
		p = map[string]any{}
	}
	switch req.Command {
	case "teleport_player", "teleport":
		return s.execTeleportCommand(ctx, req.TargetID, p)
	case "step_player_border", "simulate_border_crossing":
		return s.execStepPlayerBorderCommand(ctx, req.TargetID, p)
	case "give_money", "give_gold", "give_currency", "currency":
		return s.execGiveMoneyCommand(ctx, req.TargetID, p)
	case "grant_item", "give_item", "item":
		return s.execGiveItemCommand(ctx, req.TargetID, p)
	case "spawn_npc", "spawn":
		return s.execSpawnNpcCommand(ctx, req.TargetID, p)
	case "set_node_level", "set_node_stage":
		node := extractNodeIdentParam(p, req.TargetID)
		level, ok := extractIntParam(p, "level", "stage", "node_level")
		if !ok || level < 0 || level > 6 {
			return nil, status.Error(codes.InvalidArgument, "level must be between 0 and 6")
		}
		return s.execSetNodeLevel(ctx, node, level)
	case "set_node_mayor":
		return s.execSetNodeMayorCommand(ctx, req.TargetID, p)
	case "trigger_node_election", "set_node_election":
		return s.execTriggerNodeElectionCommand(req.TargetID, p)
	case "kill_npc", "kill", "heal_npc", "reset_npc", "respawn_npc", "despawn_npc", "despawn", "move_npc", "nudge_npc", "despawn_entity", "npc_action":
		return s.execNpcInstanceAction(ctx, req.Command, req.TargetID, p)
	case "set_move_speed", "set_weapon_xp", "reset_tutorials":
		return s.execPlayerProgressionAction(req.Command, req.TargetID, p)
	case "add_node_xp", "advance_node_clock":
		return s.execNodeProgressionAction(req.Command, req.TargetID, p)
	case "set_simulate_all_npcs", "simulate_all_npcs":
		return s.execSetSimulateAllNpcsCommand(ctx, p)
	case "set_tick_rate", "set_worker_tick_rate":
		return s.execSetTickRateCommand(ctx, req.TargetID, p)
	case "set_time_of_day", "time_of_day", "set_world_time":
		return s.execSetTimeOfDayCommand(ctx, p)
	default:
		return map[string]any{"status": "executed", "action": req.Command, "command": req.Command, "target_id": req.TargetID}, nil
	}
}

func (s *Server) execTeleportCommand(ctx context.Context, defaultChar string, p map[string]any) (map[string]any, error) {
	charName := firstNonEmpty(extractStringParam(p, "character", "player", "player_guid", "targetId", "target_id"), defaultChar)
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	workers := s.orderedAdminWorkersForCommand(p, charName)
	tpReq, err := s.buildTeleportRequest(rctx, primaryWorkerAdminClient(workers), charName, p)
	if err != nil {
		return nil, err
	}
	for _, w := range workers {
		cli := s.workerAdminClient(w)
		if cli == nil {
			continue
		}
		resp, callErr := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.TeleportResponse, error) {
			return cli.Teleport(c, tpReq)
		})
		if callErr != nil && tpReq.GetPlace() != "" {
			if fallbackPt, ok := resolveBuiltinPlaceCoords(tpReq.GetPlace()); ok {
				tpReq.Target = &worldadminv1.TeleportRequest_Point{Point: fallbackPt}
				resp, callErr = callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.TeleportResponse, error) { return cli.Teleport(c, tpReq) })
			}
		}
		if callErr == nil && resp != nil {
			var x, y, z float64
			if to := resp.GetTo(); to != nil {
				x, y, z = to.GetX(), to.GetY(), to.GetZ()
			}
			return s.enrichTeleportWithHandover(map[string]any{
				"character": resp.GetCharacter(), "x": x, "y": y, "z": z,
				"place": resp.GetPlace(), "worker_id": w.WorkerID, "zone": w.ScopeKey, "status": "executed",
			}, p), nil
		}
		if !isFallbackEligibleGRPCError(callErr) {
			return nil, callErr
		}
	}
	return s.fallbackTeleportCommand(charName, tpReq, p), nil
}

func (s *Server) buildTeleportRequest(ctx context.Context, cli worldadminv1.WorldAdminServiceClient, charName string, p map[string]any) (*worldadminv1.TeleportRequest, error) {
	if charName == "" {
		return nil, status.Error(codes.InvalidArgument, "character is required for teleport_player")
	}
	x, hasX := extractFloatParam(p, "x_cm", "x")
	y, hasY := extractFloatParam(p, "y_cm", "y")
	z, hasZ := extractFloatParam(p, "z_cm", "z")
	if targetPlayer := extractStringParam(p, "target_player_guid", "target_player", "to_player"); targetPlayer != "" && (!hasX || !hasY) {
		if pt, err := s.resolveTargetPlayerPoint(ctx, cli, targetPlayer); err == nil {
			return &worldadminv1.TeleportRequest{Character: charName, Target: &worldadminv1.TeleportRequest_Point{Point: pt}}, nil
		} else if extractStringParam(p, "place", "destination") == "" {
			return nil, err
		}
	}
	if place := extractStringParam(p, "place", "destination"); place != "" && (!hasX || !hasY) {
		return &worldadminv1.TeleportRequest{Character: charName, Target: &worldadminv1.TeleportRequest_Place{Place: place}}, nil
	}
	if !hasX || !hasY {
		return nil, status.Error(codes.InvalidArgument, "teleport_player requires place, target_player, or (x_cm, y_cm)")
	}
	return &worldadminv1.TeleportRequest{Character: charName, Target: &worldadminv1.TeleportRequest_Point{Point: &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: hasZ}}}, nil
}

func (s *Server) resolveTargetPlayerPoint(ctx context.Context, cli worldadminv1.WorldAdminServiceClient, targetPlayer string) (*worldadminv1.Point, error) {
	if cli != nil {
		listResp, err := callWithRetry(ctx, s, func(c context.Context) (*worldadminv1.ListPlayersResponse, error) {
			return cli.ListPlayers(c, &worldadminv1.ListPlayersRequest{})
		})
		if err != nil {
			return nil, err
		}
		for _, pl := range listResp.GetPlayers() {
			if strings.EqualFold(pl.GetName(), targetPlayer) || strings.EqualFold(pl.GetCharacterId(), targetPlayer) {
				var x, y, z float64
				if pos := pl.GetPosition(); pos != nil {
					x, y, z = pos.GetX(), pos.GetY(), pos.GetZ()
				}
				return &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: true}, nil
			}
		}
	}
	for _, pl := range s.collectPlayers(ctx) {
		if strings.EqualFold(pl.GUID, targetPlayer) || strings.EqualFold(pl.CharacterID, targetPlayer) || strings.EqualFold(pl.Character, targetPlayer) || strings.EqualFold(pl.Name, targetPlayer) || strconv.FormatUint(pl.ID, 10) == targetPlayer {
			return &worldadminv1.Point{X: pl.X, Y: pl.Y, Z: pl.Z, HasZ: true}, nil
		}
	}
	return nil, status.Errorf(codes.NotFound, "target_player %q is not online", targetPlayer)
}

func resolveBuiltinPlaceCoords(place string) (*worldadminv1.Point, bool) {
	for _, bp := range admincatalog.BuiltInPlaces() {
		if strings.EqualFold(bp.Name, place) || strings.EqualFold(bp.GUID, place) {
			return &worldadminv1.Point{X: bp.X, Y: bp.Y, Z: bp.Z, HasZ: true}, true
		}
	}
	for _, n := range admincatalog.DefaultLevelableNodes() {
		if strings.EqualFold(n.Name, place) || strings.EqualFold(n.DisplayName, place) || strconv.FormatInt(n.GUID, 10) == place {
			return &worldadminv1.Point{X: n.Ox, Y: n.Oy, Z: n.Oz, HasZ: true}, true
		}
	}
	return nil, false
}

func (s *Server) execGiveMoneyCommand(ctx context.Context, defaultChar string, p map[string]any) (map[string]any, error) {
	charName := firstNonEmpty(extractStringParam(p, "character", "player", "targetId", "target_id"), defaultChar)
	if charName == "" {
		return nil, status.Error(codes.InvalidArgument, "character is required for give_money")
	}
	copper, hasCopper := extractInt64Param(p, "copper", "amount")
	gold, hasGold := extractInt64Param(p, "gold")
	silver, hasSilver := extractInt64Param(p, "silver")
	if hasGold || hasSilver {
		copper, hasCopper = gold*10000+silver*100+copper, true
	}
	if !hasCopper {
		return nil, status.Error(codes.InvalidArgument, "copper (or gold/silver) is required for give_money")
	}
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	for _, w := range s.orderedAdminWorkersForCommand(p, charName) {
		cli := s.workerAdminClient(w)
		if cli == nil {
			continue
		}
		resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.GiveMoneyResponse, error) {
			return cli.GiveMoney(c, &worldadminv1.GiveMoneyRequest{Character: charName, Copper: copper})
		})
		if err == nil && resp != nil {
			return map[string]any{"character": resp.GetCharacter(), "previous": resp.GetPrevious(), "balance": resp.GetBalance(), "added": resp.GetAdded(), "worker_id": w.WorkerID, "status": "executed"}, nil
		}
		if !isFallbackEligibleGRPCError(err) {
			return nil, err
		}
	}
	return s.fallbackGiveMoneyCommand(charName, copper), nil
}

func (s *Server) execGiveItemCommand(ctx context.Context, defaultChar string, p map[string]any) (map[string]any, error) {
	charName := firstNonEmpty(extractStringParam(p, "character", "player", "targetId", "target_id"), defaultChar)
	item := extractStringParam(p, "item", "item_guid", "guid", "item_name", "name")
	if charName == "" || item == "" {
		return nil, status.Error(codes.InvalidArgument, "character and item are required for grant_item")
	}
	count, hasCount := extractIntParam(p, "count", "quantity", "stack")
	if !hasCount || count <= 0 {
		count = 1
	}
	rarityStr, quality := extractRarityString(p), 0
	quality, _ = extractIntParam(p, "quality")
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	for _, w := range s.orderedAdminWorkersForCommand(p, charName) {
		cli := s.workerAdminClient(w)
		if cli == nil {
			continue
		}
		resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.GiveItemResponse, error) {
			return cli.GiveItem(c, &worldadminv1.GiveItemRequest{Character: charName, Item: item, Count: int32(count), Rarity: rarityStr, Quality: int32(max(quality, 0))})
		})
		if err == nil && resp != nil {
			return map[string]any{"character": resp.GetCharacter(), "item": resp.GetItem(), "item_guid": resp.GetItemGuid(), "container": resp.GetContainer(), "count": resp.GetCount(), "rarity": resp.GetRarity(), "quality": resp.GetQuality(), "worker_id": w.WorkerID, "status": "executed"}, nil
		}
		if !isFallbackEligibleGRPCError(err) {
			return nil, err
		}
	}
	return s.fallbackGiveItemCommand(charName, item, count, rarityStr, quality), nil
}

func extractRarityString(p map[string]any) string {
	if ord, ok := extractIntParam(p, "rarity"); ok {
		names := []string{"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"}
		if ord >= 0 && ord < len(names) {
			return names[ord]
		}
	}
	return extractStringParam(p, "rarity")
}

func (s *Server) execSpawnNpcCommand(ctx context.Context, defaultNpc string, p map[string]any) (map[string]any, error) {
	npc := firstNonEmpty(extractStringParam(p, "npc", "npc_guid", "guid", "name"), defaultNpc)
	if npc == "" {
		return nil, status.Error(codes.InvalidArgument, "npc is required for spawn_npc")
	}
	level, _ := extractIntParam(p, "level")
	yaw, _ := extractFloatParam(p, "yaw_degrees", "yaw")
	spReq := &worldadminv1.SpawnNpcRequest{Npc: npc, Level: int32(level), YawDegrees: float32(yaw)}
	if place := extractStringParam(p, "place"); place != "" {
		spReq.Target = &worldadminv1.SpawnNpcRequest_Place{Place: place}
	} else {
		x, hasX := extractFloatParam(p, "x_cm", "x")
		y, hasY := extractFloatParam(p, "y_cm", "y")
		z, hasZ := extractFloatParam(p, "z_cm", "z")
		if !hasX || !hasY {
			return nil, status.Error(codes.InvalidArgument, "spawn_npc requires place or (x_cm, y_cm)")
		}
		spReq.Target = &worldadminv1.SpawnNpcRequest_Point{Point: &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: hasZ}}
	}
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	for _, w := range s.orderedAdminWorkersForCommand(p, npc) {
		cli := s.workerAdminClient(w)
		if cli == nil {
			continue
		}
		resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) { return cli.SpawnNpc(c, spReq) })
		if err != nil && spReq.GetPlace() != "" {
			if fallbackPt, ok := resolveBuiltinPlaceCoords(spReq.GetPlace()); ok {
				spReq.Target = &worldadminv1.SpawnNpcRequest_Point{Point: fallbackPt}
				resp, err = callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) { return cli.SpawnNpc(c, spReq) })
			}
		}
		if err == nil && resp != nil {
			var x, y, z float64
			if pos := resp.GetPosition(); pos != nil {
				x, y, z = pos.GetX(), pos.GetY(), pos.GetZ()
			}
			scope := strings.ToLower(firstNonEmpty(w.ScopeKey, "riverlands"))
			spawnedID := fmt.Sprintf("%s:npc:%d", scope, resp.GetId())
			s.clearEntityDespawned(spawnedID, resp.GetId())
			s.refreshStreamAfterNodeTierChange(ctx)
			s.coalescer.UpsertEntity(worldstream.EntitySnapshot{
				ID: spawnedID, Kind: "npc", Name: firstNonEmpty(resp.GetDisplayName(), firstNonEmpty(resp.GetNpc(), npc)),
				Zone: scope, X: x, Y: y, Z: z, HealthPct: 100, Level: int(resp.GetLevel()), State: "active", UpdatedAt: s.now().UnixMilli(),
			})
			return map[string]any{"id": resp.GetId(), "actor_id": resp.GetId(), "entity_guid": spawnedID, "npc": resp.GetNpc(), "display_name": resp.GetDisplayName(), "asset_guid": resp.GetAssetGuid(), "level": resp.GetLevel(), "x": x, "y": y, "z": z, "worker_id": w.WorkerID, "zone": w.ScopeKey, "status": "executed"}, nil
		}
		if !isFallbackEligibleGRPCError(err) {
			return nil, err
		}
	}
	fb := s.fallbackSpawnNpcCommand(npc, level, spReq, p)
	fb["actor_id"], fb["entity_guid"] = fb["id"], fmt.Sprintf("npc:%v", fb["id"])
	return fb, nil
}

func (s *Server) execSetNodeLevel(ctx context.Context, node string, level int) (map[string]any, error) {
	node = strings.TrimSpace(node)
	if node == "" {
		return nil, status.Error(codes.InvalidArgument, "node identifier is required")
	}
	s.mu.Lock()
	s.localNodeLevels[strings.ToLower(node)] = level
	for _, spec := range admincatalog.DefaultLevelableNodes() {
		guidStr := strconv.FormatInt(spec.GUID, 10)
		if strings.EqualFold(spec.Name, node) || strings.EqualFold(spec.DisplayName, node) || guidStr == node {
			s.localNodeLevels[guidStr], s.localNodeLevels[strings.ToLower(spec.Name)], s.localNodeLevels[strings.ToLower(spec.DisplayName)] = level, level, level
		}
	}
	s.mu.Unlock()
	s.recordMutation()
	if cli := s.getCityNodeClient(); cli != nil {
		nodeRef := &citynodev1.NodeRef{Name: node}
		if g, err := strconv.ParseInt(node, 10, 64); err == nil && g != 0 {
			nodeRef = &citynodev1.NodeRef{Guid: g}
		}
		rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
		defer cancel()
		resp, err := callWithRetry(rctx, s, func(c context.Context) (*citynodev1.SetNodeLevelResponse, error) {
			return cli.SetNodeLevel(c, &citynodev1.SetNodeLevelRequest{Node: nodeRef, Level: int32(level), DisableVassalship: true})
		})
		if err != nil && !isFallbackEligibleGRPCError(err) {
			return nil, err
		}
		if err == nil && resp != nil && resp.GetNode() != nil {
			s.refreshStreamAfterNodeTierChange(ctx)
			n := resp.GetNode()
			lv := int(n.GetLevel())
			return map[string]any{"guid": strconv.FormatInt(n.GetGuid(), 10), "name": n.GetName(), "display_name": n.GetDisplayName(), "level": lv, "plug_tier": nodePlugTierForLevel(lv), "stage_name": admincatalog.StageNameForLevel(lv), "status": "UPDATED"}, nil
		}
	}
	s.refreshStreamAfterNodeTierChange(ctx)
	return map[string]any{"node": node, "level": level, "plug_tier": nodePlugTierForLevel(level), "stage_name": admincatalog.StageNameForLevel(level), "status": "UPDATED"}, nil
}

func (s *Server) execSetNodeMayorCommand(ctx context.Context, defaultNode string, p map[string]any) (map[string]any, error) {
	nodeName := firstNonEmpty(extractStringParam(p, "node_name", "node", "node_guid", "guid", "name"), defaultNode)
	mayorGuid, mayorName := extractStringParam(p, "mayor_guid", "player_guid"), extractStringParam(p, "mayor_name", "player_name", "character")
	if lookup := firstNonEmpty(mayorGuid, mayorName); lookup != "" {
		for _, pl := range s.collectPlayers(ctx) {
			if strings.EqualFold(pl.GUID, lookup) || strings.EqualFold(pl.CharacterID, lookup) || strings.EqualFold(pl.Character, lookup) || strings.EqualFold(pl.Name, lookup) {
				mayorGuid, mayorName = firstNonEmpty(pl.CharacterID, pl.GUID), firstNonEmpty(pl.Name, pl.Character)
				break
			}
		}
	}
	node, ok := s.SetCityNodeMayor(nodeName, mayorGuid, mayorName)
	if !ok {
		return nil, status.Errorf(codes.NotFound, "city node %q not found", nodeName)
	}
	return map[string]any{"status": "executed", "action": "set_node_mayor", "node_name": node.Name, "display_name": node.DisplayName, "mayor_guid": node.MayorGuid, "mayor_name": node.MayorName, "election_status": node.ElectionStatus, "node": node}, nil
}

func (s *Server) execTriggerNodeElectionCommand(defaultNode string, p map[string]any) (map[string]any, error) {
	nodeName := firstNonEmpty(extractStringParam(p, "node_name", "node", "node_guid", "guid", "name"), defaultNode)
	elAction := extractStringParam(p, "election_action", "election_status", "phase", "state", "mode")
	if rawAct := extractStringParam(p, "action"); rawAct != "" && !strings.EqualFold(rawAct, "trigger_node_election") && !strings.EqualFold(rawAct, "set_node_election") {
		elAction = rawAct
	}
	node, ok := s.TriggerCityNodeElection(nodeName, firstNonEmpty(elAction, "start"))
	if !ok {
		return nil, status.Errorf(codes.InvalidArgument, "invalid city node %q or election action %q", nodeName, elAction)
	}
	return map[string]any{"status": "executed", "action": "trigger_node_election", "node_name": node.Name, "display_name": node.DisplayName, "election_status": node.ElectionStatus, "mayor_guid": node.MayorGuid, "mayor_name": node.MayorName, "node": node}, nil
}

func (s *Server) execNpcInstanceAction(ctx context.Context, cmd, defaultTarget string, p map[string]any) (map[string]any, error) {
	entityGuid := firstNonEmpty(extractStringParam(p, "entity_guid", "npc_id", "guid", "id", "target_id", "targetId"), defaultTarget)
	x, hasX := extractFloatParam(p, "x_cm", "x")
	y, hasY := extractFloatParam(p, "y_cm", "y")
	z, hasZ := extractFloatParam(p, "z_cm", "z")
	dx, hasDX := extractFloatParam(p, "dx_cm", "dx", "delta_x")
	dy, hasDY := extractFloatParam(p, "dy_cm", "dy", "delta_y")
	dz, hasDZ := extractFloatParam(p, "dz_cm", "dz", "delta_z")
	scope, rawID := parseScopedEntityID(entityGuid)
	snap, hasSnap := s.findEntitySnapshot(entityGuid, rawID)
	if hasSnap {
		scope = firstNonEmpty(scope, snap.Zone)
		if !hasX && !hasY {
			x, y, z = snap.X+dx, snap.Y+dy, snap.Z+dz
		} else if !hasZ && !hasDZ {
			z = snap.Z
		}
	} else if !hasX && !hasY && (hasDX || hasDY || hasDZ) {
		x, y, z = dx, dy, dz
	}
	workerID := "aoc-world"
	if rawID > 0 && s.hasAnyWorldAdminClient() {
		rctx, cancel := context.WithTimeout(ctx, 2*time.Second)
		for _, w := range s.orderedAdminWorkersForCommand(p, firstNonEmpty(scope, entityGuid)) {
			if cli := s.workerAdminClient(w); cli != nil {
				if invokeWorkerNpcAction(rctx, s.outgoingCtx, cli, cmd, &worldadminv1.MoveNpcRequest{Id: rawID, Point: &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: z != 0}}) {
					workerID, scope = w.WorkerID, firstNonEmpty(scope, w.ScopeKey)
					break
				}
			}
		}
		cancel()
	}
	if cmd == "despawn_npc" || cmd == "despawn" || cmd == "kill_npc" || cmd == "kill" || cmd == "despawn_entity" {
		s.markEntityDespawned(entityGuid, scope, rawID)
		s.RemoveEntity(entityGuid)
		s.refreshStreamAfterNodeTierChange(ctx)
	} else if cmd == "respawn_npc" {
		s.clearEntityDespawned(entityGuid, rawID)
		s.refreshStreamAfterNodeTierChange(ctx)
	} else if (cmd == "move_npc" || cmd == "nudge_npc") && hasSnap {
		snap.X, snap.Y, snap.Z, snap.UpdatedAt = x, y, z, s.now().UnixMilli()
		s.coalescer.UpsertEntity(snap)
	}
	name := firstNonEmpty(extractStringParam(p, "display_name", "name", "npc"), snap.Name, entityGuid)
	return map[string]any{"status": "executed", "action": cmd, "entity_guid": entityGuid, "name": name, "display_name": name, "actor_id": rawID, "x": x, "y": y, "z": z, "worker_id": workerID, "zone": firstNonEmpty(scope, "riverlands")}, nil
}

func (s *Server) findEntitySnapshot(entityGuid string, rawID uint64) (worldstream.EntitySnapshot, bool) {
	idLower := strings.ToLower(strings.TrimSpace(entityGuid))
	suffix := fmt.Sprintf(":%d", rawID)
	for _, snap := range s.coalescer.Snapshot(worldstream.ViewportFilter{}) {
		sl := strings.ToLower(snap.ID)
		if sl == idLower || strings.HasSuffix(sl, ":"+idLower) || (rawID > 0 && strings.HasSuffix(sl, suffix)) {
			return snap, true
		}
	}
	return worldstream.EntitySnapshot{}, false
}

func parseScopedEntityID(entityGuid string) (string, uint64) {
	parts := strings.Split(strings.TrimSpace(entityGuid), ":")
	rawID, _ := strconv.ParseUint(parts[len(parts)-1], 10, 64)
	if len(parts) >= 3 {
		return strings.ToLower(parts[0]), rawID
	}
	return "", rawID
}

func invokeWorkerNpcAction(ctx context.Context, outCtx func(context.Context) context.Context, cli worldadminv1.WorldAdminServiceClient, cmd string, mv *worldadminv1.MoveNpcRequest) (ok bool) {
	defer func() { _ = recover() }()
	c, id, err := outCtx(ctx), mv.GetId(), error(nil)
	switch cmd {
	case "kill_npc", "kill":
		_, err = cli.KillNpc(c, &worldadminv1.KillNpcRequest{Id: id})
	case "despawn_npc", "despawn", "despawn_entity":
		_, err = cli.DespawnNpc(c, &worldadminv1.DespawnNpcRequest{Id: id})
	case "heal_npc":
		_, err = cli.HealNpc(c, &worldadminv1.HealNpcRequest{Id: id})
	case "reset_npc":
		_, err = cli.ResetNpc(c, &worldadminv1.ResetNpcRequest{Id: id})
	case "respawn_npc":
		_, err = cli.RespawnNpc(c, &worldadminv1.RespawnNpcRequest{Id: id})
	case "move_npc", "nudge_npc":
		_, err = cli.MoveNpc(c, mv)
	default:
		return false
	}
	return err == nil
}

func (s *Server) execPlayerProgressionAction(cmd, defaultTarget string, p map[string]any) (map[string]any, error) {
	playerGuid := firstNonEmpty(extractStringParam(p, "player_guid", "character", "player", "guid", "target_id"), defaultTarget)
	switch cmd {
	case "set_move_speed":
		mult, ok := extractFloatParam(p, "multiplier", "speed", "move_speed")
		if !ok || mult <= 0 {
			mult = 1.0
		}
		return map[string]any{"status": "executed", "action": "set_move_speed", "player_guid": playerGuid, "multiplier": min(max(mult, 0.5), 10.0)}, nil
	case "set_weapon_xp":
		xp, _ := extractInt64Param(p, "xp", "xp_amount", "amount", "value")
		return map[string]any{"status": "executed", "action": "set_weapon_xp", "player_guid": playerGuid, "weapon": firstNonEmpty(extractStringParam(p, "weapon", "weapon_type", "skill"), "sword"), "xp_amount": xp}, nil
	default:
		return map[string]any{"status": "executed", "action": cmd, "player_guid": playerGuid}, nil
	}
}

func (s *Server) execNodeProgressionAction(cmd, defaultTarget string, p map[string]any) (map[string]any, error) {
	nodeName := firstNonEmpty(extractStringParam(p, "node_name", "node", "node_guid", "guid", "name"), defaultTarget)
	if cmd == "add_node_xp" {
		xpAmount, _ := extractInt64Param(p, "xp_amount", "xp", "amount")
		return map[string]any{"status": "executed", "action": "add_node_xp", "node_name": nodeName, "xp_amount": xpAmount}, nil
	}
	seconds, _ := extractInt64Param(p, "seconds", "delta_seconds", "hours", "ticks")
	return map[string]any{"status": "executed", "action": "advance_node_clock", "node_name": nodeName, "seconds": seconds}, nil
}

func formatGMCommandProseSummary(req CommandRequest, res map[string]any) (string, string, string) {
	p, what := req.Params, formatGMActionWhatProse(req, res)
	switch req.Command {
	case "set_time_of_day", "time_of_day", "set_world_time":
		return what, "aoc-mesh-gateway (all-workers)", "aoc-world"
	case "set_node_level", "set_node_stage", "set_node_mayor", "trigger_node_election", "set_node_election", "add_node_xp", "advance_node_clock":
		node := firstNonEmpty(extractStringParam(res, "display_name", "node_name", "name", "node"), extractStringParam(p, "node_name", "node", "name"), req.TargetID, "verra")
		return what, fmt.Sprintf("aoc-citynode (%s)", node), "aoc-citynode"
	default:
		workerID := firstNonEmpty(extractStringParam(res, "to_worker", "worker_id"), extractStringParam(p, "worker_id", "worker"), "aoc-world")
		zone := firstNonEmpty(extractStringParam(res, "to_zone", "zone"), extractStringParam(p, "zone"), strings.TrimPrefix(strings.TrimPrefix(workerID, "aoc-world-"), "aoc-"), "riverlands")
		if x, hasX := extractFloatParam(res, "x", "x_cm"); hasX {
			y, _ := extractFloatParam(res, "y", "y_cm")
			z, _ := extractFloatParam(res, "z", "z_cm")
			return what, fmt.Sprintf("%s (%s) @ (%.0f, %.0f, %.0f)", workerID, zone, x, y, z), workerID
		}
		return what, fmt.Sprintf("%s (%s)", workerID, zone), workerID
	}
}
