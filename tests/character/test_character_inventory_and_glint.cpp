#include "../world/test_harness.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

struct BagSlot {
    uint32_t item_id{0};
    uint32_t count{0};
    uint32_t max_stack{20};
    int64_t updated_ms{0};
    bool dirty{false};
};

[[nodiscard]] bool merge_bag_slots(BagSlot& dst, BagSlot& src, int64_t now_ms) noexcept {
    if (src.item_id == 0U || src.count == 0U || dst.max_stack == 0U) {
        return false;
    }
    if (dst.item_id != 0U && dst.item_id != src.item_id) {
        return false;
    }
    if (dst.count >= dst.max_stack) {
        return false;
    }
    const uint32_t space = dst.max_stack - dst.count;
    const uint32_t moved = std::min(space, src.count);
    dst.item_id = src.item_id;
    dst.count += moved;
    dst.updated_ms = now_ms;
    dst.dirty = true;

    src.count -= moved;
    if (src.count == 0U) {
        src.item_id = 0U;
    }
    src.updated_ms = now_ms;
    src.dirty = true;
    return true;
}

[[nodiscard]] constexpr int64_t glint_unit_value(uint32_t item_id) noexcept {
    constexpr std::array<int64_t, 6> kGlintValues{10, 50, 100, 500, 1000, 5000};
    if (item_id < 1003001U || item_id > 1003006U) {
        return 0;
    }
    return kGlintValues[item_id - 1003001U];
}

struct VendorPurchaseResult {
    bool ok{false};
    int64_t change_copper{0};
};

[[nodiscard]] constexpr VendorPurchaseResult execute_vendor_glint_purchase(
    uint32_t glint_item_id,
    uint32_t glint_count,
    int64_t vendor_price_copper) noexcept {
    const int64_t unit = glint_unit_value(glint_item_id);
    if (unit <= 0 || glint_count == 0U || vendor_price_copper <= 0) {
        return {};
    }
    const int64_t tendered = unit * static_cast<int64_t>(glint_count);
    if (tendered < vendor_price_copper) {
        return {};
    }
    return {true, tendered - vendor_price_copper};
}

int test_inventory_bag_slot_stacking() {
    sandal::test::ManualClock clock{0};
    clock.advance_ms(250);

    BagSlot dst{7001U, 14U, 20U, 0, false};
    BagSlot src{7001U, 9U, 20U, 0, false};
    SANDAL_ASSERT(merge_bag_slots(dst, src, clock.now_ms()));
    SANDAL_ASSERT(dst.count == 20U && dst.dirty && dst.updated_ms == 250);
    SANDAL_ASSERT(src.count == 3U && src.item_id == 7001U && src.dirty);

    SANDAL_ASSERT(!merge_bag_slots(dst, src, clock.now_ms()));

    BagSlot empty_dst{0U, 0U, 20U, 0, false};
    clock.advance_ms(50);
    SANDAL_ASSERT(merge_bag_slots(empty_dst, src, clock.now_ms()));
    SANDAL_ASSERT(empty_dst.item_id == 7001U && empty_dst.count == 3U);
    SANDAL_ASSERT(src.item_id == 0U && src.count == 0U);

    BagSlot other{8002U, 5U, 20U, 0, false};
    SANDAL_ASSERT(!merge_bag_slots(empty_dst, other, clock.now_ms()));
    return 0;
}

int test_vendor_glint_denomination_change() {
    sandal::test::ManualClock clock{0};
    SANDAL_ASSERT(glint_unit_value(1003001U) == 10);
    SANDAL_ASSERT(glint_unit_value(1003003U) == 100);
    SANDAL_ASSERT(glint_unit_value(1003006U) == 5000);
    SANDAL_ASSERT(glint_unit_value(1003000U) == 0);
    SANDAL_ASSERT(glint_unit_value(1003007U) == 0);

    clock.advance_ticks(3, 30);
    const auto exact = execute_vendor_glint_purchase(1003003U, 3U, 300);
    SANDAL_ASSERT(exact.ok && exact.change_copper == 0);

    const auto with_change = execute_vendor_glint_purchase(1003004U, 2U, 725);
    SANDAL_ASSERT(with_change.ok && with_change.change_copper == 275);

    const auto short_funds = execute_vendor_glint_purchase(1003001U, 4U, 41);
    SANDAL_ASSERT(!short_funds.ok && short_funds.change_copper == 0);
    SANDAL_ASSERT(!execute_vendor_glint_purchase(1003002U, 1U, 0).ok);
    SANDAL_ASSERT(clock.now_ms() == 100);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "inventory_bag_slot_stacking")) {
        if (test_inventory_bag_slot_stacking() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "vendor_glint_denomination_change")) {
        if (test_vendor_glint_denomination_change() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
