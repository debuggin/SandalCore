// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package standin

import (
	"fmt"
	"strings"
	"testing"
)

type spyTB struct {
	testing.TB
	skipped bool
	msg     string
}

func (s *spyTB) Helper() {}

func (s *spyTB) Skipf(format string, args ...any) {
	s.skipped = true
	s.msg = fmt.Sprintf(format, args...)
}

func TestTagConstantAndBuildInfo(t *testing.T) {
	if Tag != "aoc_standin" {
		t.Fatalf("Tag = %q, want %q", Tag, "aoc_standin")
	}
	if got := hasStandinTag(); got != Active {
		t.Fatalf("hasStandinTag() = %v, want Active (%v)", got, Active)
	}
}

func TestSkipRespectsActiveState(t *testing.T) {
	orig := Active
	defer func() { Active = orig }()

	Active = false
	spy := &spyTB{TB: t}
	Skip(spy, "the stat corpus")
	if spy.skipped {
		t.Fatalf("Skip with Active=false skipped with %q, want no skip", spy.msg)
	}

	Active = true
	spy = &spyTB{TB: t}
	Skip(spy, "the stat corpus")
	if !spy.skipped {
		t.Fatal("Skip with Active=true did not skip")
	}
	if !strings.Contains(spy.msg, Tag) || !strings.Contains(spy.msg, "the stat corpus") {
		t.Errorf("Skip message = %q, want both %q and %q", spy.msg, Tag, "the stat corpus")
	}
}
