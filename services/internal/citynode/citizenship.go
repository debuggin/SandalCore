package citynode

import (
	"fmt"
	"math"
	"time"
)

const DefaultCitizenshipCooldown = 24 * time.Hour

var (
	ErrNodeStageTooLow         = fmt.Errorf("%w: node stage below village minimum", ErrInvalid)
	ErrPlayerLevelTooLow       = fmt.Errorf("%w: player level below citizenship requirement", ErrInvalid)
	ErrAlreadyCitizen          = fmt.Errorf("%w: character already holds active citizenship", ErrInvalid)
	ErrCitizenshipCooldown     = fmt.Errorf("%w: citizenship re-application cooldown active", ErrInvalid)
	ErrNotCitizen              = fmt.Errorf("%w: character is not a citizen of node", ErrNotFound)
	ErrEscrowOverflow          = fmt.Errorf("%w: escrow arithmetic overflow", ErrInvalid)
	ErrEscrowInsufficientFunds = fmt.Errorf("%w: insufficient balance or escrow for operation", ErrInvalid)
	ErrOrderAlreadyExists      = fmt.Errorf("%w: buy order escrow already exists", ErrInvalid)
	ErrOrderNotFound           = fmt.Errorf("%w: buy order escrow not found", ErrNotFound)
	ErrOrderClosed             = fmt.Errorf("%w: buy order escrow already finalized", ErrInvalid)
)

type EscrowOrderStatus uint8

const (
	EscrowStatusReserved EscrowOrderStatus = iota + 1
	EscrowStatusSettled
	EscrowStatusRefunded
)

type CitizenRecord struct {
	CharacterID string
	NodeID      string
	NodeLevel   int
	JoinedAt    time.Time
}

type CitizenshipRequest struct {
	CharacterID string
	NodeID      string
	NodeLevel   int
	PlayerLevel int
	Now         time.Time
}

type BuyOrderEscrow struct {
	OrderID      string
	BuyerID      string
	NodeID       string
	ReservedGold uint64
	SettledGold  uint64
	RefundedGold uint64
	Status       EscrowOrderStatus
}

type CitizenshipLedger struct {
	cooldown      time.Duration
	citizens      map[string]CitizenRecord
	revokedAt     map[string]time.Time
	wallets       map[string]uint64
	totalReserved map[string]uint64
	escrows       map[string]BuyOrderEscrow
}

func NewCitizenshipLedger(cooldown time.Duration) *CitizenshipLedger {
	if cooldown < 0 {
		cooldown = DefaultCitizenshipCooldown
	}
	return &CitizenshipLedger{
		cooldown:      cooldown,
		citizens:      make(map[string]CitizenRecord),
		revokedAt:     make(map[string]time.Time),
		wallets:       make(map[string]uint64),
		totalReserved: make(map[string]uint64),
		escrows:       make(map[string]BuyOrderEscrow),
	}
}

func addUint64Checked(a, b uint64) (uint64, bool) {
	if math.MaxUint64-a < b {
		return 0, false
	}
	return a + b, true
}

func subUint64Checked(a, b uint64) (uint64, bool) {
	if a < b {
		return 0, false
	}
	return a - b, true
}

func validateCitizenshipRequest(req CitizenshipRequest) error {
	if req.CharacterID == "" || req.NodeID == "" {
		return ErrInvalid
	}
	if req.NodeLevel < MinimumOperationalNodeLevel || req.NodeLevel > MaxNodeLevel {
		return ErrNodeStageTooLow
	}
	if req.PlayerLevel < NodeCitizenshipPlayerLevelRequirement {
		return ErrPlayerLevelTooLow
	}
	return nil
}

func (l *CitizenshipLedger) ApplyCitizenship(req CitizenshipRequest) (CitizenRecord, error) {
	if l == nil {
		return CitizenRecord{}, ErrInvalid
	}
	if err := validateCitizenshipRequest(req); err != nil {
		return CitizenRecord{}, err
	}
	if _, exists := l.citizens[req.CharacterID]; exists {
		return CitizenRecord{}, ErrAlreadyCitizen
	}
	if lastRevoked, ok := l.revokedAt[req.CharacterID]; ok && l.cooldown > 0 {
		if req.Now.Before(lastRevoked.Add(l.cooldown)) {
			return CitizenRecord{}, ErrCitizenshipCooldown
		}
	}
	rec := CitizenRecord{
		CharacterID: req.CharacterID,
		NodeID:      req.NodeID,
		NodeLevel:   req.NodeLevel,
		JoinedAt:    req.Now.UTC(),
	}
	l.citizens[req.CharacterID] = rec
	return rec, nil
}

func (l *CitizenshipLedger) RevokeCitizenship(characterID, nodeID string, now time.Time) error {
	if l == nil || characterID == "" || nodeID == "" {
		return ErrInvalid
	}
	rec, exists := l.citizens[characterID]
	if !exists || rec.NodeID != nodeID {
		return ErrNotCitizen
	}
	delete(l.citizens, characterID)
	l.revokedAt[characterID] = now.UTC()
	return nil
}

func (l *CitizenshipLedger) CitizenOf(characterID string) (CitizenRecord, bool) {
	if l == nil {
		return CitizenRecord{}, false
	}
	rec, ok := l.citizens[characterID]
	return rec, ok
}

func (l *CitizenshipLedger) NodeCitizenCount(nodeID string) int {
	if l == nil || nodeID == "" {
		return 0
	}
	count := 0
	for _, rec := range l.citizens {
		if rec.NodeID == nodeID {
			count++
		}
	}
	return count
}

