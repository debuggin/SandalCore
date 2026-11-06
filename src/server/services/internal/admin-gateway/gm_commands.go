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
	if s.worldAdmin != nil {
		return s.worldAdmin
	}
	if strings.TrimSpace(s.worldAdminAddr) == "" {
		return nil
	}
	conn, err := grpc.NewClient(s.worldAdminAddr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil
	}
	s.worldAdminConn = conn
	s.worldAdmin = worldadminv1.NewWorldAdminServiceClient(conn)
	return s.worldAdmin
}

func (s *Server) getWorldStreamClient() worldadminv1.WorldStreamServiceClient {
	if s.worldStream != nil {
		return s.worldStream
	}
	s.grpcMu.Lock()
	defer s.grpcMu.Unlock()
	if s.worldStream != nil {
		return s.worldStream
	}
	if strings.TrimSpace(s.worldStreamAddr) == "" {
		return nil
	}
	conn, err := grpc.NewClient(s.worldStreamAddr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil
	}
	s.worldStreamConn = conn
	s.worldStream = worldadminv1.NewWorldStreamServiceClient(conn)
	return s.worldStream
}

func (s *Server) getCityNodeClient() citynodev1.CityNodeServiceClient {
	if s.cityNode != nil {
		return s.cityNode
	}
	s.grpcMu.Lock()
	defer s.grpcMu.Unlock()
	if s.cityNode != nil {
		return s.cityNode
	}
	if strings.TrimSpace(s.cityNodeAddr) == "" {
		return nil
	}
	conn, err := grpc.NewClient(s.cityNodeAddr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil
	}
	s.cityNodeConn = conn
	s.cityNode = citynodev1.NewCityNodeServiceClient(conn)
	return s.cityNode
}

func isTransientGRPCError(err error) bool {
	if err == nil {
		return false
	}
	st, ok := status.FromError(err)
	if !ok {
		return false
	}
	switch st.Code() {
	case codes.Unavailable, codes.DeadlineExceeded, codes.ResourceExhausted:
		return true
	default:
		return false
	}
}

func callWithRetry[T any](ctx context.Context, s *Server, op func(context.Context) (T, error)) (T, error) {
	var zero T
	var lastErr error
	const maxAttempts = 3
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		res, err := op(s.outgoingCtx(ctx))
		if err == nil {
			return res, nil
		}
		lastErr = err
		if attempt == maxAttempts || !isTransientGRPCError(err) {
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
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*citynodev1.ListNodesResponse, error) {
		return cli.ListNodes(c, &citynodev1.ListNodesRequest{})
	})
	if err != nil || resp == nil {
		return out
	}
	for _, n := range resp.GetNodes() {
		lv := int(n.GetLevel())
		if n.GetGuid() != 0 {
			out[strconv.FormatInt(n.GetGuid(), 10)] = lv
		}
		if n.GetName() != "" {
			out[strings.ToLower(n.GetName())] = lv
		}
		if n.GetDisplayName() != "" {
			out[strings.ToLower(n.GetDisplayName())] = lv
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
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.ListPlacesResponse, error) {
		return cli.ListPlaces(c, &worldadminv1.ListPlacesRequest{})
	})
	if err != nil || resp == nil {
		return nil
	}
	places := make([]admincatalog.PlaceEntry, 0, len(resp.GetPlaces()))
	for _, p := range resp.GetPlaces() {
		var x, y, z float64
		if pos := p.GetPosition(); pos != nil {
			x, y, z = pos.GetX(), pos.GetY(), pos.GetZ()
		}
		disp := firstNonEmpty(p.GetDisplayName(), p.GetName())
		places = append(places, admincatalog.PlaceEntry{
			GUID:        "place:" + strings.ToLower(p.GetName()),
			Name:        p.GetName(),
			DisplayName: disp,
			NodeName:    disp,
			NodeType:    firstNonEmpty(p.GetKind(), "place"),
			Kind:        firstNonEmpty(p.GetKind(), "place"),
			X:           x,
			Y:           y,
			Z:           z,
		})
	}
	return places
}

