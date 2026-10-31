// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import "fmt"

// Program is the code of one formula.
type Program struct {
	Locals uint16
	Code   []byte
}

// Stat is one stat of the graph.
type Stat struct {
	Guid        int64
	Name        uint32 // index into Strings
	Replication uint8  // 0 none, 1 Owner, 5 OwnerProxy, 7 EveryoneProxy, 6 ProxyOnly
	Flags       uint8
	// Base, Min, Max, Regen are program indexes, -1 when the stat has no such formula.
	Base, Min, Max, Regen int32
}

// Step is one initializer entry: a program that gives Stat its next value.
type Step struct {
	Stat uint32
	Prog uint32
}

// Class is one player class: its own initializer list and the stats it has.
type Class struct {
	Enum    uint8 // ECharacterClass
	Name    uint32
	Steps   []Step   // the class's list, in order, after the common lists
	Members []uint32 // the stats of the class's own list
}

// [C_Y_B - C O A]

// Mode is the arithmetic of a bundle.
type Mode uint8

const (
	// ModeFloat32 (the default): every value the engine stores or returns is a
	// float32 (a formula's result, a stat's initial value, a stat's value), and
	// the arithmetic between them is float64. This is what reproduces the captured
	// stats of the level-1 Bard to the bit (442 of 442; ModeDouble gets 440 and ModeSingle
	// 440: the engine's formulas return floats, and its stats are stored as floats).
	ModeFloat32 Mode = iota
	// ModeDouble keeps every value a float64.
	ModeDouble
	// ModeSingle also rounds every arithmetic result, constant and curve value to
	// float32, as aocexpr's Env.Single does.
	ModeSingle
)

func (m Mode) String() string {
	switch m {
	case ModeFloat32:
		return "float32"
	case ModeDouble:
		return "double"
	case ModeSingle:
		return "single"
	}
	return fmt.Sprintf("Mode(%d)", uint8(m))
}