func (l *CitizenshipLedger) CreditWallet(characterID string, amount uint64) error {
	if l == nil || characterID == "" || amount == 0 {
		return ErrInvalid
	}
	next, ok := addUint64Checked(l.wallets[characterID], amount)
	if !ok {
		return ErrEscrowOverflow
	}
	l.wallets[characterID] = next
	return nil
}

func (l *CitizenshipLedger) WalletBalance(characterID string) uint64 {
	if l == nil {
		return 0
	}
	return l.wallets[characterID]
}

func (l *CitizenshipLedger) ReservedBalance(characterID string) uint64 {
	if l == nil {
		return 0
	}
	return l.totalReserved[characterID]
}

func (l *CitizenshipLedger) ReserveBuyOrderEscrow(
	orderID, buyerID, nodeID string,
	amount uint64,
) (BuyOrderEscrow, error) {
	if l == nil || orderID == "" || buyerID == "" || nodeID == "" || amount == 0 {
		return BuyOrderEscrow{}, ErrInvalid
	}
	if _, exists := l.escrows[orderID]; exists {
		return BuyOrderEscrow{}, ErrOrderAlreadyExists
	}
	rec, isCitizen := l.citizens[buyerID]
	if !isCitizen || rec.NodeID != nodeID {
		return BuyOrderEscrow{}, ErrNotCitizen
	}
	nextWallet, ok := subUint64Checked(l.wallets[buyerID], amount)
	if !ok {
		return BuyOrderEscrow{}, ErrEscrowInsufficientFunds
	}
	nextReserved, ok := addUint64Checked(l.totalReserved[buyerID], amount)
	if !ok {
		return BuyOrderEscrow{}, ErrEscrowOverflow
	}
	order := BuyOrderEscrow{
		OrderID:      orderID,
		BuyerID:      buyerID,
		NodeID:       nodeID,
		ReservedGold: amount,
		Status:       EscrowStatusReserved,
	}
	l.wallets[buyerID] = nextWallet
	l.totalReserved[buyerID] = nextReserved
	l.escrows[orderID] = order
	return order, nil
}

func (l *CitizenshipLedger) SettleBuyOrderEscrow(
	orderID string,
	fillAmount uint64,
) (BuyOrderEscrow, error) {
	if l == nil || orderID == "" || fillAmount == 0 {
		return BuyOrderEscrow{}, ErrInvalid
	}
	order, exists := l.escrows[orderID]
	if !exists {
		return BuyOrderEscrow{}, ErrOrderNotFound
	}
	if order.Status != EscrowStatusReserved {
		return BuyOrderEscrow{}, ErrOrderClosed
	}
	surplus, ok := subUint64Checked(order.ReservedGold, fillAmount)
	if !ok {
		return BuyOrderEscrow{}, ErrEscrowInsufficientFunds
	}
	nextReserved, ok := subUint64Checked(l.totalReserved[order.BuyerID], order.ReservedGold)
	if !ok {
		return BuyOrderEscrow{}, ErrEscrowInsufficientFunds
	}
	nextWallet, ok := addUint64Checked(l.wallets[order.BuyerID], surplus)
	if !ok {
		return BuyOrderEscrow{}, ErrEscrowOverflow
	}
	order.SettledGold = fillAmount
	order.RefundedGold = surplus
	order.Status = EscrowStatusSettled
	l.totalReserved[order.BuyerID] = nextReserved
	l.wallets[order.BuyerID] = nextWallet
	l.escrows[orderID] = order
	return order, nil
}

func (l *CitizenshipLedger) RefundBuyOrderEscrow(orderID string) (BuyOrderEscrow, error) {
	if l == nil || orderID == "" {
		return BuyOrderEscrow{}, ErrInvalid
	}
	order, exists := l.escrows[orderID]
	if !exists {
		return BuyOrderEscrow{}, ErrOrderNotFound
	}
	if order.Status != EscrowStatusReserved {
		return BuyOrderEscrow{}, ErrOrderClosed
	}
	nextReserved, ok := subUint64Checked(l.totalReserved[order.BuyerID], order.ReservedGold)
	if !ok {
		return BuyOrderEscrow{}, ErrEscrowInsufficientFunds
	}
	nextWallet, ok := addUint64Checked(l.wallets[order.BuyerID], order.ReservedGold)
	if !ok {
		return BuyOrderEscrow{}, ErrEscrowOverflow
	}
	order.RefundedGold = order.ReservedGold
	order.Status = EscrowStatusRefunded
	l.totalReserved[order.BuyerID] = nextReserved
	l.wallets[order.BuyerID] = nextWallet
	l.escrows[orderID] = order
	return order, nil
}

func ApplyCitizenship(ledger *CitizenshipLedger, req CitizenshipRequest) (CitizenRecord, error) {
	return ledger.ApplyCitizenship(req)
}

func RevokeCitizenship(ledger *CitizenshipLedger, characterID, nodeID string, now time.Time) error {
	return ledger.RevokeCitizenship(characterID, nodeID, now)
}

func ReserveBuyOrderEscrow(
	ledger *CitizenshipLedger,
	orderID, buyerID, nodeID string,
	amount uint64,
) (BuyOrderEscrow, error) {
	return ledger.ReserveBuyOrderEscrow(orderID, buyerID, nodeID, amount)
}

func SettleBuyOrderEscrow(
	ledger *CitizenshipLedger,
	orderID string,
	fillAmount uint64,
) (BuyOrderEscrow, error) {
	return ledger.SettleBuyOrderEscrow(orderID, fillAmount)
}

func RefundBuyOrderEscrow(ledger *CitizenshipLedger, orderID string) (BuyOrderEscrow, error) {
	return ledger.RefundBuyOrderEscrow(orderID)
}
