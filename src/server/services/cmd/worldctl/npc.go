// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"fmt"
	"io"
	"strconv"
	"strings"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
)

// worldctl npc: a game master's tools on one creature, named by its actor id (the number the world stream and Inspect give; `worldctl watch` does not print ids, the admin app
// does). Every action answers with the one plain sentence worldd says of what it did.
//
//	worldctl npc kill <id>               kills it as a blow would: a corpse, the loot a kill gives (open to every player in the world, no experience), its usual respawn timer
//	worldctl npc respawn <id>            a dead creature (corpse or only its timer left) is made again at its spawn point now; a living one is replaced by a fresh one there
//	worldctl npc reset <id>              back to its spawn point at a run, at full health, its threat cleared, its leash centred on the spawn point again
//	worldctl npc heal <id>               full health
//	worldctl npc despawn <id>            takes it out of the world now; its spawn point makes it again on its usual timer
//	worldctl npc move <id> <x> <y> [<z>] stands it at the point (cm; without z, the ground under it); its leash and its walk home are centred there, its spawn point stays
//	worldctl npc spawn <def_or_name> <x> <y> [<z>] [--level N]  spawns a creature by record name, display name, or Guid at (x, y[, z])

const npcHelp = "worldctl npc kill|respawn|reset|heal|despawn <id> | worldctl npc move <id> <x> <y> [<z>] | worldctl npc spawn <def_or_name> <x> <y> [<z>] [--level N]"

// parseNpc checks the arguments of `worldctl npc` and returns the call that runs it.
func parseNpc(pos []string) (call, error) {
	if len(pos) >= 1 && pos[0] == "spawn" {
		return parseNpcSpawn(pos[1:])
	}
	if len(pos) < 2 {
		return nil, usageError("needs an action and a mob's actor id: " + npcHelp)
	}
	action := pos[0]
	id, err := strconv.ParseUint(strings.TrimSpace(pos[1]), 10, 64)
	if err != nil || id == 0 {
		return nil, usageError(fmt.Sprintf("%q is not an actor id (the number the world stream and Inspect give): %s", pos[1], npcHelp))
	}
	if action != "move" && len(pos) != 2 {
		return nil, usageError(fmt.Sprintf("takes only the mob's id after %q: %s", action, npcHelp))
	}
	say := func(out io.Writer, s string, err error) error {
		if err != nil {
			return err
		}
		_, werr := fmt.Fprintln(out, s)
		return werr
	}
	switch action {
	case "kill":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.KillNpc(ctx, &worldadminv1.KillNpcRequest{Id: id})
			return say(out, r.GetResult(), err)
		}, nil
	case "respawn":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.RespawnNpc(ctx, &worldadminv1.RespawnNpcRequest{Id: id})
			return say(out, r.GetResult(), err)
		}, nil
	case "reset":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.ResetNpc(ctx, &worldadminv1.ResetNpcRequest{Id: id})
			return say(out, r.GetResult(), err)
		}, nil
	case "heal":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.HealNpc(ctx, &worldadminv1.HealNpcRequest{Id: id})
			return say(out, r.GetResult(), err)
		}, nil
	case "despawn":
		return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
			r, err := c.DespawnNpc(ctx, &worldadminv1.DespawnNpcRequest{Id: id})
			return say(out, r.GetResult(), err)
		}, nil
	case "move":
		return parseNpcMove(id, pos[2:])
	}
	return nil, usageError(fmt.Sprintf("%q is not an NPC action: %s", action, npcHelp))
}

func parseNpcMove(id uint64, coords []string) (call, error) {
	if len(coords) < 2 || len(coords) > 3 {
		return nil, usageError("needs the mob's id and a point: worldctl npc move <id> <x> <y> [<z>]")
	}
	var v [3]float64
	for i, s := range coords {
		n, err := parseNumber(s)
		if err != nil {
			return nil, usageError(fmt.Sprintf("%q is not a number: worldctl npc move <id> <x> <y> [<z>]", s))
		}
		v[i] = n
	}
	req := &worldadminv1.MoveNpcRequest{Id: id, Point: &worldadminv1.Point{X: v[0], Y: v[1], Z: v[2], HasZ: len(coords) == 3}}
	return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
		r, err := c.MoveNpc(ctx, req)
		if err != nil {
			return err
		}
		_, err = fmt.Fprintln(out, r.GetResult())
		return err
	}, nil
}

func parseNpcSpawn(args []string) (call, error) {
	const help = "worldctl npc spawn <def_or_name> <x> <y> [<z>] [--level N]"
	var rest []string
	var level int64
	for i := 0; i < len(args); i++ {
		a := strings.TrimSpace(args[i])
		if a == "--level" || a == "-level" {
			if i+1 >= len(args) {
				return nil, usageError("--level needs a whole number from 1 to 100: " + help)
			}
			i++
			n, err := strconv.ParseInt(strings.TrimSpace(args[i]), 10, 32)
			if err != nil || n < 1 || n > 100 {
				return nil, usageError(fmt.Sprintf("--level %q is not a whole number from 1 to 100: %s", args[i], help))
			}
			level = n
		} else if strings.HasPrefix(a, "--level=") || strings.HasPrefix(a, "-level=") {
			val := a[strings.IndexByte(a, '=')+1:]
			n, err := strconv.ParseInt(strings.TrimSpace(val), 10, 32)
			if err != nil || n < 1 || n > 100 {
				return nil, usageError(fmt.Sprintf("--level %q is not a whole number from 1 to 100: %s", val, help))
			}
			level = n
		} else {
			rest = append(rest, a)
		}
	}
	if len(rest) < 3 || len(rest) > 4 || strings.TrimSpace(rest[0]) == "" {
		return nil, usageError("needs a creature name/Guid and coordinates: " + help)
	}
	var v [3]float64
	for i, s := range rest[1:] {
		n, err := parseNumber(s)
		if err != nil {
			return nil, usageError(fmt.Sprintf("%q is not a number: %s", s, help))
		}
		v[i] = n
	}
	req := &worldadminv1.SpawnNpcRequest{
		Npc:    rest[0],
		Level:  int32(level),
		Target: &worldadminv1.SpawnNpcRequest_Point{Point: &worldadminv1.Point{X: v[0], Y: v[1], Z: v[2], HasZ: len(rest) == 4}},
	}
	return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
		r, err := c.SpawnNpc(ctx, req)
		if err != nil {
			return err
		}
		name := r.GetDisplayName()
		if name == "" {
			name = r.GetNpc()
		}
		p := r.GetPosition()
		_, err = fmt.Fprintf(out, "spawned %s (id %d, level %d) at (%.0f, %.0f, %.0f)\n", name, r.GetId(), r.GetLevel(), p.GetX(), p.GetY(), p.GetZ())
		return err
	}, nil
}