func (s *Server) handlePutWorldNode(w http.ResponseWriter, r *http.Request, _ Role) {
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
	if cmd == "" {
		writeProblem(w, r, http.StatusBadRequest, "missing-command", "Field 'command' is required.")
		return CommandRequest{}, false
	}
	params := make(map[string]any)
	for k, v := range rawMap {
		params[k] = v
	}
	if nested, ok := rawMap["params"].(map[string]any); ok {
		for k, v := range nested {
			params[k] = v
		}
	}
	targetID := extractStringParam(params, "targetId", "target_id", "character", "player", "node", "npc")
	zone := extractStringParam(params, "zone")
	reason := extractStringParam(params, "reason")
	return CommandRequest{Command: cmd, TargetID: targetID, Zone: zone, Params: params, Reason: reason}, true
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
		writeJSON(w, http.StatusOK, replay)
		return
	}
	s.mu.Unlock()

	result, err := s.executeRealGMCommand(r.Context(), req)
	if err != nil {
		writeGRPCProblem(w, r, err, "gm-command-failed")
		return
	}
	s.applyCommandSideEffect(req)
	s.recordMutation()

	receipt := CommandReceipt{
		ReceiptID:      "rcpt-" + hex.EncodeToString(pHash[:8]),
		IdempotencyKey: idemKey,
		Command:        req.Command,
		TargetID:       req.TargetID,
		Role:           role,
		Status:         "EXECUTED",
		ExecutedAtMs:   s.now().UnixMilli(),
		Result:         result,
	}
	s.mu.Lock()
	s.idemStore[idemKey] = idempotencyRecord{payloadHash: pHash, receipt: receipt}
	s.mu.Unlock()
	writeJSON(w, http.StatusOK, receipt)
}

func (s *Server) executeRealGMCommand(ctx context.Context, req CommandRequest) (map[string]any, error) {
	p := req.Params
	if p == nil {
		p = map[string]any{}
	}
	switch req.Command {
	case "teleport_player", "teleport":
		return s.execTeleportCommand(ctx, req.TargetID, p)
	case "give_money", "give_gold":
		return s.execGiveMoneyCommand(ctx, req.TargetID, p)
	case "grant_item", "give_item":
		return s.execGiveItemCommand(ctx, req.TargetID, p)
	case "spawn_npc":
		return s.execSpawnNpcCommand(ctx, req.TargetID, p)
	case "set_node_level", "set_node_stage":
		node := firstNonEmpty(extractStringParam(p, "node", "node_guid", "guid", "name"), req.TargetID)
		level, ok := extractIntParam(p, "level", "stage")
		if !ok || level < 0 || level > 6 {
			return nil, status.Error(codes.InvalidArgument, "level must be between 0 and 6")
		}
		return s.execSetNodeLevel(ctx, node, level)
	default:
		return map[string]any{"command": req.Command, "target_id": req.TargetID}, nil
	}
}

func (s *Server) execTeleportCommand(ctx context.Context, defaultChar string, p map[string]any) (map[string]any, error) {
	charName := firstNonEmpty(extractStringParam(p, "character", "player", "targetId", "target_id"), defaultChar)
	cli := s.getWorldAdminClient()
	if cli == nil {
		return map[string]any{"character": charName, "simulated": true}, nil
	}
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	tpReq, err := s.buildTeleportRequest(rctx, cli, charName, p)
	if err != nil {
		return nil, err
	}
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.TeleportResponse, error) {
		return cli.Teleport(c, tpReq)
	})
	if err != nil && tpReq.GetPlace() != "" {
		if fallbackPt, ok := resolveBuiltinPlaceCoords(tpReq.GetPlace()); ok {
			tpReq.Target = &worldadminv1.TeleportRequest_Point{Point: fallbackPt}
			resp, err = callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.TeleportResponse, error) {
				return cli.Teleport(c, tpReq)
			})
		}
	}
	if err != nil {
		return nil, err
	}
	var x, y, z float64
	if to := resp.GetTo(); to != nil {
		x, y, z = to.GetX(), to.GetY(), to.GetZ()
	}
	return map[string]any{"character": resp.GetCharacter(), "x": x, "y": y, "z": z, "place": resp.GetPlace()}, nil
}

