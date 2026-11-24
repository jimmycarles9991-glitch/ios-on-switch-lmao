/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ack.h"

#include <stddef.h>

union switch_test_memory {
    uint64_t alignment;
    uint8_t bytes[768];
};

static union switch_test_memory switch_wlc[2], switch_pub[2], switch_tunables;
static void *switch_keys[2][132], *switch_other_keys[132];
static uint32_t switch_objects[80][57], switch_aux[80], switch_object_count;
static const uint8_t switch_address[6] = {2, 3, 4, 5, 6, 7};
static uint8_t switch_receiver_address[6];
static void *switch_receiver_wlc;
static int32_t switch_receiver_index;
static uint32_t switch_allocated, switch_cleared, switch_programmed, switch_fault;
static int32_t switch_allocate_error, switch_set_error, switch_bad_index;
static uint8_t switch_empty_object;

static struct {
    uint32_t sequence, clears, cleared_at, infos, info_at, info_index, info, sets, set_at, transmitter;
    int32_t set_index, set_error;
    uint8_t address[6];
} switch_network;

uint32_t switch_ack_test_results[45];
volatile uint32_t switch_ack_test_done;

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

static void switch_zero(void *output, size_t bytes)
{
    uint8_t *out = output;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = 0;
}

static int switch_equal(const uint8_t *left, const uint8_t *right)
{
    for (size_t index = 0; index < 6; ++index)
        if (left[index] != right[index])
            return 0;
    return 1;
}

static void switch_initialize_wlc(unsigned index)
{
    switch_zero(&switch_wlc[index], sizeof switch_wlc[index]);
    switch_zero(&switch_pub[index], sizeof switch_pub[index]);
    switch_zero(switch_keys[index], sizeof switch_keys[index]);
    *(void **)(switch_wlc[index].bytes + 0) = switch_pub[index].bytes;
    *(void **)(switch_wlc[index].bytes + 0x2a0) = switch_keys[index];
    switch_wlc[index].bytes[25] = 1;
    *(void **)(switch_pub[index].bytes + 28) = switch_tunables.bytes;
    *(uint32_t *)(switch_pub[index].bytes + 0xbc) = 40;
    *(uint32_t *)(switch_pub[index].bytes + 0xc0) = 64;
}

static void switch_reset(void)
{
    switch_a3_ack_detached();
    switch_initialize_wlc(0);
    switch_initialize_wlc(1);
    switch_zero(switch_objects, sizeof switch_objects);
    switch_zero(switch_other_keys, sizeof switch_other_keys);
    switch_object_count = switch_allocated = switch_cleared = switch_programmed = switch_fault = 0;
    switch_allocate_error = switch_set_error = switch_bad_index = 0;
    switch_empty_object = 0;
    switch_zero(&switch_network, sizeof switch_network);
    switch_receiver_wlc = NULL;
    switch_receiver_index = -1;
}

int32_t switch_a3_key_allocate(void *wlc, uint32_t first, uint32_t count, uint32_t consecutive)
{
    ++switch_allocated;
    if (first != 4 || !count || count > 36 || consecutive != 1)
        switch_fault = 1;
    if (switch_allocate_error)
        return switch_allocate_error;
    if (switch_bad_index)
        return switch_bad_index;
    void **keys = *(void ***)((uint8_t *)wlc + 0x2a0);
    for (uint32_t index = first; index < first + count; ++index) {
        if (keys[index])
            continue;
        if (switch_empty_object)
            return index;
        if (switch_object_count >= 80) {
            switch_fault = 2;
            return -27;
        }
        uint8_t *key = (uint8_t *)switch_objects[switch_object_count];
        switch_zero(key, 228);
        key[6] = index;
        key[9] = 64;
        *(void **)(key + 224) = &switch_aux[switch_object_count++];
        keys[index] = key;
        return index;
    }
    return -22;
}

static uint32_t switch_network_call(void *wlc, uint32_t *calls)
{
    if (!((uint8_t *)wlc)[25])
        switch_fault = 5;
    ++*calls;
    return ++switch_network.sequence;
}

uint32_t switch_a3_amt_clear(void *wlc, int32_t index)
{
    if (index == 59) {
        switch_network.cleared_at = switch_network_call(wlc, &switch_network.clears);
        return 0x8008;
    }
    void **keys = *(void ***)((uint8_t *)wlc + 0x2a0);
    if (!((uint8_t *)wlc)[25] || index < 0 || index >= 36 || !keys[index + 4])
        switch_fault = 3;
    ++switch_cleared;
    switch_receiver_wlc = wlc;
    switch_receiver_index = index;
    switch_zero(switch_receiver_address, sizeof switch_receiver_address);
    return 0x8008;
}

