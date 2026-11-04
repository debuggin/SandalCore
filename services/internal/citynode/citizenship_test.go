package citynode

import (
	"errors"
	"math"
	"testing"
	"time"
)

type citizenshipTestCase struct {
	name    string
	req     CitizenshipRequest
	wantErr error
}

func buildCitizenshipValidationCases(baseTime time.Time) []citizenshipTestCase {
	return []citizenshipTestCase{
		{
			name:    "village_stage_3_valid",
			req:     CitizenshipRequest{CharacterID: "char-1", NodeID: "node-alpha", NodeLevel: LevelVillage, PlayerLevel: NodeCitizenshipPlayerLevelRequirement, Now: baseTime},
			wantErr: nil,
		},
		{
			name:    "town_stage_4_valid",
			req:     CitizenshipRequest{CharacterID: "char-2", NodeID: "node-beta", NodeLevel: LevelTown, PlayerLevel: 25, Now: baseTime},
			wantErr: nil,
		},
		{
			name:    "encampment_stage_2_rejected",
			req:     CitizenshipRequest{CharacterID: "char-3", NodeID: "node-alpha", NodeLevel: LevelEncampment, PlayerLevel: 15, Now: baseTime},
			wantErr: ErrNodeStageTooLow,
		},
		{
			name:    "player_level_below_requirement_rejected",
			req:     CitizenshipRequest{CharacterID: "char-4", NodeID: "node-alpha", NodeLevel: LevelVillage, PlayerLevel: NodeCitizenshipPlayerLevelRequirement - 1, Now: baseTime},
			wantErr: ErrPlayerLevelTooLow,
		},
		{
			name:    "empty_character_rejected",
			req:     CitizenshipRequest{CharacterID: "", NodeID: "node-alpha", NodeLevel: LevelVillage, PlayerLevel: 10, Now: baseTime},
			wantErr: ErrInvalid,
		},
	}
}

func TestApplyCitizenshipValidationTable(t *testing.T) {
	baseTime := time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)
	for _, tc := range buildCitizenshipValidationCases(baseTime) {
		t.Run(tc.name, func(t *testing.T) {
			ledger := NewCitizenshipLedger(time.Hour)
			rec, err := ApplyCitizenship(ledger, tc.req)
			if !errors.Is(err, tc.wantErr) {
				t.Fatalf("ApplyCitizenship error = %v, want %v", err, tc.wantErr)
			}
			if tc.wantErr == nil && rec.NodeID != tc.req.NodeID {
				t.Fatalf("CitizenRecord.NodeID = %q, want %q", rec.NodeID, tc.req.NodeID)
			}
		})
	}
}

func TestSingleNodeExclusivityAndCooldown(t *testing.T) {
	t0 := time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)
	cooldown := 2 * time.Hour
	ledger := NewCitizenshipLedger(cooldown)

	reqAlpha := CitizenshipRequest{
		CharacterID: "char-10",
		NodeID:      "node-alpha",
		NodeLevel:   LevelVillage,
		PlayerLevel: 12,
		Now:         t0,
	}
	if _, err := ApplyCitizenship(ledger, reqAlpha); err != nil {
		t.Fatalf("initial ApplyCitizenship failed: %v", err)
	}
	if got := ledger.NodeCitizenCount("node-alpha"); got != 1 {
		t.Fatalf("NodeCitizenCount = %d, want 1", got)
	}

	reqBeta := reqAlpha
	reqBeta.NodeID = "node-beta"
	if _, err := ApplyCitizenship(ledger, reqBeta); !errors.Is(err, ErrAlreadyCitizen) {
		t.Fatalf("second node ApplyCitizenship = %v, want %v", err, ErrAlreadyCitizen)
	}

	if err := RevokeCitizenship(ledger, "char-10", "node-beta", t0.Add(10*time.Minute)); !errors.Is(err, ErrNotCitizen) {
		t.Fatalf("RevokeCitizenship wrong node = %v, want %v", err, ErrNotCitizen)
	}
	tRevoke := t0.Add(30 * time.Minute)
	if err := RevokeCitizenship(ledger, "char-10", "node-alpha", tRevoke); err != nil {
		t.Fatalf("RevokeCitizenship failed: %v", err)
	}

	reqEarly := reqBeta
	reqEarly.Now = tRevoke.Add(cooldown - time.Second)
	if _, err := ApplyCitizenship(ledger, reqEarly); !errors.Is(err, ErrCitizenshipCooldown) {
		t.Fatalf("cooldown ApplyCitizenship = %v, want %v", err, ErrCitizenshipCooldown)
	}

	reqAfter := reqBeta
	reqAfter.Now = tRevoke.Add(cooldown)
	if _, err := ApplyCitizenship(ledger, reqAfter); err != nil {
		t.Fatalf("post-cooldown ApplyCitizenship failed: %v", err)
	}
	rec, ok := ledger.CitizenOf("char-10")
	if !ok || rec.NodeID != "node-beta" {
		t.Fatalf("CitizenOf = (%+v, %v), want node-beta", rec, ok)
	}
}