func (s *Server) buildTeleportRequest(ctx context.Context, cli worldadminv1.WorldAdminServiceClient, charName string, p map[string]any) (*worldadminv1.TeleportRequest, error) {
	if charName == "" {
		return nil, status.Error(codes.InvalidArgument, "character is required for teleport_player")
	}
	if targetPlayer := extractStringParam(p, "target_player", "to_player"); targetPlayer != "" {
		listResp, err := callWithRetry(ctx, s, func(c context.Context) (*worldadminv1.ListPlayersResponse, error) {
			return cli.ListPlayers(c, &worldadminv1.ListPlayersRequest{})
		})
		if err != nil {
			return nil, err
		}
		for _, pl := range listResp.GetPlayers() {
			if strings.EqualFold(pl.GetName(), targetPlayer) || strings.EqualFold(pl.GetCharacterId(), targetPlayer) {
				pos := pl.GetPosition()
				var x, y, z float64
				if pos != nil {
					x, y, z = pos.GetX(), pos.GetY(), pos.GetZ()
				}
				return &worldadminv1.TeleportRequest{
					Character: charName,
					Target: &worldadminv1.TeleportRequest_Point{
						Point: &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: true},
					},
				}, nil
			}
		}
		return nil, status.Errorf(codes.NotFound, "target_player %q is not online", targetPlayer)
	}
	if place := extractStringParam(p, "place", "destination"); place != "" {
		return &worldadminv1.TeleportRequest{
			Character: charName,
			Target:    &worldadminv1.TeleportRequest_Place{Place: place},
		}, nil
	}
	x, hasX := extractFloatParam(p, "x_cm", "x")
	y, hasY := extractFloatParam(p, "y_cm", "y")
	z, hasZ := extractFloatParam(p, "z_cm", "z")
	if !hasX || !hasY {
		return nil, status.Error(codes.InvalidArgument, "teleport_player requires place, target_player, or (x_cm, y_cm)")
	}
	return &worldadminv1.TeleportRequest{
		Character: charName,
		Target: &worldadminv1.TeleportRequest_Point{
			Point: &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: hasZ},
		},
	}, nil
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
		copper = gold*10000 + silver*100 + copper
		hasCopper = true
	}
	if !hasCopper {
		return nil, status.Error(codes.InvalidArgument, "copper (or gold/silver) is required for give_money")
	}
	cli := s.getWorldAdminClient()
	if cli == nil {
		return map[string]any{"character": charName, "copper": copper, "simulated": true}, nil
	}
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.GiveMoneyResponse, error) {
		return cli.GiveMoney(c, &worldadminv1.GiveMoneyRequest{Character: charName, Copper: copper})
	})
	if err != nil {
		return nil, err
	}
	return map[string]any{
		"character": resp.GetCharacter(), "previous": resp.GetPrevious(),
		"balance": resp.GetBalance(), "added": resp.GetAdded(),
	}, nil
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
	rarityStr := extractRarityString(p)
	quality, _ := extractIntParam(p, "quality")
	cli := s.getWorldAdminClient()
	if cli == nil {
		return map[string]any{"character": charName, "item": item, "count": count, "simulated": true}, nil
	}
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.GiveItemResponse, error) {
		return cli.GiveItem(c, &worldadminv1.GiveItemRequest{
			Character: charName, Item: item, Count: int32(count), Rarity: rarityStr, Quality: int32(max(quality, 0)),
		})
	})
	if err != nil {
		return nil, err
	}
	return map[string]any{
		"character": resp.GetCharacter(), "item": resp.GetItem(),
		"item_guid": resp.GetItemGuid(), "container": resp.GetContainer(),
		"count": resp.GetCount(), "rarity": resp.GetRarity(), "quality": resp.GetQuality(),
	}, nil
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
	cli := s.getWorldAdminClient()
	if cli == nil {
		return map[string]any{"npc": npc, "level": level, "simulated": true}, nil
	}
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
		spReq.Target = &worldadminv1.SpawnNpcRequest_Point{
			Point: &worldadminv1.Point{X: x, Y: y, Z: z, HasZ: hasZ},
		}
	}
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) {
		return cli.SpawnNpc(c, spReq)
	})
	if err != nil && spReq.GetPlace() != "" {
		if fallbackPt, ok := resolveBuiltinPlaceCoords(spReq.GetPlace()); ok {
			spReq.Target = &worldadminv1.SpawnNpcRequest_Point{Point: fallbackPt}
			resp, err = callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) {
				return cli.SpawnNpc(c, spReq)
			})
		}
	}
	if err != nil {
		return nil, err
	}
	var x, y, z float64
	if pos := resp.GetPosition(); pos != nil {
		x, y, z = pos.GetX(), pos.GetY(), pos.GetZ()
	}
	return map[string]any{
		"id": resp.GetId(), "npc": resp.GetNpc(), "display_name": resp.GetDisplayName(),
		"asset_guid": resp.GetAssetGuid(), "level": resp.GetLevel(), "x": x, "y": y, "z": z,
	}, nil
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
			s.localNodeLevels[guidStr] = level
			s.localNodeLevels[strings.ToLower(spec.Name)] = level
			s.localNodeLevels[strings.ToLower(spec.DisplayName)] = level
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
		if err != nil {
			return nil, err
		}
		if n := resp.GetNode(); n != nil {
			lv := int(n.GetLevel())
			return map[string]any{
				"guid": strconv.FormatInt(n.GetGuid(), 10), "name": n.GetName(),
				"display_name": n.GetDisplayName(), "level": lv,
				"plug_tier": admincatalog.PlugTierForLevel(lv), "stage_name": admincatalog.StageNameForLevel(lv),
				"status": "UPDATED",
			}, nil
		}
	}
	return map[string]any{
		"node": node, "level": level,
		"plug_tier": admincatalog.PlugTierForLevel(level), "stage_name": admincatalog.StageNameForLevel(level),
		"status": "UPDATED",
	}, nil
}

