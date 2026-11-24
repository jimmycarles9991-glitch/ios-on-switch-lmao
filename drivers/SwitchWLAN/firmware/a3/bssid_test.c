/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>
#include <stdint.h>

extern int switch_bssid_native_attach(void *, void *);
extern void switch_bssid_native_detach(void *, void *);
extern int switch_bssid_native_set(void *, void *);
extern void switch_bssid_native_clear(void *, void *);
extern void switch_bssid_native_dispatch(void *);
extern int switch_bssid_native_receiver(void *, unsigned, const uint8_t *);
extern int switch_bssid_native_network(void *, unsigned, const uint8_t *, unsigned);

struct switch_bssid_slot {
    uint8_t address[6];
    uint16_t flags, metadata;
};

static struct {
    _Alignas(4) uint8_t wlc[0x280], pub[0x120], module[44], primary[952], secondary[952], information[48], object[36], key[64];
    struct switch_bssid_slot slots[64];
    unsigned allocations, frees, writes, invalid;
} switch_bssid_fixture;

#define SWITCH_REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (0)
enum { SWITCH_CUBBY = 944, SWITCH_RECEIVER = 9, SWITCH_KEY_SLOT = 14, SWITCH_BSSID = 41, SWITCH_PRIMARY = 62, SWITCH_CASES = 11 };

static void switch_bssid_pointer(uint8_t *base, unsigned offset, void *value)
{
    *(void **)(base + offset) = value;
}

static void switch_bssid_initialize(void)
{
    for (size_t n = 0; n < sizeof switch_bssid_fixture; ++n)
        ((uint8_t *)&switch_bssid_fixture)[n] = 0;
    uint8_t *wlc = switch_bssid_fixture.wlc, *pub = switch_bssid_fixture.pub, *module = switch_bssid_fixture.module;
    uint8_t *primary = switch_bssid_fixture.primary, *secondary = switch_bssid_fixture.secondary;
    switch_bssid_pointer(wlc, 0, pub);
    switch_bssid_pointer(wlc, 16, switch_bssid_fixture.information);
    switch_bssid_pointer(wlc, 0x148, module);
    switch_bssid_pointer(wlc, 0x26c, primary);
    switch_bssid_pointer(module, 0, wlc);
    switch_bssid_pointer(primary, 0, wlc);
    switch_bssid_pointer(secondary, 0, wlc);
    switch_bssid_pointer(primary, 272, switch_bssid_fixture.information);
    switch_bssid_pointer(secondary, 272, switch_bssid_fixture.information);
    switch_bssid_pointer(secondary, 304, switch_bssid_fixture.key);
    switch_bssid_fixture.key[6] = SWITCH_KEY_SLOT + 4;
    pub[24] = primary[16] = 1;
    secondary[68] = 7;
    primary[216] = 0x12;
    secondary[216] = 0x02;
    secondary[221] = 0xef;
    wlc[25] = pub[58] = 1;
    *(uint32_t *)(pub + 192) = 64;
    *(uint32_t *)(module + 4) = SWITCH_CUBBY;
    *(uint32_t *)(module + 36) = 40;
    *(uint32_t *)(module + 40) = 52;
    module[11] = module[12] = module[13] = 255;
}

void *switch_bssid_alloc(unsigned bytes)
{
    if (bytes != 36 || switch_bssid_fixture.allocations != switch_bssid_fixture.frees)
        ++switch_bssid_fixture.invalid;
    ++switch_bssid_fixture.allocations;
    return switch_bssid_fixture.object;
}

void switch_bssid_free(void *object)
{
    if (object != switch_bssid_fixture.object || switch_bssid_fixture.allocations <= switch_bssid_fixture.frees)
        ++switch_bssid_fixture.invalid;
    ++switch_bssid_fixture.frees;
}

void switch_bssid_write(void *hardware, unsigned index, const uint8_t *address, unsigned flags)
{
    if (hardware != switch_bssid_fixture.information || index >= 64) {
        ++switch_bssid_fixture.invalid;
        return;
    }
    struct switch_bssid_slot *slot = &switch_bssid_fixture.slots[index];
    for (unsigned i = 0; i < sizeof slot->address; ++i)
        slot->address[i] = address[i];
    slot->flags = flags;
    ++switch_bssid_fixture.writes;
}

unsigned switch_bssid_metadata(void *wlc, unsigned index)
{
    if (wlc != switch_bssid_fixture.wlc || index >= 64) {
        ++switch_bssid_fixture.invalid;
        return 0;
    }
    return switch_bssid_fixture.slots[index].metadata;
}

void switch_bssid_set_metadata(void *wlc, unsigned index, unsigned value)
{
    if (wlc != switch_bssid_fixture.wlc || index >= 64)
        ++switch_bssid_fixture.invalid;
    else
        switch_bssid_fixture.slots[index].metadata = value;
}

