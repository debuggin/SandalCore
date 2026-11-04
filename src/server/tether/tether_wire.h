// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace tether::proto {

inline void varint_encode(std::vector<uint8_t>& out, uint64_t v) {
    do {
        uint8_t b = static_cast<uint8_t>(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}

inline uint64_t varint_decode(const uint8_t* buf, size_t len, size_t& off) {
    uint64_t r = 0;
    int shift  = 0;
    while (off < len) {
        uint8_t b = buf[off++];
        r |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return r;
        shift += 7;
        if (shift >= 64) break;
    }
    return r;
}

inline void field_string(std::vector<uint8_t>& out, uint32_t fn, const std::string& s) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

inline void field_bytes(std::vector<uint8_t>& out, uint32_t fn,
                        const std::vector<uint8_t>& b) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

inline void field_message(std::vector<uint8_t>& out, uint32_t fn,
                          const std::vector<uint8_t>& m) {
    varint_encode(out, (static_cast<uint64_t>(fn) << 3) | 2);
    varint_encode(out, m.size());
    out.insert(out.end(), m.begin(), m.end());
}

struct Field {
    uint32_t             field_number = 0;
    int                  wire_type    = 0;
    uint64_t             varint_value = 0;
    std::vector<uint8_t> bytes_value;

    [[nodiscard]] std::string as_string() const {
        return {bytes_value.begin(), bytes_value.end()};
    }
};

inline std::vector<Field> decode(const uint8_t* buf, size_t len) {
    std::vector<Field> out;
    size_t off = 0;
    while (off < len) {
        const size_t saved = off;
        const uint64_t tag = varint_decode(buf, len, off);
        if (off == saved) break;

        Field f;
        f.field_number = static_cast<uint32_t>(tag >> 3);
        f.wire_type    = static_cast<int>(tag & 7);

        if (f.wire_type == 0) {
            f.varint_value = varint_decode(buf, len, off);
        } else if (f.wire_type == 2) {
            const uint64_t sz = varint_decode(buf, len, off);
            if (off + sz <= len) {
                f.bytes_value.assign(buf + off,
                                     buf + off + static_cast<size_t>(sz));
                off += static_cast<size_t>(sz);
            }
        } else if (f.wire_type == 1) {
            if (off + 8 <= len) off += 8;
        } else if (f.wire_type == 5) {
            if (off + 4 <= len) off += 4;
        } else {
            break;
        }
        out.push_back(std::move(f));
    }
    return out;
}

} // namespace tether::proto

namespace tether::wire {

inline constexpr uint32_t APP_NUMBER   = 42330;
inline constexpr size_t   HEADER_SIZE  = 40;

enum class MsgId : uint8_t {
    data        = 1,
    ack         = 2,
    ask_window  = 3,
    tell_window = 4,
    connect     = 5,
};

inline const char* msg_name(MsgId id) noexcept {
    switch (id) {
    case MsgId::data:        return "Data";
    case MsgId::ack:         return "ACK";
    case MsgId::ask_window:  return "AskWindow";
    case MsgId::tell_window: return "TellWindow";
    case MsgId::connect:     return "Connect";
    default:                 return "?";
    }
}

struct Header {
    uint32_t magic_number           = 0;
    uint32_t session_id             = 0;
    uint32_t window_size            = 0;
    uint16_t fragment_number        = 0;
    uint16_t data_length            = 0;
    int64_t  sequence_number        = 0;
    int64_t  next_expected_recv_seq = 0;
    int64_t  timestamp_msec         = 0;

    [[nodiscard]] uint8_t version()  const noexcept {
        return static_cast<uint8_t>((magic_number >> 8) & 0xFF);
    }
    [[nodiscard]] MsgId   msg_id()   const noexcept {
        return static_cast<MsgId>(magic_number & 0xFF);
    }
    [[nodiscard]] bool    is_valid() const noexcept {
        return (magic_number >> 16) == APP_NUMBER;
    }
};

// Big-endian read/write helpers (protocol is big-endian on wire)
inline void     w16(uint8_t* p, uint16_t v)  noexcept {
    p[0] = uint8_t(v >> 8); p[1] = uint8_t(v);
}
inline void     w32(uint8_t* p, uint32_t v)  noexcept {
    p[0]=uint8_t(v>>24); p[1]=uint8_t(v>>16); p[2]=uint8_t(v>>8); p[3]=uint8_t(v);
}
inline void     w64(uint8_t* p, int64_t v)   noexcept {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(u >> (56 - 8*i));
}
inline uint16_t r16(const uint8_t* p)        noexcept {
    return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}
inline uint32_t r32(const uint8_t* p)        noexcept {
    return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
inline int64_t  r64(const uint8_t* p)        noexcept {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | p[i];
    return static_cast<int64_t>(u);
}

inline std::vector<uint8_t> encode(const Header& h) {
    std::vector<uint8_t> b(HEADER_SIZE, 0);
    w32(b.data() +  0, h.magic_number);
    w32(b.data() +  4, h.session_id);
    w32(b.data() +  8, h.window_size);
    w16(b.data() + 12, h.fragment_number);
    w16(b.data() + 14, h.data_length);
    w64(b.data() + 16, h.sequence_number);
    w64(b.data() + 24, h.next_expected_recv_seq);
    w64(b.data() + 32, h.timestamp_msec);
    return b;
}

inline bool decode(const uint8_t* buf, size_t len, Header& out) {
    if (len < HEADER_SIZE) return false;
    out.magic_number           = r32(buf +  0);
    out.session_id             = r32(buf +  4);
    out.window_size            = r32(buf +  8);
    out.fragment_number        = r16(buf + 12);
    out.data_length            = r16(buf + 14);
    out.sequence_number        = r64(buf + 16);
    out.next_expected_recv_seq = r64(buf + 24);
    out.timestamp_msec         = r64(buf + 32);
    return true;
}

inline void decode_tag_entry(const std::vector<uint8_t>& bytes,
                             std::map<std::string, std::string>& tags) {
    std::string k;
    std::string v;
    for (const auto& ef : proto::decode(bytes.data(), bytes.size())) {
        if      (ef.field_number == 1 && ef.wire_type == 2) k = ef.as_string();
        else if (ef.field_number == 2 && ef.wire_type == 2) v = ef.as_string();
    }
    if (!k.empty()) tags[k] = v;
}

struct Message {
    std::map<std::string, std::string> tags;
    std::string                        data_string;
    std::vector<uint8_t>               data_bytes;

    [[nodiscard]] std::vector<uint8_t> encode() const {
        std::vector<uint8_t> out;
        for (const auto& [k, v] : tags) {
            std::vector<uint8_t> entry;
            proto::field_string(entry, 1, k);
            proto::field_string(entry, 2, v);
            proto::field_message(out, 1, entry);
        }
        if (!data_string.empty()) proto::field_string(out, 2, data_string);
        if (!data_bytes.empty())  proto::field_bytes (out, 3, data_bytes);
        return out;
    }

    static Message decode(const uint8_t* buf, size_t len) {
        Message msg;
        for (const auto& f : proto::decode(buf, len)) {
            if (f.field_number == 1 && f.wire_type == 2) {
                decode_tag_entry(f.bytes_value, msg.tags);
            } else if (f.field_number == 2 && f.wire_type == 2) {
                msg.data_string = f.as_string();
            } else if (f.field_number == 3 && f.wire_type == 2) {
                msg.data_bytes = f.bytes_value;
            }
        }
        return msg;
    }
};

inline std::string hex_dump(const uint8_t* data, size_t len, size_t max_bytes = 64) {
    std::ostringstream oss;
    const size_t n = len < max_bytes ? len : max_bytes;
    for (size_t i = 0; i < n; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(data[i]);
        if (i + 1 < n) oss << ' ';
    }
    if (len > max_bytes) oss << "... (" << len << "B)";
    return oss.str();
}

} // namespace tether::wire