func writeGRPCProblem(w http.ResponseWriter, r *http.Request, err error, defaultCode string) {
	st, ok := status.FromError(err)
	if !ok {
		writeProblem(w, r, http.StatusBadGateway, defaultCode, err.Error())
		return
	}
	httpStatus := http.StatusBadGateway
	switch st.Code() {
	case codes.InvalidArgument, codes.OutOfRange:
		httpStatus = http.StatusBadRequest
	case codes.NotFound:
		httpStatus = http.StatusNotFound
	case codes.AlreadyExists, codes.Aborted, codes.FailedPrecondition:
		httpStatus = http.StatusConflict
	case codes.PermissionDenied:
		httpStatus = http.StatusForbidden
	case codes.Unauthenticated:
		httpStatus = http.StatusUnauthorized
	case codes.Unavailable, codes.DeadlineExceeded:
		httpStatus = http.StatusServiceUnavailable
	}
	writeProblem(w, r, httpStatus, defaultCode, st.Message())
}

func extractStringParam(m map[string]any, keys ...string) string {
	for _, k := range keys {
		if v, ok := m[k]; ok && v != nil {
			switch tv := v.(type) {
			case string:
				if s := strings.TrimSpace(tv); s != "" {
					return s
				}
			case json.Number:
				return tv.String()
			case float64:
				return strconv.FormatInt(int64(tv), 10)
			}
		}
	}
	return ""
}

func extractIntParam(m map[string]any, keys ...string) (int, bool) {
	v, ok := extractInt64Param(m, keys...)
	return int(v), ok
}

func extractInt64Param(m map[string]any, keys ...string) (int64, bool) {
	for _, k := range keys {
		v, ok := m[k]
		if !ok || v == nil {
			continue
		}
		switch tv := v.(type) {
		case json.Number:
			if i, err := tv.Int64(); err == nil {
				return i, true
			}
			if f, err := tv.Float64(); err == nil {
				return int64(f), true
			}
		case float64:
			return int64(tv), true
		case int:
			return int64(tv), true
		case int64:
			return tv, true
		case string:
			if i, err := strconv.ParseInt(strings.TrimSpace(tv), 10, 64); err == nil {
				return i, true
			}
		}
	}
	return 0, false
}

func extractFloatParam(m map[string]any, keys ...string) (float64, bool) {
	for _, k := range keys {
		v, ok := m[k]
		if !ok || v == nil {
			continue
		}
		switch tv := v.(type) {
		case json.Number:
			if f, err := tv.Float64(); err == nil {
				return f, true
			}
		case float64:
			return tv, true
		case int:
			return float64(tv), true
		case string:
			if f, err := strconv.ParseFloat(strings.TrimSpace(tv), 64); err == nil {
				return f, true
			}
		}
	}
	return 0, false
}
