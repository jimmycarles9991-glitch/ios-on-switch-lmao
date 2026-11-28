// SPDX-License-Identifier: BSD-2-Clause-Patent
// Lock-free SPSC rings, latest-wins rumble slot, and half-duplex UART link state for
// ARMv8.0-A (no LSE: acquire/release loads/stores only, zero LL/SC RMWs on steady-state paths).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_RX_RING_SIZE  1024u
#define SWITCH_JOYCON_RX_RING_MASK  (SWITCH_JOYCON_RX_RING_SIZE - 1u)
#define SWITCH_JOYCON_CMD_RING_SIZE 16u
#define SWITCH_JOYCON_CMD_RING_MASK (SWITCH_JOYCON_CMD_RING_SIZE - 1u)
#define SWITCH_JOYCON_MAX_OUTPUT    64u

enum switch_joycon_link_state {
    SWITCH_JOYCON_LINK_IDLE = 0,
    SWITCH_JOYCON_LINK_TX_BUSY = 1,
    SWITCH_JOYCON_LINK_WAIT_RX = 2,
    SWITCH_JOYCON_LINK_RX_DONE = 3
};

static inline uint32_t switch_joycon_link_load(const uint32_t *state)
{
    return __atomic_load_n(state, __ATOMIC_ACQUIRE);
}

static inline void switch_joycon_link_store(uint32_t *state, enum switch_joycon_link_state next)
{
    __atomic_store_n(state, (uint32_t)next, __ATOMIC_RELEASE);
}

static inline bool switch_joycon_link_tx_idle(const uint32_t *state)
{
    return switch_joycon_link_load(state) != (uint32_t)SWITCH_JOYCON_LINK_TX_BUSY;
}

