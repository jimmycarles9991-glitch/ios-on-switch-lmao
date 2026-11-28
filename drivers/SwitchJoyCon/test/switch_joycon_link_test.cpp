// drivers/SwitchJoyCon/SwitchJoyConLink.h: lock-free SPSC RX ring, 16-slot command ring,
// latest-wins rumble seqlock, and half-duplex link state machine transitions.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "../SwitchJoyConLink.h"

namespace {

TEST(SwitchJoyConLink, SpscRxRingHandlesWraparoundOverflowAndDrop)
{
    switch_joycon_spsc_rx rx{};
    switch_joycon_spsc_rx_init(&rx);

    // Fill to capacity (SWITCH_JOYCON_RX_RING_SIZE - 1 bytes) and verify overflow counts extra writes.
    uint32_t head = rx.head;
    const uint32_t tail = switch_joycon_spsc_rx_peek_tail(&rx);
    for (uint32_t i = 0; i < SWITCH_JOYCON_RX_RING_SIZE - 1u; ++i)
        EXPECT_TRUE(switch_joycon_spsc_rx_stage(&rx, &head, tail, static_cast<uint8_t>(i & 0xffu)));
    EXPECT_FALSE(switch_joycon_spsc_rx_stage(&rx, &head, tail, 0xaa));
    EXPECT_EQ(rx.overflows, 1u);
    switch_joycon_spsc_rx_commit_head(&rx, head);

    // Drain half, then push across wraparound.
    uint32_t curTail = rx.tail;
    const uint32_t curHead = switch_joycon_spsc_rx_peek_head(&rx);
    ASSERT_EQ((curHead - curTail) & SWITCH_JOYCON_RX_RING_MASK, SWITCH_JOYCON_RX_RING_SIZE - 1u);
    for (uint32_t i = 0; i < 600u; ++i) {
        EXPECT_EQ(rx.ring[curTail], static_cast<uint8_t>(i & 0xffu));
        curTail = (curTail + 1u) & SWITCH_JOYCON_RX_RING_MASK;
    }
    switch_joycon_spsc_rx_commit_tail(&rx, curTail);

    head = rx.head;
    const uint32_t newTail = switch_joycon_spsc_rx_peek_tail(&rx);
    for (uint32_t i = 0; i < 600u; ++i)
        EXPECT_TRUE(switch_joycon_spsc_rx_stage(&rx, &head, newTail, static_cast<uint8_t>((i + 0x55u) & 0xffu)));
    switch_joycon_spsc_rx_commit_head(&rx, head);

    // Consumer drop clears ring without mutating producer-owned head.
    const uint32_t savedHead = rx.head;
    switch_joycon_spsc_rx_drop(&rx);
    EXPECT_EQ(rx.head, savedHead);
    EXPECT_EQ(rx.tail, savedHead);
}

TEST(SwitchJoyConLink, CommandRingSupports16SlotsFifoGenerationFilteringAndDrop)
{
    switch_joycon_cmd_ring q{};
    switch_joycon_cmd_ring_init(&q);
    EXPECT_TRUE(switch_joycon_cmd_ring_empty(&q));

    for (uint32_t i = 0; i < SWITCH_JOYCON_CMD_RING_SIZE; ++i) {
        const uint8_t pkt[4] = {0x01, 0x00, static_cast<uint8_t>(i), 0x40};
        EXPECT_TRUE(switch_joycon_cmd_ring_push(&q, 1u, pkt, sizeof(pkt)));
    }
    // 17th push fails with ring full.
    const uint8_t extra[4] = {0x01, 0x00, 0xff, 0x40};
    EXPECT_FALSE(switch_joycon_cmd_ring_push(&q, 1u, extra, sizeof(extra)));
    EXPECT_FALSE(switch_joycon_cmd_ring_empty(&q));

    uint8_t out[SWITCH_JOYCON_MAX_OUTPUT];
    size_t len = 0;
    for (uint32_t i = 0; i < SWITCH_JOYCON_CMD_RING_SIZE; ++i) {
        ASSERT_TRUE(switch_joycon_cmd_ring_pop(&q, 1u, out, &len));
        EXPECT_EQ(len, 4u);
        EXPECT_EQ(out[2], static_cast<uint8_t>(i));
    }
    EXPECT_TRUE(switch_joycon_cmd_ring_empty(&q));

    // Stale generation entries are skipped cleanly in favor of the live generation.
    const uint8_t stale[3] = {0x01, 0x00, 0xaa};
    const uint8_t live[3] = {0x01, 0x00, 0xbb};
    EXPECT_TRUE(switch_joycon_cmd_ring_push(&q, 1u, stale, sizeof(stale)));
    EXPECT_TRUE(switch_joycon_cmd_ring_push(&q, 2u, live, sizeof(live)));
    ASSERT_TRUE(switch_joycon_cmd_ring_pop(&q, 2u, out, &len));
    EXPECT_EQ(out[2], 0xbbu);
    EXPECT_TRUE(switch_joycon_cmd_ring_empty(&q));

    // Drop preserves producer-owned tail.
    EXPECT_TRUE(switch_joycon_cmd_ring_push(&q, 2u, live, sizeof(live)));
    const uint32_t savedTail = q.tail;
    switch_joycon_cmd_ring_drop(&q);
    EXPECT_EQ(q.tail, savedTail);
    EXPECT_TRUE(switch_joycon_cmd_ring_empty(&q));
}

TEST(SwitchJoyConLink, RumbleSlotCoalescesLatestAndFiltersGenerationAndOddSeq)
{
    switch_joycon_rumble_slot r{};
    switch_joycon_rumble_slot_init(&r);
    EXPECT_FALSE(switch_joycon_rumble_slot_poll(&r, 1u));

    const uint8_t first[10] = {0x10, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t latest[10] = {0x10, 0, 9, 8, 7, 6, 5, 4, 3, 2};
    switch_joycon_rumble_slot_write(&r, 1u, first, sizeof(first));
    switch_joycon_rumble_slot_write(&r, 1u, latest, sizeof(latest));

    ASSERT_TRUE(switch_joycon_rumble_slot_poll(&r, 1u));
    EXPECT_TRUE(r.fresh);
    EXPECT_EQ(r.cached_len, 10u);
    EXPECT_EQ(memcmp(r.cached, latest, sizeof(latest)), 0);

    // Unchanged poll takes fast path (returns false).
    EXPECT_FALSE(switch_joycon_rumble_slot_poll(&r, 1u));

    // Mismatched generation is ignored.
    switch_joycon_rumble_slot_write(&r, 1u, first, sizeof(first));
    EXPECT_FALSE(switch_joycon_rumble_slot_poll(&r, 2u));

    // Odd sequence (writer mid-update) does not spin unboundedly and leaves state intact.
    r.seq |= 1u;
    EXPECT_FALSE(switch_joycon_rumble_slot_poll(&r, 2u));
    r.seq &= ~1u;

    switch_joycon_rumble_slot_drop(&r);
    EXPECT_FALSE(r.fresh);
    EXPECT_EQ(r.cached_len, 0u);
}

TEST(SwitchJoyConLink, HalfDuplexStateMachineTransitions)
{
    uint32_t state = SWITCH_JOYCON_LINK_IDLE;
    EXPECT_TRUE(switch_joycon_link_tx_idle(&state));

    switch_joycon_link_store(&state, SWITCH_JOYCON_LINK_TX_BUSY);
    EXPECT_FALSE(switch_joycon_link_tx_idle(&state));
    EXPECT_EQ(switch_joycon_link_load(&state), static_cast<uint32_t>(SWITCH_JOYCON_LINK_TX_BUSY));

    switch_joycon_link_store(&state, SWITCH_JOYCON_LINK_WAIT_RX);
    EXPECT_TRUE(switch_joycon_link_tx_idle(&state));
    EXPECT_EQ(switch_joycon_link_load(&state), static_cast<uint32_t>(SWITCH_JOYCON_LINK_WAIT_RX));

    switch_joycon_link_store(&state, SWITCH_JOYCON_LINK_RX_DONE);
    EXPECT_TRUE(switch_joycon_link_tx_idle(&state));
    EXPECT_EQ(switch_joycon_link_load(&state), static_cast<uint32_t>(SWITCH_JOYCON_LINK_RX_DONE));

    switch_joycon_link_store(&state, SWITCH_JOYCON_LINK_IDLE);
    EXPECT_TRUE(switch_joycon_link_tx_idle(&state));
}

}  // namespace
