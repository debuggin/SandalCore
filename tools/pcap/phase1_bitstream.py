#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Bit-level primitives and UE5/AoC packet, bunch, GUID, RepLayout, and content block decoders."""

import struct

OUTER_HDR_BITS = 38
CUSTOM_FIELD_BITS = 48
MAX_PKT_BITS = 1024 * 8
MAX_CHSEQ = 1024


def read_bit(data, pos):
    byte_idx = pos >> 3
    if byte_idx >= len(data):
        return 0
    return (data[byte_idx] >> (pos & 7)) & 1


def read_bits_le(data, off, n):
    """Read n bits starting at bit offset 'off', LSB first."""
    v = 0
    for i in range(n):
        bp = off + i
        byte_idx = bp >> 3
        if byte_idx < len(data):
            v |= ((data[byte_idx] >> (bp & 7)) & 1) << i
    return v, off + n


def read_uint8(data, pos):
    return read_bits_le(data, pos, 8)


def read_uint16(data, pos):
    return read_bits_le(data, pos, 16)


def read_uint32(data, pos):
    return read_bits_le(data, pos, 32)


def serialize_int(data, pos, max_val):
    """UE5 SerializeInt: variable-length encoding up to max_val."""
    v = 0
    mask = 1
    while (v + mask) < max_val and mask < (1 << 32):
        if read_bit(data, pos):
            v |= mask
        pos += 1
        mask <<= 1
    return v, pos


def serialize_int_packed(data, pos, end_pos=None):
    """UE5 SerializeIntPacked (uint32): bit0=continue, bits1-7=data."""
    v = 0
    shift = 0
    for _ in range(5):
        if end_pos is not None and pos + 8 > end_pos:
            return None, pos
        byte_val, pos = read_bits_le(data, pos, 8)
        v |= ((byte_val >> 1) & 0x7F) << shift
        if not (byte_val & 1):
            break
        shift += 7
    return v, pos


def serialize_int_packed64(data, pos, end_pos=None):
    """UE5 SerializeIntPacked64 (uint64): up to 10 bytes."""
    v = 0
    shift = 0
    for _ in range(10):
        if end_pos is not None and pos + 8 > end_pos:
            return None, pos
        byte_val, pos = read_bits_le(data, pos, 8)
        v |= ((byte_val >> 1) & 0x7F) << shift
        if not (byte_val & 1):
            break
        shift += 7
    return v, pos


def _read_utf16_chars(data, pos, count):
    chars = []
    for _ in range(count):
        c, pos = read_bits_le(data, pos, 16)
        if c == 0:
            break
        try:
            chars.append(chr(c))
        except (ValueError, OverflowError):
            chars.append(f'\\u{c:04x}')
    return ''.join(chars), pos


def _read_ascii_chars(data, pos, strlen):
    chars = []
    for _ in range(strlen):
        c, pos = read_bits_le(data, pos, 8)
        chars.append('' if c == 0 else chr(c & 0x7F))
    return ''.join(chars).rstrip('\x00'), pos


def read_fstring(data, pos):
    """Read UE5 FString: int32 length + chars. Negative = UTF-16."""
    raw, pos = read_bits_le(data, pos, 32)
    strlen = raw if raw < 0x80000000 else raw - 0x100000000
    if strlen == 0:
        return "", pos
    if strlen < 0:
        return _read_utf16_chars(data, pos, min(-strlen, 4096))
    if strlen <= 4096:
        return _read_ascii_chars(data, pos, strlen)
    return None, pos


def static_parse_name(data, pos):
    """Read FName: bIsHardcoded(1) + [PackedIndex | FString + Number]."""
    is_hardcoded = read_bit(data, pos)
    pos += 1
    if is_hardcoded:
        idx, pos = serialize_int_packed(data, pos)
        return f"EName[{idx}]", pos
    name, pos = read_fstring(data, pos)
    _, pos = read_bits_le(data, pos, 32)
    if name is None:
        return None, pos
    return name, pos


def find_content_end(data):
    """Find the last '1' bit (UE5 termination bit), return bit count."""
    for i in range(len(data) - 1, -1, -1):
        if data[i]:
            byte_val = data[i]
            bit_pos = i * 8 + 7
            while not (byte_val & 0x80):
                byte_val <<= 1
                bit_pos -= 1
            return bit_pos
    return 0


