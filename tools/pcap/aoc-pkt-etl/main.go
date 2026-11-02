package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
)

const (
	PcapMagicMicroLE uint32 = 0xa1b2c3d4
	PcapMagicNanoLE  uint32 = 0xa1b23c4d
	PcapngMagicSHB   uint32 = 0x0A0D0D0A
	SniffSchemaDDL   string = `CREATE TABLE IF NOT EXISTS packets (pkt_id INTEGER PRIMARY KEY, build_sha256 TEXT NOT NULL, timestamp_us INTEGER NOT NULL, direction INTEGER NOT NULL, seq INTEGER NOT NULL, ack INTEGER NOT NULL, raw_bytes BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS bunches (bunch_id INTEGER PRIMARY KEY, pkt_id INTEGER NOT NULL, ch_index INTEGER NOT NULL, ch_type INTEGER NOT NULL, b_open INTEGER NOT NULL, b_close INTEGER NOT NULL, b_reliable INTEGER NOT NULL, ch_seq INTEGER NOT NULL, bit_len INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS rep_ops (op_id INTEGER PRIMARY KEY, bunch_id INTEGER NOT NULL, class_name TEXT NOT NULL, cmd_handle INTEGER NOT NULL, field_name TEXT NOT NULL, bit_start INTEGER NOT NULL, bit_end INTEGER NOT NULL, decoded_json TEXT NOT NULL, leftover_bits INTEGER NOT NULL, is_error INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS rpc_calls (rpc_id INTEGER PRIMARY KEY, bunch_id INTEGER NOT NULL, actor_class TEXT NOT NULL, func_name TEXT NOT NULL, handle INTEGER NOT NULL, bit_len INTEGER NOT NULL, params_json TEXT NOT NULL, is_error INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS bit_cursor_deltas (delta_id INTEGER PRIMARY KEY, op_id INTEGER NOT NULL, step_index INTEGER NOT NULL, serializer_fn TEXT NOT NULL, bit_offset INTEGER NOT NULL, bit_count INTEGER NOT NULL, raw_uint64 INTEGER NOT NULL);`
)

type BitTraceSlot struct {
	QpcTicks       uint64
	StructTypeHash uint32
	ChannelIndex   uint32
	BitPosBefore   int32
	BitPosAfter    int32
	RetOk          uint8
}

func DecodeBitTraceSlot(buf []byte) (BitTraceSlot, error) {
	if len(buf) < 32 {
		return BitTraceSlot{}, errors.New("bit trace slot smaller than 32 bytes")
	}
	s := BitTraceSlot{
		QpcTicks:       binary.LittleEndian.Uint64(buf[0:8]),
		StructTypeHash: binary.LittleEndian.Uint32(buf[8:12]),
		ChannelIndex:   binary.LittleEndian.Uint32(buf[12:16]),
		BitPosBefore:   int32(binary.LittleEndian.Uint32(buf[16:20])),
		BitPosAfter:    int32(binary.LittleEndian.Uint32(buf[20:24])),
		RetOk:          buf[24] & 1,
	}
	if s.BitPosBefore < 0 || s.BitPosAfter < s.BitPosBefore {
		return BitTraceSlot{}, errors.New("invalid bit cursor delta")
	}
	return s, nil
}

func DetectCaptureMagic(buf []byte) (string, error) {
	if len(buf) < 4 {
		return "", errors.New("capture header shorter than 4 bytes")
	}
	switch binary.LittleEndian.Uint32(buf[:4]) {
	case PcapMagicMicroLE, PcapMagicNanoLE:
		return "pcap", nil
	case PcapngMagicSHB:
		return "pcapng", nil
	default:
		return "", errors.New("unrecognized capture magic")
	}
}

func LeftoverBitsAuditSQL(className string) string {
	clean := strings.ReplaceAll(className, "'", "''")
	if clean == "" {
		return "SELECT op_id, bunch_id, class_name, cmd_handle, leftover_bits FROM rep_ops WHERE leftover_bits != 0 OR is_error != 0 ORDER BY op_id;"
	}
	return fmt.Sprintf("SELECT op_id, bunch_id, class_name, cmd_handle, leftover_bits FROM rep_ops WHERE class_name = '%s' AND (leftover_bits != 0 OR is_error != 0) ORDER BY op_id;", clean)
}

func main() {
	fmt.Println(LeftoverBitsAuditSQL(""))
}