int32_t switch_a3_receiver_set(void *wlc, int32_t index, const uint8_t address[6])
{
    if (!switch_cleared || switch_receiver_wlc != wlc || switch_receiver_index != index)
        switch_fault = 4;
    ++switch_programmed;
    if (switch_set_error)
        return switch_set_error;
    for (size_t offset = 0; offset < 6; ++offset)
        switch_receiver_address[offset] = address[offset];
    return 0;
}

void switch_a3_amt_info_set(void *wlc, uint32_t index, uint32_t info)
{
    switch_network.info_at = switch_network_call(wlc, &switch_network.infos);
    switch_network.info_index = index;
    switch_network.info = info;
}

int32_t switch_a3_network_set(void *wlc, int32_t index, const uint8_t address[6], uint32_t transmitter)
{
    switch_network.set_at = switch_network_call(wlc, &switch_network.sets);
    switch_network.set_index = index;
    switch_network.transmitter = transmitter;
    for (size_t offset = 0; offset < 6; ++offset)
        switch_network.address[offset] = address[offset];
    return switch_network.set_error;
}

static uint32_t switch_test_prepare(void)
{
    switch_reset();
    for (unsigned index = 4; index < 10; ++index)
        switch_keys[0][index] = (void *)1;
    SWITCH_REQUIRE(switch_a3_ack_prepare(switch_wlc[0].bytes) == 0);
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 1 && !switch_cleared && !switch_programmed);
    SWITCH_REQUIRE(switch_keys[0][10] && ((uint8_t *)switch_keys[0][10])[6] == 10);
    SWITCH_REQUIRE(switch_a3_ack_prepare(switch_wlc[0].bytes) == 0 && switch_allocated == 1);
    return 0;
}

static uint32_t switch_test_enable(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 1 && switch_cleared == 1 && switch_programmed == 1);
    SWITCH_REQUIRE(switch_receiver_index == 0 && switch_equal(switch_receiver_address, switch_address));
    return 0;
}

static uint32_t switch_test_reopen(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    void *key = switch_keys[0][4];
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, NULL, 0));
    SWITCH_REQUIRE(switch_programmed == 1 && switch_cleared == 2 && switch_keys[0][4] == key);
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 1 && switch_cleared == 3 && switch_programmed == 2);
    return 0;
}

static uint32_t switch_test_address_change(void)
{
    static const uint8_t other[6] = {2, 8, 7, 6, 5, 4};
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, other, 1));
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 1 && switch_equal(switch_receiver_address, other));
    return 0;
}

static uint32_t switch_test_reconcile(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    switch_zero(switch_receiver_address, sizeof switch_receiver_address);
    SWITCH_REQUIRE(!switch_a3_ack_reconcile(switch_wlc[0].bytes));
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 1 && switch_cleared == 2 && switch_programmed == 2);
    SWITCH_REQUIRE(switch_equal(switch_receiver_address, switch_address));
    return 0;
}

static uint32_t switch_test_down(unsigned variant)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_prepare(switch_wlc[0].bytes));
    switch_wlc[0].bytes[25] = 0;
    if (variant == 0) {
        SWITCH_REQUIRE(!switch_a3_ack_prepare(switch_wlc[0].bytes));
    } else {
        SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1) == -11);
        if (variant == 2)
            SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, NULL, 0) == -11);
    }
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 1 && !switch_cleared && !switch_programmed);
    switch_wlc[0].bytes[25] = 1;
    SWITCH_REQUIRE(!switch_a3_ack_reconcile(switch_wlc[0].bytes));
    SWITCH_REQUIRE(switch_cleared == 1 && switch_programmed == (variant == 1));
    return 0;
}

static uint32_t switch_test_allocation_failure(void)
{
    switch_reset();
    switch_allocate_error = -27;
    SWITCH_REQUIRE(switch_a3_ack_prepare(switch_wlc[0].bytes) == -27);
    SWITCH_REQUIRE(!switch_cleared && !switch_programmed && !switch_keys[0][4]);
    switch_allocate_error = 0;
    SWITCH_REQUIRE(!switch_a3_ack_prepare(switch_wlc[0].bytes) && switch_allocated == 2);
    return 0;
}

static uint32_t switch_test_no_slot(void)
{
    switch_reset();
    for (unsigned index = 4; index < 40; ++index)
        switch_keys[0][index] = (void *)1;
    SWITCH_REQUIRE(switch_a3_ack_prepare(switch_wlc[0].bytes) == -22);
    SWITCH_REQUIRE(!switch_fault && !switch_cleared && !switch_programmed);
    return 0;
}

