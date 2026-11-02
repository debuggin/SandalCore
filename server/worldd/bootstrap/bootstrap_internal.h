// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include <cmath>
#include <optional>
#include <string>

#include "bootstrap/bootstrap.h"
#include "bootstrap/captured_replies.h"
#include "protocol/schema/generated/index.h"

namespace aoc::worldd::bootstrap {

inline repl::Value object(const protocol::emit::FIntrepidNetworkGUID &g) { return repl::Value(g); }

inline int round_to_int(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

inline uint16_t compress_short(double degrees) {
    return static_cast<uint16_t>(round_to_int(static_cast<float>(degrees) * 65536.0f / 360.0f) & 0xFFFF);
}

inline uint16_t compress_byte(double degrees) {
    return static_cast<uint16_t>(round_to_int(static_cast<float>(degrees) * 256.0f / 360.0f) & 0xFF);
}

inline std::string player_name_private(const std::string &option) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < option.size() && chars < 20;) {
        size_t n = 1;
        const unsigned char c = static_cast<unsigned char>(option[i]);
        if (c >= 0xF0)
            n = 4;
        else if (c >= 0xE0)
            n = 3;
        else if (c >= 0xC0)
            n = 2;
        out.append(option, i, n);
        i += n;
        ++chars;
    }
    return out;
}

inline void write_bits_of(protocol::emit::BunchWriter &out, const captured::CapturedBits &b) {
    const auto bytes = captured::bytes_of(b);
    out.write_bit_range(bytes.data(), 0, b.bits);
}

inline repl::Subobject make_component(const protocol::emit::FIntrepidNetworkGUID &guid, const char *name, uint32_t checksum) {
    repl::Subobject s;
    s.guid = guid;
    s.name = name;
    s.checksum = checksum;
    s.obj.schema = ::aoc::protocol::schema::gen::find_component_schema(name);
    return s;
}

inline bool make_resource_component(const protocol::emit::FIntrepidNetworkGUID &guid, size_t index, repl::Subobject &out) {
    out = repl::Subobject{};
    out.guid = guid;
    out.dynamic_class = resource_storage_class_export();
    out.obj.schema = ::aoc::protocol::schema::gen::find_class_schema("UResourceStorageComponent");
    return out.obj.schema && out.obj.state.set(*out.obj.schema, "ItemLocationType", repl::Value::u64(11 + index));
}

inline std::optional<ItemStorage> item_storage_of(StorageKind kind) {
    switch (kind) {
        case StorageKind::Backpack:
            return ItemStorage::Backpack;
        case StorageKind::Equipment:
            return ItemStorage::Equipment;
        case StorageKind::QuestStorage:
            return ItemStorage::Quest;
        case StorageKind::Resource0:
            return ItemStorage::Resource0;
        case StorageKind::Resource1:
            return ItemStorage::Resource1;
        case StorageKind::Resource2:
            return ItemStorage::Resource2;
    }
    return std::nullopt;
}

}  // namespace aoc::worldd::bootstrap
