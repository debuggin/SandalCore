#include <stdint.h>

#define RING_CAPACITY_SLOTS 65536U
#define RING_SLOT_MASK      0xFFFFU
#define FARCHIVE_BITPOS_OFFSET 0x48U
typedef struct {
    uint64_t qpc_ticks;
    uint32_t struct_type_hash;
    uint32_t channel_index;
    int32_t  bit_pos_before;
    int32_t  bit_pos_after;
    uint32_t ret_ok;
    uint32_t reserved;
} BitTraceSlot;
typedef struct {
    volatile uint32_t write_idx;
    uint8_t           pad0[60];
    volatile uint32_t read_idx;
    uint8_t           pad1[60];
    volatile uint64_t dropped_records;
    BitTraceSlot      slots[RING_CAPACITY_SLOTS];
} BitTraceRingBuffer;
extern BitTraceRingBuffer *g_ring;
static __thread const uint8_t *g_tls_farchive = 0;
static __thread int32_t g_tls_bit_before = 0;
static __thread uint32_t g_tls_struct_hash = 0;

void on_enter(const uint8_t *farchive, uint32_t struct_hash) {
    g_tls_farchive = farchive;
    g_tls_struct_hash = struct_hash;
    g_tls_bit_before = farchive ? *(const int32_t *)(farchive + FARCHIVE_BITPOS_OFFSET) : 0;
}

void on_leave(uint64_t qpc_ticks, uint32_t channel_index, uint32_t retval) {
    if (!g_tls_farchive || !g_ring) return;
    int32_t bit_after = *(const int32_t *)(g_tls_farchive + FARCHIVE_BITPOS_OFFSET);
    uint32_t w = __atomic_load_n(&g_ring->write_idx, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&g_ring->read_idx, __ATOMIC_ACQUIRE);
    if ((uint32_t)(w - r) >= RING_CAPACITY_SLOTS) {
        __atomic_fetch_add(&g_ring->dropped_records, 1U, __ATOMIC_RELAXED);
        g_tls_farchive = 0;
        return;
    }
    g_ring->slots[w & RING_SLOT_MASK] = (BitTraceSlot){
        qpc_ticks, g_tls_struct_hash, channel_index, g_tls_bit_before, bit_after, retval & 1U, 0U};
    __atomic_store_n(&g_ring->write_idx, w + 1U, __ATOMIC_RELEASE);
    g_tls_farchive = 0;
}