func TestBuyOrderEscrowReserveSettleAndRefund(t *testing.T) {
	t0 := time.Date(2026, 11, 3, 15, 0, 0, 0, time.UTC)
	ledger := NewCitizenshipLedger(time.Hour)
	_, err := ApplyCitizenship(ledger, CitizenshipRequest{
		CharacterID: "buyer-1",
		NodeID:      "node-alpha",
		NodeLevel:   LevelVillage,
		PlayerLevel: 20,
		Now:         t0,
	})
	if err != nil {
		t.Fatalf("ApplyCitizenship failed: %v", err)
	}
	if err := ledger.CreditWallet("buyer-1", 1000); err != nil {
		t.Fatalf("CreditWallet failed: %v", err)
	}

	order1, err := ReserveBuyOrderEscrow(ledger, "ord-1", "buyer-1", "node-alpha", 600)
	if err != nil || order1.Status != EscrowStatusReserved {
		t.Fatalf("ReserveBuyOrderEscrow ord-1 = (%+v, %v)", order1, err)
	}
	if ledger.WalletBalance("buyer-1") != 400 || ledger.ReservedBalance("buyer-1") != 600 {
		t.Fatalf("unexpected balances after reserve: wallet=%d reserved=%d", ledger.WalletBalance("buyer-1"), ledger.ReservedBalance("buyer-1"))
	}

	settled, err := SettleBuyOrderEscrow(ledger, "ord-1", 450)
	if err != nil {
		t.Fatalf("SettleBuyOrderEscrow failed: %v", err)
	}
	if settled.SettledGold != 450 || settled.RefundedGold != 150 || settled.Status != EscrowStatusSettled {
		t.Fatalf("settled order mismatch: %+v", settled)
	}
	if ledger.WalletBalance("buyer-1") != 550 || ledger.ReservedBalance("buyer-1") != 0 {
		t.Fatalf("balances after partial settle: wallet=%d reserved=%d", ledger.WalletBalance("buyer-1"), ledger.ReservedBalance("buyer-1"))
	}

	if _, err := ReserveBuyOrderEscrow(ledger, "ord-2", "buyer-1", "node-alpha", 300); err != nil {
		t.Fatalf("ReserveBuyOrderEscrow ord-2 failed: %v", err)
	}
	refunded, err := RefundBuyOrderEscrow(ledger, "ord-2")
	if err != nil || refunded.Status != EscrowStatusRefunded || refunded.RefundedGold != 300 {
		t.Fatalf("RefundBuyOrderEscrow ord-2 = (%+v, %v)", refunded, err)
	}
	if ledger.WalletBalance("buyer-1") != 550 || ledger.ReservedBalance("buyer-1") != 0 {
		t.Fatalf("balances after refund: wallet=%d reserved=%d", ledger.WalletBalance("buyer-1"), ledger.ReservedBalance("buyer-1"))
	}
}

func TestBuyOrderEscrowUnderflowOverflowAndStateGuards(t *testing.T) {
	t0 := time.Date(2026, 11, 3, 16, 0, 0, 0, time.UTC)
	ledger := NewCitizenshipLedger(time.Hour)
	_, _ = ApplyCitizenship(ledger, CitizenshipRequest{
		CharacterID: "buyer-2",
		NodeID:      "node-alpha",
		NodeLevel:   LevelTown,
		PlayerLevel: 30,
		Now:         t0,
	})
	_ = ledger.CreditWallet("buyer-2", 500)

	if err := ledger.CreditWallet("buyer-2", math.MaxUint64-100); !errors.Is(err, ErrEscrowOverflow) {
		t.Fatalf("CreditWallet overflow = %v, want %v", err, ErrEscrowOverflow)
	}
	if _, err := ReserveBuyOrderEscrow(ledger, "ord-over", "buyer-2", "node-alpha", 501); !errors.Is(err, ErrEscrowInsufficientFunds) {
		t.Fatalf("ReserveBuyOrderEscrow insufficient = %v, want %v", err, ErrEscrowInsufficientFunds)
	}
	if _, err := ReserveBuyOrderEscrow(ledger, "ord-outsider", "stranger", "node-alpha", 10); !errors.Is(err, ErrNotCitizen) {
		t.Fatalf("ReserveBuyOrderEscrow non-citizen = %v, want %v", err, ErrNotCitizen)
	}

	if _, err := ReserveBuyOrderEscrow(ledger, "ord-valid", "buyer-2", "node-alpha", 200); err != nil {
		t.Fatalf("ReserveBuyOrderEscrow ord-valid failed: %v", err)
	}
	if _, err := ReserveBuyOrderEscrow(ledger, "ord-valid", "buyer-2", "node-alpha", 50); !errors.Is(err, ErrOrderAlreadyExists) {
		t.Fatalf("duplicate order ID = %v, want %v", err, ErrOrderAlreadyExists)
	}
	if _, err := SettleBuyOrderEscrow(ledger, "ord-valid", 201); !errors.Is(err, ErrEscrowInsufficientFunds) {
		t.Fatalf("settle > reserved = %v, want %v", err, ErrEscrowInsufficientFunds)
	}
	if _, err := SettleBuyOrderEscrow(ledger, "ord-valid", 200); err != nil {
		t.Fatalf("SettleBuyOrderEscrow exact failed: %v", err)
	}
	if _, err := RefundBuyOrderEscrow(ledger, "ord-valid"); !errors.Is(err, ErrOrderClosed) {
		t.Fatalf("RefundBuyOrderEscrow after settle = %v, want %v", err, ErrOrderClosed)
	}
}
