// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/zoi_surface.cpp (see zoi_surface.h)
// ============================================================================
#include "world/zoi_surface.h"

#include "world/zoi_surface_table.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

namespace aoc::worldd::world {

namespace zs = zoi_surface;

namespace {

bool is_disk_data_mode() noexcept {
    for (const char* k : {"AOC_DATA_MODE_ZOIS", "AOC_DATA_MODE_DEFAULT"}) {
        if (const char* v = std::getenv(k); v && *v) return v[0] == 'd' || v[0] == 'D';
    }
    return false;
}

const char* resolve_zois_tsv_path() noexcept {
    if (const char* p = std::getenv("AOC_ZOIS_TSV_PATH"); p && *p) return p;
    return "/content/zois.tsv";
}

std::vector<uint64_t> read_zois_tsv_guids(const char* path) {
    std::vector<uint64_t> out;
    std::ifstream in(path);
    if (!in.is_open() && std::string(path) == "/content/zois.tsv") in.open("dist/content/zois.tsv");
    if (!in.is_open()) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("guid", 0) == 0) continue;
        char* endp = nullptr;
        const unsigned long long g = std::strtoull(line.c_str(), &endp, 10);
        if (endp != line.c_str() && g != 0) out.push_back(static_cast<uint64_t>(g));
    }
    return out;
}

const std::vector<uint64_t>& cached_tsv_guids() {
    static std::once_flag once;
    static std::vector<uint64_t> cached;
    std::call_once(once, [] { cached = read_zois_tsv_guids(resolve_zois_tsv_path()); });
    return cached;
}

}  // namespace

uint64_t zoi_at(double x_cm, double y_cm) {
    if (!std::isfinite(x_cm) || !std::isfinite(y_cm)) return 0;
    const double fx = std::floor(x_cm), fy = std::floor(y_cm);
    // far outside the table's box, before the integer conversion
    if (std::fabs(fx) > 1.0e9 || std::fabs(fy) > 1.0e9) return 0;
    const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
    // floor division: the chunk of a negative position is the one below
    auto floor_div = [](int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const int64_t cx = floor_div(ix, zs::kChunkCm), cy = floor_div(iy, zs::kChunkCm);
    const int64_t col = cx - zs::kMinCx, row = cy - zs::kMinCy;
    if (col < 0 || col >= zs::kColumns || row < 0 || row >= zs::kRowsOfChunks) return 0;
    const uint32_t slot = zs::kIndex[row * zs::kColumns + col];
    if (slot == zs::kNone) return 0;
    if (slot & zs::kUniform) return zs::kPalette[slot & 0xFFu];
    // the cell inside the chunk, row-major
    const int64_t cell_x = (ix - cx * zs::kChunkCm) / zs::kCellCm, cell_y = (iy - cy * zs::kChunkCm) / zs::kCellCm;
    const int64_t want = cell_y * zs::kCells + cell_x;
    int64_t at = 0;
    for (size_t p = slot; p + 2 < std::size(zs::kRuns); p += 3) {
        at += static_cast<int64_t>(zs::kRuns[p]) | (static_cast<int64_t>(zs::kRuns[p + 1]) << 8);
        if (want < at) return zs::kPalette[zs::kRuns[p + 2]];
    }
    return 0;
}

std::vector<uint64_t> zoi_surface_palette() {
    std::vector<uint64_t> pal(std::begin(zs::kPalette), std::end(zs::kPalette));
    const std::vector<uint64_t> tsv = is_disk_data_mode() ? read_zois_tsv_guids(resolve_zois_tsv_path()) : cached_tsv_guids();
    for (uint64_t g : tsv) {
        if (std::find(pal.begin(), pal.end(), g) == pal.end()) pal.push_back(g);
    }
    std::sort(pal.begin(), pal.end());
    return pal;
}

ZoiSurfaceInfo zoi_surface_info() {
    if (!is_disk_data_mode()) (void)cached_tsv_guids();
    ZoiSurfaceInfo info;
    for (const uint32_t slot : zs::kIndex) {
        if (slot == zs::kNone) continue;
        ++info.chunks;
        if (slot & zs::kUniform) ++info.uniform;
    }
    return info;
}

}  // namespace aoc::worldd::world