static uint32_t switch_bssid_qualify(unsigned variant)
{
    switch_bssid_initialize();
    uint8_t *wlc = switch_bssid_fixture.wlc, *pub = switch_bssid_fixture.pub, *module = switch_bssid_fixture.module;
    uint8_t *secondary = switch_bssid_fixture.secondary, *primary = switch_bssid_fixture.primary;
    uint8_t *cfg = variant == 2 ? primary : secondary;
    if (variant == 0 || variant == 1)
        pub[58] = 0;
    if (variant == 3) {
        module[15] = 0x0f;
        module[14] = 0xfe;
    }
    if (variant == 4)
        module[11] = module[12] = module[13] = 0;
    if (variant == 5)
        secondary[6] = 1;
    int result = switch_bssid_native_attach(module, cfg);
    uint8_t *object = *(uint8_t **)(cfg + SWITCH_CUBBY);
    if (variant <= 1) {
        SWITCH_REQUIRE(!result && !object && !switch_bssid_fixture.allocations);
        secondary[16] = variant;
        switch_bssid_native_dispatch(secondary);
        SWITCH_REQUIRE(switch_bssid_fixture.writes == variant);
        SWITCH_REQUIRE(switch_bssid_fixture.slots[SWITCH_KEY_SLOT].flags == (variant ? 0x8004 : 0));
        SWITCH_REQUIRE(!switch_bssid_fixture.slots[SWITCH_BSSID].flags);
    } else if (variant == 3 || variant == 4) {
        SWITCH_REQUIRE(result == -22 && !object && switch_bssid_fixture.frees == 1);
        if (variant == 3)
            SWITCH_REQUIRE(module[14] == 0xfe && module[15] == 0x0f && module[11] == 255);
    } else {
        SWITCH_REQUIRE(!result && object == switch_bssid_fixture.object && module[11] == cfg[68]);
        if (variant == 2) {
            SWITCH_REQUIRE(object[4] == 64 && object[5] == 64 && module[14] == 0);
            switch_bssid_native_dispatch(primary);
            SWITCH_REQUIRE(switch_bssid_fixture.slots[SWITCH_PRIMARY].address[0] == 0x12);
        } else {
            const unsigned bssid = variant == 5 ? 40 : SWITCH_BSSID;
            struct switch_bssid_slot *slot = &switch_bssid_fixture.slots[bssid];
            SWITCH_REQUIRE(object[4] == 40 && object[5] == bssid && object[6] == 1);
            switch_bssid_native_dispatch(primary);
            switch_bssid_native_receiver(wlc, SWITCH_RECEIVER, secondary + 216);
            slot->metadata = 0x8000;
            if (variant == 6)
                wlc[25] = 0;
            if (variant == 7) {
                switch_bssid_native_dispatch(secondary);
                SWITCH_REQUIRE(!slot->flags && switch_bssid_fixture.writes == 2);
                secondary[16] = 1;
                switch_bssid_native_dispatch(secondary);
                SWITCH_REQUIRE(!slot->flags && switch_bssid_fixture.slots[SWITCH_KEY_SLOT].flags == 0x8004);
            }
            SWITCH_REQUIRE(!switch_bssid_native_set(module, secondary));
            if (variant == 6) {
                SWITCH_REQUIRE(switch_bssid_fixture.writes == 2 && !slot->flags && slot->metadata == 0x8000);
                wlc[25] = 1;
            } else {
                SWITCH_REQUIRE(slot->address[0] == 2 && slot->address[5] == 0xef);
                SWITCH_REQUIRE(slot->flags == (variant == 5 ? 0x8002 : 0x8006));
                SWITCH_REQUIRE(slot->metadata == (variant == 5 ? 0x8004 : 0x8106));
            }
            if (variant == 8) {
                secondary[221] = 0xaa;
                switch_bssid_native_clear(module, secondary);
                switch_bssid_native_set(module, secondary);
                SWITCH_REQUIRE(slot->address[5] == 0xaa);
            }
            switch_bssid_native_clear(module, secondary);
            SWITCH_REQUIRE(!slot->address[0] && !slot->address[5] && !slot->flags && !(slot->metadata & 4));
            SWITCH_REQUIRE(switch_bssid_fixture.slots[SWITCH_PRIMARY].address[0] == 0x12 &&
                           switch_bssid_fixture.slots[SWITCH_PRIMARY].flags == 0x800e &&
                           switch_bssid_fixture.slots[SWITCH_PRIMARY].metadata == 4 &&
                           switch_bssid_fixture.slots[SWITCH_RECEIVER].flags == 0x8008);
        }
        switch_bssid_native_detach(module, cfg);
        SWITCH_REQUIRE(!*(void **)(cfg + SWITCH_CUBBY) && module[11] == 255 && !module[14] &&
                       switch_bssid_fixture.allocations == switch_bssid_fixture.frees);
        if (variant == 9) {
            SWITCH_REQUIRE(!switch_bssid_native_attach(module, cfg) && module[14] == 3);
            switch_bssid_native_detach(module, cfg);
            SWITCH_REQUIRE(!module[14] && switch_bssid_fixture.frees == 2);
        }
    }
    SWITCH_REQUIRE(!switch_bssid_fixture.invalid);
    return 0;
}

static uint32_t switch_bssid_network(void)
{
    static const uint8_t network[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
    const unsigned index = 59;
    uint8_t *wlc = switch_bssid_fixture.wlc;
    struct switch_bssid_slot *slot = &switch_bssid_fixture.slots[index];
    for (unsigned transmitter = 0; transmitter < 2; ++transmitter) {
        switch_bssid_initialize();
        SWITCH_REQUIRE(!switch_bssid_native_network(wlc, index, network, transmitter));
        for (unsigned i = 0; i < sizeof network; ++i)
            SWITCH_REQUIRE(slot->address[i] == network[i]);
        SWITCH_REQUIRE(slot->flags == (transmitter ? 0x8006 : 0x8002) && switch_bssid_fixture.writes == 1);
        SWITCH_REQUIRE(!switch_bssid_fixture.invalid);
    }
    switch_bssid_initialize();
    wlc[25] = 0;
    SWITCH_REQUIRE(switch_bssid_native_network(wlc, index, network, 0) == -11);
    SWITCH_REQUIRE(!switch_bssid_fixture.writes && !slot->flags && !switch_bssid_fixture.invalid);
    return 0;
}

void switch_bssid_test_run(volatile uint32_t *results)
{
    for (unsigned variant = 0; variant + 1 < SWITCH_CASES; ++variant)
        results[variant] = switch_bssid_qualify(variant);
    results[SWITCH_CASES - 1] = switch_bssid_network();
}