def extract_realigned(data, start_bit, num_bits):
    """Extract num_bits starting at start_bit into a new byte array, realigned."""
    result = bytearray((num_bits + 7) // 8)
    for i in range(num_bits):
        sb = start_bit + i
        byte_idx = sb >> 3
        if byte_idx < len(data) and ((data[byte_idx] >> (sb & 7)) & 1):
            result[i >> 3] |= 1 << (i & 7)
    return bytes(result)


def _read_packet_info(inner_data, pos, content_bits, direction):
    has_pkt_info = read_bit(inner_data, pos)
    pos += 1
    jitter_ms = 0
    if has_pkt_info:
        jitter_ms, pos = serialize_int(inner_data, pos, 1024)
    has_srv_frame = read_bit(inner_data, pos)
    pos += 1
    if has_srv_frame and direction == 'S>C':
        _, pos = read_bits_le(inner_data, pos, 8)
    return has_pkt_info, has_srv_frame, jitter_ms, pos


def parse_packet(raw, direction):
    """Parse a single UE5+AoC UDP data packet."""
    outer_bits = find_content_end(raw)
    if outer_bits < OUTER_HDR_BITS + 64:
        return None
    inner_total = outer_bits - OUTER_HDR_BITS
    inner_data = extract_realigned(raw, OUTER_HDR_BITS, inner_total)
    content_bits = find_content_end(inner_data)
    if content_bits < 64:
        return None
    packed_hdr, _ = read_bits_le(inner_data, 0, 32)
    hist_word_count = (packed_hdr & 0xF) + 1
    acked_seq = (packed_hdr >> 4) & 0x3FFF
    seq = (packed_hdr >> 18) & 0x3FFF
    pos = 32
    history = []
    for _ in range(hist_word_count):
        hw, pos = read_uint32(inner_data, pos)
        history.append(hw)
    if pos + CUSTOM_FIELD_BITS > content_bits:
        return None
    _, pos = read_bits_le(inner_data, pos, CUSTOM_FIELD_BITS)
    if pos >= content_bits:
        return {'seq': seq, 'ack': acked_seq, 'bunches': [], 'direction': direction}
    has_pkt_info, has_srv_frame, jitter_ms, pos = _read_packet_info(inner_data, pos, content_bits, direction)
    bunches = []
    while pos < content_bits and len(bunches) < 64:
        bunch, new_pos = parse_bunch_header(inner_data, pos, content_bits)
        if bunch is None:
            break
        bunches.append(bunch)
        pos = new_pos
    return {
        'seq': seq, 'ack': acked_seq, 'direction': direction,
        'has_pkt_info': has_pkt_info, 'has_srv_frame': has_srv_frame,
        'jitter': jitter_ms, 'bunches': bunches,
        'inner_data': inner_data, 'content_bits': content_bits,
    }


def _read_bunch_ctrl(data, pos, b_ctrl):
    if not b_ctrl:
        return 0, 0, 0, pos
    b_open = read_bit(data, pos)
    b_close = read_bit(data, pos + 1)
    pos += 2
    close_reason = 0
    if b_close:
        close_reason, pos = serialize_int(data, pos, 15)
    return b_open, b_close, close_reason, pos


def _read_bunch_partial(data, pos, b_partial):
    if not b_partial:
        return 0, 0, pos
    partial_initial = read_bit(data, pos)
    partial_final = read_bit(data, pos + 2)
    return partial_initial, partial_final, pos + 3


def parse_bunch_header(data, pos, content_bits):
    """Parse a single bunch header. Returns (bunch_dict, next_pos) or (None, pos)."""
    start = pos
    if pos >= content_bits:
        return None, pos
    b_ctrl = read_bit(data, pos)
    pos += 1
    b_open, b_close, close_reason, pos = _read_bunch_ctrl(data, pos, b_ctrl)
    pos += 1
    b_reliable = read_bit(data, pos)
    pos += 1
    ch_idx, pos = serialize_int_packed(data, pos)
    if ch_idx > 100000:
        return None, start
    b_has_exports = read_bit(data, pos)
    b_has_must_map = read_bit(data, pos + 1)
    b_partial = read_bit(data, pos + 2)
    pos += 3
    ch_seq = 0
    if b_reliable:
        ch_seq, pos = serialize_int(data, pos, MAX_CHSEQ)
    partial_initial, partial_final, pos = _read_bunch_partial(data, pos, b_partial)
    ch_name = None
    if b_reliable or b_open:
        ch_name, pos = static_parse_name(data, pos)
        if ch_name is None:
            return None, start
    bunch_data_bits, pos = serialize_int(data, pos, MAX_PKT_BITS)
    if bunch_data_bits > (content_bits - pos + 16) or bunch_data_bits < 0:
        return None, start
    data_start = pos
    return {
        'ctrl': b_ctrl, 'open': b_open, 'close': b_close, 'close_reason': close_reason,
        'reliable': b_reliable, 'ch': ch_idx, 'ch_name': ch_name or '', 'ch_seq': ch_seq,
        'has_exports': b_has_exports, 'has_must_map': b_has_must_map,
        'partial': b_partial, 'partial_initial': partial_initial, 'partial_final': partial_final,
        'bunch_data_bits': bunch_data_bits, 'data_start': data_start, 'hdr_bits': data_start - start,
    }, pos + bunch_data_bits


class GUIDCache:
    """Persistent GUID->path cache, like UE5's FNetGUIDCache."""

    def __init__(self):
        self.paths = {}
        self.names = {}
        self.outers = {}
        self.dynamic = {}

    def register(self, guid_value, name, outer_value=None, is_dynamic=False):
        if name and not name.startswith('<BAD'):
            self.names[guid_value] = name
        if outer_value is not None:
            self.outers[guid_value] = outer_value
        self.dynamic[guid_value] = is_dynamic

    def get_full_path(self, guid_value, max_depth=10):
        parts = []
        current = guid_value
        for _ in range(max_depth):
            name = self.names.get(current)
            if name:
                parts.append(name)
            outer = self.outers.get(current)
            if outer and outer != current:
                current = outer
            else:
                break
        parts.reverse()
        return '/'.join(parts) if parts else None

    def resolve(self, guid_value):
        return self.get_full_path(guid_value) or self.names.get(guid_value)


def _load_exported_object_path(data, pos, end_pos, guid_cache, result):
    depth = result.pop('_depth', 0)
    outer, pos = internal_load_object(data, pos, end_pos, True, guid_cache, depth + 1)
    result['outer'] = outer
    if pos + 32 > end_pos:
        result['error'] = 'truncated_name'
        return result, pos
    obj_name, pos = read_fstring(data, pos)
    if obj_name is None:
        result['error'] = 'bad_name_length'
        return result, pos
    result['name'] = obj_name
    outer_value = outer.get('value', 0) if outer.get('guid') else None
    guid_cache.register(result['value'], obj_name, outer_value, result['dynamic'])
    if result['flags'] & 0x04:
        if pos + 32 <= end_pos:
            checksum, pos = read_uint32(data, pos)
            result['checksum'] = checksum
        else:
            result['error'] = 'truncated_checksum'
    return result, pos


def internal_load_object(data, pos, end_pos, is_exporting, guid_cache, *extra):
    """Mirror of UPackageMapClient::InternalLoadObject."""
    depth = extra[0] if extra else 0
    if pos + 8 > end_pos:
        return {'guid': 0, 'value': 0, 'error': 'truncated'}, pos
    net_guid, pos = serialize_int_packed64(data, pos)
    if net_guid is None:
        net_guid = 0
    is_dynamic = bool(net_guid & 1)
    guid_value = net_guid >> 1
    result = {'guid': net_guid, 'value': guid_value, 'dynamic': is_dynamic}
    if net_guid == 0 or not (is_exporting and depth < 10):
        return result, pos
    if pos + 8 > end_pos:
        result['error'] = 'truncated_flags'
        return result, pos
    export_flags, pos = read_uint8(data, pos)
    has_path = bool(export_flags & 0x01)
    result['flags'] = export_flags
    result['has_path'] = has_path
    if not has_path:
        return result, pos
    result['_depth'] = depth
    return _load_exported_object_path(data, pos, end_pos, guid_cache, result)


def decode_guid_exports(data, pos, end_pos, guid_cache):
    """Decode ReceiveNetGUIDBunch: NumGUIDs + [InternalLoadObject x N]."""
    if pos + 32 > end_pos:
        return {'num': 0, 'exports': [], 'error': 'truncated'}, pos
    num_guids, pos = read_uint32(data, pos)
    if num_guids > 256:
        return {'num': num_guids, 'exports': [], 'error': 'too_many'}, pos
    exports = []
    for _ in range(num_guids):
        if pos + 8 > end_pos:
            break
        exp, pos = internal_load_object(data, pos, end_pos, True, guid_cache)
        exports.append(exp)
    return {'num': num_guids, 'exports': exports}, pos


def _decode_rep_layout_field(data, pos):
    handle, pos = read_uint32(data, pos)
    compat_cksum, pos = read_uint32(data, pos)
    is_exported = read_bit(data, pos)
    pos += 1
    fld = {'handle': handle, 'checksum': compat_cksum, 'exported': is_exported}
    if is_exported:
        fname, pos = read_fstring(data, pos)
        if fname is None:
            fld['error'] = 'bad_field_name'
            return fld, pos, False
        fld['name'] = fname
        ftype, pos = read_uint8(data, pos)
        fld['type'] = ftype
    return fld, pos, True


def _decode_rep_layout_entry(data, pos, end_pos):
    path_index, pos = read_uint32(data, pos)
    was_exported = read_bit(data, pos)
    pos += 1
    entry = {'path_index': path_index, 'exported': was_exported}
    if not was_exported:
        return entry, pos, True
    path_name, pos = read_fstring(data, pos)
    if path_name is None:
        entry['error'] = 'bad_path'
        return entry, pos, False
    entry['path'] = path_name
    if pos + 32 > end_pos:
        return entry, pos, False
    num_fields, pos = read_uint32(data, pos)
    if num_fields > 512:
        entry['error'] = 'too_many_fields'
        return entry, pos, False
    fields = []
    for _ in range(num_fields):
        if pos + 65 > end_pos:
            break
        fld, pos, ok = _decode_rep_layout_field(data, pos)
        fields.append(fld)
        if not ok:
            break
    entry['fields'] = fields
    return entry, pos, True


def decode_rep_layout_exports(data, pos, end_pos):
    """Decode ReceiveNetFieldExportsCompat."""
    if pos + 32 > end_pos:
        return {'num': 0, 'entries': [], 'error': 'truncated'}, pos
    num_entries, pos = read_uint32(data, pos)
    if num_entries > 256:
        return {'num': num_entries, 'entries': [], 'error': 'too_many'}, pos
    entries = []
    for _ in range(num_entries):
        if pos + 33 > end_pos:
            break
        entry, pos, ok = _decode_rep_layout_entry(data, pos, end_pos)
        entries.append(entry)
        if not ok:
            break
    return {'num': num_entries, 'entries': entries}, pos


def read_double_vector(data, pos):
    """Read 3 x float64 (192 bits) from bitstream."""
    components = []
    for _ in range(3):
        raw_bytes = bytearray(8)
        for byte_i in range(8):
            raw_bytes[byte_i], _ = read_bits_le(data, pos + byte_i * 8, 8)
        pos += 64
        val = struct.unpack('<d', raw_bytes)[0]
        components.append(round(val, 3))
    return tuple(components), pos


def read_packed_vector(data, pos, max_bits=24, scale=10):
    """Read SerializePackedVector."""
    bits, pos = serialize_int(data, pos, max_bits)
    comp_bits = bits + 2
    if comp_bits > 32 or comp_bits < 2:
        return None, pos
    bias = 1 << (bits + 1)
    x, pos = read_bits_le(data, pos, comp_bits)
    y, pos = read_bits_le(data, pos, comp_bits)
    z, pos = read_bits_le(data, pos, comp_bits)
    return (round((x - bias) / scale, 2), round((y - bias) / scale, 2), round((z - bias) / scale, 2)), pos


def read_rotation(data, pos):
    """Read FRotator::SerializeCompressedShort (3 flags + 16-bit components)."""
    flags, pos = read_bits_le(data, pos, 3)
    pitch = yaw = roll = 0.0
    if flags & 1:
        raw, pos = read_bits_le(data, pos, 16)
        pitch = raw * 360.0 / 65536.0
    if flags & 2:
        raw, pos = read_bits_le(data, pos, 16)
        yaw = raw * 360.0 / 65536.0
    if flags & 4:
        raw, pos = read_bits_le(data, pos, 16)
        roll = raw * 360.0 / 65536.0
    return (round(pitch, 2), round(yaw, 2), round(roll, 2)), pos


def _read_quantized_or_double_vec(data, pos, result, key):
    b_quantize = read_bit(data, pos)
    pos += 1
    if b_quantize:
        vec, pos = read_packed_vector(data, pos)
        result[key] = vec
    else:
        vec, pos = read_double_vector(data, pos)
        result[key] = vec
        result[f'{key}_raw'] = True
    return pos


def _decode_actor_transforms(data, pos, result):
    b_loc = read_bit(data, pos)
    pos += 1
    result['has_location'] = bool(b_loc)
    if b_loc:
        pos = _read_quantized_or_double_vec(data, pos, result, 'location')
    b_rot = read_bit(data, pos)
    pos += 1
    result['has_rotation'] = bool(b_rot)
    if b_rot:
        rot, pos = read_rotation(data, pos)
        result['rotation'] = rot
    b_scale = read_bit(data, pos)
    pos += 1
    result['has_scale'] = bool(b_scale)
    if b_scale:
        pos = _read_quantized_or_double_vec(data, pos, result, 'scale')
    b_vel = read_bit(data, pos)
    pos += 1
    result['has_velocity'] = bool(b_vel)
    if b_vel:
        pos = _read_quantized_or_double_vec(data, pos, result, 'velocity')
    return pos


def decode_new_actor(data, pos, end_pos, guid_cache):
    """Decode SerializeNewActor. GUIDs are BARE (not in export mode)."""
    result = {}
    start = pos
    actor_guid, pos = serialize_int_packed64(data, pos)
    actor_guid = actor_guid or 0
    result['guid'] = actor_guid
    result['value'] = actor_guid >> 1
    result['dynamic'] = bool(actor_guid & 1)
    if not result['dynamic']:
        result['bits'] = pos - start
        return result, pos
    if pos + 8 > end_pos:
        result['error'] = 'truncated'
        result['bits'] = pos - start
        return result, pos
    archetype, pos = serialize_int_packed64(data, pos)
    archetype = archetype or 0
    result['archetype'] = archetype
    result['archetype_value'] = archetype >> 1
    level, pos = serialize_int_packed64(data, pos)
    level = level or 0
    result['level'] = level
    result['level_value'] = level >> 1
    if pos >= end_pos:
        result['error'] = 'truncated_spawn'
        result['bits'] = pos - start
        return result, pos
    pos = _decode_actor_transforms(data, pos, result)
    result['bits'] = pos - start
    return result, pos


def _read_rep_payload(data, pos, end_pos, header, b_has_rep):
    if pos >= end_pos:
        return {'header': header, 'payload_bits': 0}, pos
    if not b_has_rep:
        return {'header': header, 'payload_bits': end_pos - pos}, end_pos
    num_payload, pos = serialize_int_packed(data, pos)
    num_payload = num_payload or 0
    if num_payload > 100000 or num_payload > (end_pos - pos):
        return {'header': header, 'payload_bits': num_payload, 'error': f'bad_payload_{num_payload}'}, pos
    return {'header': header, 'payload_bits': num_payload}, pos + num_payload


def _decode_null_sub_block(data, pos, end_pos, header, b_has_rep):
    if not b_has_rep:
        return {'header': header, 'payload_bits': end_pos - pos, 'null_sub': True}, end_pos
    if pos < end_pos:
        num_payload, pos = serialize_int_packed(data, pos)
        if num_payload is not None and num_payload <= (end_pos - pos):
            pos += num_payload
        return {'header': header, 'payload_bits': num_payload or 0, 'null_sub': True}, pos
    return {'header': header, 'payload_bits': 0, 'null_sub': True}, pos


def decode_content_block(data, pos, end_pos, direction, guid_cache):
    """Decode one content block header and advance past its payload."""
    if pos + 2 > end_pos:
        return None, pos
    b_has_rep = read_bit(data, pos)
    b_is_actor = read_bit(data, pos + 1)
    pos += 2
    header = {'has_rep': b_has_rep, 'is_actor': b_is_actor}
    if b_is_actor:
        return _read_rep_payload(data, pos, end_pos, header, b_has_rep)
    sub_guid, pos = serialize_int_packed64(data, pos)
    sub_guid = sub_guid or 0
    header['sub_guid'] = sub_guid
    header['sub_value'] = sub_guid >> 1 if sub_guid else 0
    if sub_guid == 0:
        return _decode_null_sub_block(data, pos, end_pos, header, b_has_rep)
    if direction == 'S>C':
        if pos >= end_pos:
            return {'header': header, 'payload_bits': 0}, pos
        b_stably = read_bit(data, pos)
        pos += 1
        header['stably_named'] = b_stably
        if not b_stably:
            class_guid, pos = serialize_int_packed64(data, pos)
            header['class_guid'] = class_guid
            header['class_value'] = class_guid >> 1 if class_guid else 0
            if class_guid == 0:
                return {'header': header, 'payload_bits': 0, 'is_destroy': True}, pos
    return _read_rep_payload(data, pos, end_pos, header, b_has_rep)