// Single-producer (UART primary filter) / single-consumer (rail workloop) RX byte ring.
struct switch_joycon_spsc_rx {
    uint8_t ring[SWITCH_JOYCON_RX_RING_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t overflows;
};

static inline void switch_joycon_spsc_rx_init(struct switch_joycon_spsc_rx *r)
{
    r->head = 0;
    r->tail = 0;
    r->overflows = 0;
}

// Producer-side batch push: caller caches head/tail across one drain, then commits head once.
static inline bool switch_joycon_spsc_rx_stage(struct switch_joycon_spsc_rx *r, uint32_t *head, uint32_t tail,
                                               uint8_t byte)
{
    const uint32_t next = (*head + 1u) & SWITCH_JOYCON_RX_RING_MASK;
    if (next == tail) {
        __atomic_store_n(&r->overflows, __atomic_load_n(&r->overflows, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELAXED);
        return false;
    }
    r->ring[*head] = byte;
    *head = next;
    return true;
}

static inline void switch_joycon_spsc_rx_commit_head(struct switch_joycon_spsc_rx *r, uint32_t head)
{
    __atomic_store_n(&r->head, head, __ATOMIC_RELEASE);
}

static inline uint32_t switch_joycon_spsc_rx_peek_tail(const struct switch_joycon_spsc_rx *r)
{
    return __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

static inline uint32_t switch_joycon_spsc_rx_peek_head(const struct switch_joycon_spsc_rx *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
}

static inline void switch_joycon_spsc_rx_commit_tail(struct switch_joycon_spsc_rx *r, uint32_t tail)
{
    __atomic_store_n(&r->tail, tail, __ATOMIC_RELEASE);
}

// Consumer-only drop: preserves strict SPSC single-writer ownership of `head`.
static inline void switch_joycon_spsc_rx_drop(struct switch_joycon_spsc_rx *r)
{
    const uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    __atomic_store_n(&r->tail, head, __ATOMIC_RELEASE);
}

// Single-producer (serialized on setReport cold path) / single-consumer (rail workloop) output command ring.
struct switch_joycon_cmd_ring {
    uint8_t data[SWITCH_JOYCON_CMD_RING_SIZE][SWITCH_JOYCON_MAX_OUTPUT];
    uint8_t len[SWITCH_JOYCON_CMD_RING_SIZE];
    uint32_t gen[SWITCH_JOYCON_CMD_RING_SIZE];
    uint32_t head;
    uint32_t tail;
};

static inline void switch_joycon_cmd_ring_init(struct switch_joycon_cmd_ring *q)
{
    q->head = 0;
    q->tail = 0;
}

static inline bool switch_joycon_cmd_ring_push(struct switch_joycon_cmd_ring *q, uint32_t generation,
                                               const uint8_t *report, size_t length)
{
    if (length == 0u || length > SWITCH_JOYCON_MAX_OUTPUT)
        return false;
    const uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    const uint32_t tail = q->tail;
    if (tail - head >= SWITCH_JOYCON_CMD_RING_SIZE)
        return false;
    const uint32_t slot = tail & SWITCH_JOYCON_CMD_RING_MASK;
    memcpy(q->data[slot], report, length);
    q->len[slot] = (uint8_t)length;
    q->gen[slot] = generation;
    __atomic_store_n(&q->tail, tail + 1u, __ATOMIC_RELEASE);
    return true;
}

static inline bool switch_joycon_cmd_ring_empty(const struct switch_joycon_cmd_ring *q)
{
    return q->head == __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
}

static inline bool switch_joycon_cmd_ring_pop(struct switch_joycon_cmd_ring *q, uint32_t expected_gen,
                                              uint8_t out[SWITCH_JOYCON_MAX_OUTPUT], size_t *length_out)
{
    const uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    uint32_t head = q->head;
    while (head != tail) {
        const uint32_t slot = head & SWITCH_JOYCON_CMD_RING_MASK;
        const uint32_t gen = q->gen[slot];
        const size_t len = q->len[slot];
        ++head;
        if (gen != expected_gen || len == 0u || len > SWITCH_JOYCON_MAX_OUTPUT) {
            __atomic_store_n(&q->head, head, __ATOMIC_RELEASE);
            continue;
        }
        memcpy(out, q->data[slot], len);
        *length_out = len;
        __atomic_store_n(&q->head, head, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

// Consumer-only discard: keeps `tail` strictly producer-owned.
static inline void switch_joycon_cmd_ring_drop(struct switch_joycon_cmd_ring *q)
{
    const uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    __atomic_store_n(&q->head, tail, __ATOMIC_RELEASE);
}

// Lock-free latest-wins rumble slot (seqlock: single producer under cold producer lock,
// single workloop consumer). Zero LL/SC; consumer never spins unboundedly on odd seq.
struct switch_joycon_rumble_slot {
    uint32_t seq;
    uint32_t gen;
    uint8_t len;
    uint8_t data[SWITCH_JOYCON_MAX_OUTPUT];
    // Consumer-owned cache (workloop only).
    uint32_t seen_seq;
    uint8_t cached_len;
    uint8_t cached[SWITCH_JOYCON_MAX_OUTPUT];
    bool fresh;
};

static inline void switch_joycon_rumble_slot_init(struct switch_joycon_rumble_slot *r)
{
    r->seq = 0;
    r->gen = 0;
    r->len = 0;
    r->seen_seq = 0;
    r->cached_len = 0;
    r->fresh = false;
}

static inline void switch_joycon_rumble_slot_write(struct switch_joycon_rumble_slot *r, uint32_t generation,
                                                   const uint8_t *report, size_t length)
{
    if (length == 0u || length > SWITCH_JOYCON_MAX_OUTPUT)
        return;
    const uint32_t seq = r->seq;
    __atomic_store_n(&r->seq, seq + 1u, __ATOMIC_RELAXED);
    // ARMv8 stlr does not prevent later stores from floating before it; full release fence orders odd seq first.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->gen = generation;
    r->len = (uint8_t)length;
    memcpy(r->data, report, length);
    __atomic_store_n(&r->seq, seq + 2u, __ATOMIC_RELEASE);
}

// Consumer-only poll: fast path is one acquire load when unchanged. Returns true if a fresh
// report for `expected_gen` was latched into `cached`.
static inline bool switch_joycon_rumble_slot_poll(struct switch_joycon_rumble_slot *r, uint32_t expected_gen)
{
    uint32_t seq = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
    if (seq == r->seen_seq)
        return false;
    for (unsigned attempt = 0; attempt < 4u; ++attempt) {
        if (seq & 1u)
            return false;
        const uint32_t gen = r->gen;
        const uint8_t len = r->len;
        uint8_t tmp[SWITCH_JOYCON_MAX_OUTPUT];
        if (len > 0u && len <= SWITCH_JOYCON_MAX_OUTPUT)
            memcpy(tmp, r->data, len);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t seq2 = __atomic_load_n(&r->seq, __ATOMIC_RELAXED);
        if (seq == seq2) {
            r->seen_seq = seq;
            if (gen == expected_gen && len > 0u && len <= SWITCH_JOYCON_MAX_OUTPUT) {
                memcpy(r->cached, tmp, len);
                r->cached_len = len;
                r->fresh = true;
                return true;
            }
            return false;
        }
        seq = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
    }
    return false;
}

static inline void switch_joycon_rumble_slot_drop(struct switch_joycon_rumble_slot *r)
{
    r->seen_seq = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE) & ~1u;
    r->cached_len = 0;
    r->fresh = false;
}

#ifdef __cplusplus
}
#endif