static uint32_t switch_test_pointer_lost(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    switch_keys[0][4] = (void *)1;
    SWITCH_REQUIRE(switch_a3_ack_reconcile(switch_wlc[0].bytes) == -1);
    SWITCH_REQUIRE(!switch_fault && switch_cleared == 1 && switch_programmed == 1);
    SWITCH_REQUIRE(!switch_a3_ack_reconcile(switch_wlc[0].bytes) && switch_allocated == 1);
    return 0;
}

static uint32_t switch_test_array_changed(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    switch_other_keys[4] = switch_keys[0][4];
    *(void **)(switch_wlc[0].bytes + 0x2a0) = switch_other_keys;
    SWITCH_REQUIRE(switch_a3_ack_reconcile(switch_wlc[0].bytes) == -1);
    SWITCH_REQUIRE(!switch_fault && switch_cleared == 1 && switch_programmed == 1);
    return 0;
}

static uint32_t switch_test_different_wlc(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    void *old_key = switch_keys[0][4];
    *(void **)(switch_wlc[0].bytes + 0) = (void *)1;
    *(void **)(switch_wlc[0].bytes + 0x2a0) = (void *)1;
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[1].bytes, switch_address, 1));
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 2 && switch_keys[0][4] == old_key);
    SWITCH_REQUIRE(switch_receiver_wlc == switch_wlc[1].bytes);
    return 0;
}

static uint32_t switch_test_detached(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_prepare(switch_wlc[0].bytes));
    void *old_key = switch_keys[0][4];
    switch_a3_ack_detached();
    SWITCH_REQUIRE(!switch_a3_ack_prepare(switch_wlc[0].bytes));
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 2 && switch_keys[0][4] == old_key && switch_keys[0][5]);
    SWITCH_REQUIRE(!switch_cleared && !switch_programmed);
    return 0;
}

static uint32_t switch_test_bounds(void)
{
    static const uint32_t limits[][2] = {{4, 64}, {41, 64}, {40, 63}, {132, 64}};
    for (size_t index = 0; index < sizeof limits / sizeof limits[0]; ++index) {
        switch_reset();
        *(uint32_t *)(switch_pub[0].bytes + 0xbc) = limits[index][0];
        *(uint32_t *)(switch_pub[0].bytes + 0xc0) = limits[index][1];
        SWITCH_REQUIRE(switch_a3_ack_prepare(switch_wlc[0].bytes) == -1);
        SWITCH_REQUIRE(!switch_fault && !switch_allocated && !switch_cleared && !switch_programmed);
    }
    return 0;
}

static uint32_t switch_test_allocator_index(void)
{
    for (int32_t index = 3; index <= 40; index += 37) {
        switch_reset();
        switch_bad_index = index;
        SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1) == -1);
        SWITCH_REQUIRE(!switch_fault && !switch_cleared && !switch_programmed);
    }
    return 0;
}

static uint32_t switch_test_empty_object(void)
{
    switch_reset();
    switch_empty_object = 1;
    SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1) == -1);
    SWITCH_REQUIRE(!switch_fault && !switch_cleared && !switch_programmed);
    return 0;
}

static uint32_t switch_test_set_failure(void)
{
    switch_reset();
    switch_set_error = -11;
    SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1) == -11);
    switch_set_error = 0;
    SWITCH_REQUIRE(!switch_a3_ack_reconcile(switch_wlc[0].bytes));
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 1 && switch_cleared == 2 && switch_programmed == 2);
    return 0;
}

static uint32_t switch_test_invalid(void)
{
    static const uint8_t zero[6] = {0}, group[6] = {1, 2, 3, 4, 5, 6};
    switch_reset();
    SWITCH_REQUIRE(switch_a3_ack_prepare(NULL) == -2);
    SWITCH_REQUIRE(switch_a3_ack_reconcile(NULL) == -2);
    SWITCH_REQUIRE(switch_a3_ack_configure(NULL, switch_address, 1) == -2);
    SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, NULL, 1) == -2);
    SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, zero, 1) == -2);
    SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, group, 1) == -2);
    SWITCH_REQUIRE(!switch_fault && !switch_allocated && !switch_cleared && !switch_programmed);
    return 0;
}

static uint32_t switch_test_limit_changed(void)
{
    switch_reset();
    switch_keys[0][4] = (void *)1;
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    *(uint32_t *)(switch_pub[0].bytes + 0xbc) = 5;
    SWITCH_REQUIRE(switch_a3_ack_reconcile(switch_wlc[0].bytes) == -1);
    SWITCH_REQUIRE(!switch_fault && switch_cleared == 1 && switch_programmed == 1);
    return 0;
}

static uint32_t switch_test_disabled(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, NULL, 0));
    SWITCH_REQUIRE(!switch_a3_ack_reconcile(switch_wlc[0].bytes));
    SWITCH_REQUIRE(!switch_fault && !switch_allocated && !switch_cleared && !switch_programmed);
    return 0;
}

