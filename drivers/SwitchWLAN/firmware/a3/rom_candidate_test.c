/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "rom.h"
#include "abi.h"
#include <stddef.h>
#include <stdint.h>

extern const uint32_t switch_a3_rom_guards[];
volatile uint32_t switch_rom_test_results[6], switch_rom_test_done;
volatile struct switch_a3_caps switch_a3_state;
extern int32_t switch_rom_test_install(const uint32_t entry[3], void *overflow);

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

static uint32_t switch_rejected(uint32_t address, uint32_t expected, uint32_t actual)
{
    SWITCH_REQUIRE(!switch_a3_rom_compatible());
#if DEVELOPMENT || DEBUG
    SWITCH_REQUIRE(switch_a3_state.reserved[0] == address && switch_a3_state.reserved[1] == expected &&
                   switch_a3_state.reserved[2] == actual);
#else
    (void)address;
    (void)expected;
    (void)actual;
    SWITCH_REQUIRE(!switch_a3_state.reserved[0] && !switch_a3_state.reserved[1] && !switch_a3_state.reserved[2]);
#endif
    return 0;
}

static uint32_t switch_flashpatch_views(void)
{
    volatile uint32_t *rom = (volatile uint32_t *)0x6a6b8;
    volatile uint32_t *slot = (volatile uint32_t *)0x181d78;
    volatile uint32_t *target = (volatile uint32_t *)0x1cfc88;
    rom[0] = 0x7983b5f8;
    rom[1] = 0x460e4605;
    rom[2] = 0x2b006804;
    rom[3] = 0x8081f000;
    slot[0] = 0xbae6f165;
    slot[1] = 0;
    target[0] = 0x7983b5f8;
    target[1] = 0x460e4605;
    target[2] = 0x2b006804;
    target[3] = 0x8887d078;
    SWITCH_REQUIRE(!switch_rejected(0x6a6b8, 0xbae6f165, 0x7983b5f8));
    slot[1] = rom[1];
    rom[0] = slot[0];
    SWITCH_REQUIRE(switch_a3_rom_compatible());
    rom[0] = slot[0] = 0xbae7f165;
    SWITCH_REQUIRE(!switch_rejected(0x6a6b8, 0xbae6f165, 0xbae7f165));
    rom[0] = slot[0] = 0xbae6f165;
    slot[0] ^= 1u;
    SWITCH_REQUIRE(!switch_rejected(0x181d78, 0xbae6f165, 0xbae6f164));
    slot[0] ^= 1u;
    slot[1] = 0;
    SWITCH_REQUIRE(!switch_rejected(0x181d7c, 0x460e4605, 0));
    slot[1] = 0x460e4605;
    rom[1] ^= 1u;
    SWITCH_REQUIRE(!switch_rejected(0x6a6bc, 0x460e4605, 0x460e4604));
    rom[1] ^= 1u;
    target[3] ^= 1u;
    SWITCH_REQUIRE(!switch_rejected(0x1cfc94, 0x8887d078, 0x8887d079));
    target[3] ^= 1u;
    volatile uint32_t *unrelated = (volatile uint32_t *)0x27674;
    *unrelated ^= 1u;
    SWITCH_REQUIRE(!switch_rejected(0x27674, 0x47f0e92d, 0x47f0e92c));
    *unrelated ^= 1u;
    SWITCH_REQUIRE(switch_a3_rom_compatible());
    return 0;
}

static uint32_t switch_installed_slot_views(void)
{
    volatile uint32_t *aligned_rom = (volatile uint32_t *)0x6a6b8;
    volatile uint32_t *aligned_slot = (volatile uint32_t *)0x181d78;
    volatile uint32_t *offset_rom = (volatile uint32_t *)0x44da8;
    volatile uint32_t *offset_slot = (volatile uint32_t *)0x181ae8;
    const uint32_t aligned_entry[3] = {0x6a6b8, 4, 0x181d78};
    const uint32_t offset_entry[3] = {0x44dac, 4, 0x181ae8};
    aligned_rom[0] = 0x7983b5f8;
    aligned_rom[1] = 0x460e4605;
    aligned_slot[0] = 0xbae6f165;
    aligned_slot[1] = 0;
    offset_rom[0] = 0xbd38ba09;
    offset_rom[1] = 0xf8d0b538;
    offset_rom[2] = 0x460430f0;
    offset_slot[0] = 0xbef2f162;
    offset_slot[1] = 0;
    SWITCH_REQUIRE(!switch_rom_test_install(aligned_entry, NULL));
    SWITCH_REQUIRE(aligned_slot[0] == 0xbae6f165 && aligned_slot[1] == 0x460e4605);
    SWITCH_REQUIRE(!switch_rom_test_install(offset_entry, NULL));
    SWITCH_REQUIRE(offset_slot[0] == 0xbd38ba09 && offset_slot[1] == 0xbef2f162);
    aligned_rom[0] = aligned_slot[0];
    offset_rom[1] = offset_slot[1];
    SWITCH_REQUIRE(switch_a3_rom_compatible());
    return 0;
}

void switch_rom_test_run(void)
{
    const uint32_t *guard = switch_a3_rom_guards;
    while (*guard) {
        const uint32_t address = *guard++;
        volatile uint32_t *rom = (volatile uint32_t *)(address & ~1u);
        for (uint32_t words = address & 1u ? 2u : 4u; words; --words)
            *rom++ = *guard++;
        ++switch_rom_test_results[2];
    }
    switch_rom_test_results[0] = switch_a3_rom_compatible() ? 0 : __LINE__;
    guard = switch_a3_rom_guards;
    while (*guard) {
        const uint32_t address = *guard++;
        volatile uint32_t *rom = (volatile uint32_t *)(address & ~1u);
        for (uint32_t words = address & 1u ? 2u : 4u; words; --words, ++rom, ++guard)
            for (uint32_t byte = 0; byte < 4; ++byte) {
                *rom ^= 1u << (byte * 8);
                if (switch_a3_rom_compatible())
                    switch_rom_test_results[1] = __LINE__;
#if DEVELOPMENT || DEBUG
                if (switch_a3_state.reserved[0] != (uint32_t)(uintptr_t)rom || switch_a3_state.reserved[1] != *guard ||
                    switch_a3_state.reserved[2] != *rom)
                    switch_rom_test_results[1] = __LINE__;
#else
                if (switch_a3_state.reserved[0] || switch_a3_state.reserved[1] || switch_a3_state.reserved[2])
                    switch_rom_test_results[1] = __LINE__;
#endif
                *rom = *guard;
                ++switch_rom_test_results[3];
            }
    }
    if (!switch_a3_rom_compatible())
        switch_rom_test_results[1] = __LINE__;
    switch_rom_test_results[4] = switch_flashpatch_views();
    switch_rom_test_results[5] = switch_installed_slot_views();
    switch_rom_test_done = 0x53574133;
}
