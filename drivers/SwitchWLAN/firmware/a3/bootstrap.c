/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "abi.h"
#include "control.h"
#include "wire.h"
#include "rom.h"

struct switch_a3_iovar {
    const char *name;
    uint16_t id, flags, type, min_length;
};

_Static_assert(sizeof(struct switch_a3_iovar) == 12, "A3 iovar record");

extern const uint8_t switch_a3_island_start[], switch_a3_island_end[];

volatile struct switch_a3_caps switch_a3_state = {
    .magic = SWITCH_A3_CAPS_MAGIC,
    .version = SWITCH_A3_CAPS_VERSION,
    .size = sizeof(struct switch_a3_caps),
    .island_begin = (uint32_t)switch_a3_island_start,
    .island_end = (uint32_t)switch_a3_island_end,
};

static const struct switch_a3_iovar switch_a3_iovars[] = {
    {SWITCH_A3_CAPS_IOVAR, 0, 0, 8, sizeof(struct switch_a3_caps)},
    {SWITCH_A3_CONTROL_IOVAR, 1, 0, 8, sizeof(struct switch_a3_control)},
#if (DEVELOPMENT || DEBUG) && !SWITCH_A3_CANDIDATE
    {SWITCH_A3_DEADLINE_IOVAR, 2, 0, 8, sizeof(struct switch_a3_deadline_snapshot)},
#endif
    {0},
};

int32_t switch_a3_iovar(void *context, const struct switch_a3_iovar *record, uint32_t action, const char *name,
                        const void *input, uint32_t input_length, void *output, uint32_t output_length,
                        uint32_t scalar_size, void *interface)
{
    (void)record;
    (void)name;
    (void)scalar_size;
    (void)interface;
    if (action & 1u)
        return -23;
    if (context != (const void *)&switch_a3_state || !output)
        return -2;
    if (action == 2)
        return switch_a3_control_request(input, input_length, output, output_length);
#if (DEVELOPMENT || DEBUG) && !SWITCH_A3_CANDIDATE
    if (action == 4)
        return switch_a3_control_deadline(output, output_length);
#endif
    if (action)
        return -23;
    if (output_length < sizeof switch_a3_state)
        return -14;
    const volatile uint8_t *source = (const volatile uint8_t *)&switch_a3_state;
    volatile uint8_t *destination = output;
    for (size_t i = 0; i < sizeof switch_a3_state; ++i)
        destination[i] = source[i];
    return 0;
}

int32_t switch_a3_up(void *context)
{
    if (context == (const void *)&switch_a3_state)
        switch_a3_control_up();
    return 0;
}

uint32_t switch_a3_down(void *context)
{
    if (context == (const void *)&switch_a3_state)
        switch_a3_control_down();
    return 0;
}

void switch_a3_initialize(void *wlc)
{
    if (!wlc || switch_a3_state.status != SWITCH_A3_HEAP_READY)
        return;
    typedef int32_t (*switch_a3_register_fn)(void *, const struct switch_a3_iovar *, const char *, void *, void *,
                                             void *, void *, void *, const struct switch_a3_iovar *, void *);
    const switch_a3_register_fn switch_register = (switch_a3_register_fn)0x27675;
    switch_a3_state.wlc = (uint32_t)wlc;
    switch_a3_state.context = (uint32_t)&switch_a3_state;
    if (!switch_a3_rom_compatible()) {
        switch_a3_state.status = SWITCH_A3_ROM_MISMATCH;
        return;
    }
    switch_a3_control_initialize(wlc);
    switch_a3_state.features = switch_a3_control_features();
    const int32_t result =
        switch_register(*(void **)wlc, switch_a3_iovars, SWITCH_A3_MODULE_NAME, (void *)&switch_a3_state,
                        (void *)switch_a3_iovar, 0, (void *)switch_a3_up, (void *)switch_a3_down, 0, 0);
    switch_a3_state.register_status = result;
    switch_a3_state.status = result ? SWITCH_A3_REGISTER_FAILED : SWITCH_A3_ATTACHED;
    switch_a3_state.initialized = !result && switch_a3_state.features != 0;
}