static uint32_t switch_test_mutated(unsigned variant)
{
    static const uint16_t offsets[] = {0, 6, 7, 8, 9, 10, 12, 16, 52, 200, 224};
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    uint8_t *key = switch_keys[0][4];
    if (offsets[variant] == 224)
        *(void **)(key + 224) = NULL;
    else
        key[offsets[variant]] ^= 1;
    SWITCH_REQUIRE(switch_a3_ack_reconcile(switch_wlc[0].bytes) == -1);
    SWITCH_REQUIRE(!switch_fault && switch_allocated == 1 && switch_cleared == 1 && switch_programmed == 1);
    return 0;
}

static uint32_t switch_test_network(void)
{
    static const uint8_t network[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    SWITCH_REQUIRE(switch_network.cleared_at == 1 && switch_network.info_at == 2 && switch_network.set_at == 3);
    SWITCH_REQUIRE(switch_network.info_index == 59 && switch_network.info == 4 && switch_network.set_index == 59);
    SWITCH_REQUIRE(switch_equal(switch_network.address, network) && !switch_network.transmitter);
    SWITCH_REQUIRE(!switch_a3_ack_reconcile(switch_wlc[0].bytes));
    SWITCH_REQUIRE(switch_network.cleared_at == 4 && switch_network.info_at == 5 && switch_network.set_at == 6);
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, NULL, 0));
    SWITCH_REQUIRE(switch_network.cleared_at == 7 && switch_network.sequence == 7);
    switch_wlc[0].bytes[25] = 0;
    SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1) == -11);
    SWITCH_REQUIRE(!switch_fault && switch_network.sequence == 7);

    switch_reset();
    switch_set_error = -11;
    SWITCH_REQUIRE(switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1) == -11);
    SWITCH_REQUIRE(switch_network.clears == 1 && switch_network.sequence == 1);
    switch_set_error = 0;
    switch_network.set_error = -11;
    SWITCH_REQUIRE(switch_a3_ack_reconcile(switch_wlc[0].bytes) == -11);
    SWITCH_REQUIRE(switch_network.set_at == 4 && switch_network.sequence == 4);
    SWITCH_REQUIRE(!switch_fault && switch_programmed == 2);
    return 0;
}

static uint32_t switch_test_disable(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_a3_ack_configure(switch_wlc[0].bytes, switch_address, 1));
    const uint32_t cleared = switch_cleared, programmed = switch_programmed, sequence = switch_network.sequence;
    switch_wlc[0].bytes[25] = 0;
    switch_a3_ack_disable();
    SWITCH_REQUIRE(!switch_fault && switch_cleared == cleared && switch_programmed == programmed &&
                   switch_network.sequence == sequence);
    switch_wlc[0].bytes[25] = 1;
    SWITCH_REQUIRE(!switch_a3_ack_reconcile(switch_wlc[0].bytes));
    SWITCH_REQUIRE(!switch_fault && switch_cleared == cleared + 1 && switch_programmed == programmed &&
                   switch_network.sequence == sequence + 1);
    return 0;
}

void switch_bssid_test_run(volatile uint32_t *results);

void switch_ack_test_run(void)
{
    switch_ack_test_results[0] = switch_test_prepare();
    switch_ack_test_results[1] = switch_test_enable();
    switch_ack_test_results[2] = switch_test_reopen();
    switch_ack_test_results[3] = switch_test_address_change();
    switch_ack_test_results[4] = switch_test_reconcile();
    for (unsigned index = 0; index < 3; ++index)
        switch_ack_test_results[5 + index] = switch_test_down(index);
    switch_ack_test_results[8] = switch_test_allocation_failure();
    switch_ack_test_results[9] = switch_test_no_slot();
    switch_ack_test_results[10] = switch_test_pointer_lost();
    switch_ack_test_results[11] = switch_test_array_changed();
    switch_ack_test_results[12] = switch_test_different_wlc();
    switch_ack_test_results[13] = switch_test_detached();
    switch_ack_test_results[14] = switch_test_bounds();
    switch_ack_test_results[15] = switch_test_allocator_index();
    switch_ack_test_results[16] = switch_test_empty_object();
    switch_ack_test_results[17] = switch_test_set_failure();
    switch_ack_test_results[18] = switch_test_invalid();
    switch_ack_test_results[19] = switch_test_limit_changed();
    switch_ack_test_results[20] = switch_test_disabled();
    for (unsigned index = 0; index < 11; ++index)
        switch_ack_test_results[21 + index] = switch_test_mutated(index);
    switch_ack_test_results[32] = switch_test_network();
    switch_bssid_test_run(switch_ack_test_results + 33);
    switch_ack_test_results[44] = switch_test_disable();
    switch_ack_test_done = 0x53574133;
}
