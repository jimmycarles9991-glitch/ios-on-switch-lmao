// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVIDIA falcon microcontrollers behind host1x (VIC, NVDEC, NVENC, NVJPG): the v1 firmware image and the boot that
// DMAs its OS into IMEM/DMEM (Linux drivers/gpu/drm/tegra/falcon.c). Register offsets are from the engine's base:
// its falcon sits at +0x1000.
#ifndef SWITCH_FALCON_H
#define SWITCH_FALCON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWITCH_FALCON_IRQSTAT = 0x1008,
    SWITCH_FALCON_IRQMSET = 0x1010,
    SWITCH_FALCON_IRQDEST = 0x101c,
    SWITCH_FALCON_MAILBOX0 = 0x1040,
    SWITCH_FALCON_MAILBOX1 = 0x1044,
    SWITCH_FALCON_ITFEN = 0x1048,
    SWITCH_FALCON_IDLESTATE = 0x104c,
    SWITCH_FALCON_OS = 0x1080,
    SWITCH_FALCON_DEBUGINFO = 0x1094,
    SWITCH_FALCON_EXCI = 0x10d0,
    SWITCH_FALCON_CPUCTL = 0x1100,
    SWITCH_FALCON_BOOTVEC = 0x1104,
    SWITCH_FALCON_DMACTL = 0x110c,
    SWITCH_FALCON_DMATRFBASE = 0x1110,
    SWITCH_FALCON_DMATRFMOFFS = 0x1114,
    SWITCH_FALCON_DMATRFCMD = 0x1118,
    SWITCH_FALCON_DMATRFFBOFFS = 0x111c,
    // Debug state (nvgpu hw_falcon_gm20b.h): the trace buffer, the external-error capture, the in-circuit debugger.
    SWITCH_FALCON_TRACEIDX = 0x1148,
    SWITCH_FALCON_TRACEPC = 0x114c,
    SWITCH_FALCON_EXTERRADDR = 0x1168,
    SWITCH_FALCON_EXTERRSTAT = 0x116c,
    // DMEM port 0 (nouveau falcon/gm200.c): DMEMC takes a 4-aligned byte address, DMEMD then reads that word.
    SWITCH_FALCON_DMEMC = 0x11c0,
    SWITCH_FALCON_DMEMD = 0x11c4,
    SWITCH_FALCON_ICD_CMD = 0x1200,
    SWITCH_FALCON_ICD_RDATA = 0x120c,
};

// falcon_parse_firmware_image's fields. fce_header/fce_data are VIC's FCE microcode words of the same header; the
// other engines leave them unused.
typedef struct switch_falcon_image {
    uint32_t size, bin_data, bin_size;
    uint32_t code, code_size, data, data_size, apps;
    uint32_t fce_header, fce_data;
} switch_falcon_image;

static inline uint32_t switch_falcon_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Magic 0x10de (or 0x10fe), version 1, the OS header and the code and data it names inside the image.
static inline bool switch_falcon_parse(const uint8_t *fw, size_t length, switch_falcon_image *image)
{
    if (length < 0x100)
        return false;
    const uint32_t magic = switch_falcon_le32(fw);
    if ((magic != 0x10de && magic != 0x10fe) || switch_falcon_le32(fw + 4) != 1)
        return false;
    image->size = switch_falcon_le32(fw + 8);
    const uint32_t os = switch_falcon_le32(fw + 12);
    image->bin_data = switch_falcon_le32(fw + 16);
    image->bin_size = switch_falcon_le32(fw + 20);
    image->fce_header = switch_falcon_le32(fw + 24);
    image->fce_data = switch_falcon_le32(fw + 28);
    if (image->size > length || os > image->size || image->size - os < 20 || image->bin_data > image->size ||
        image->bin_size > image->size - image->bin_data)
        return false;
    image->code = switch_falcon_le32(fw + os);
    image->code_size = switch_falcon_le32(fw + os + 4);
    image->data = switch_falcon_le32(fw + os + 8);
    image->data_size = switch_falcon_le32(fw + os + 12);
    image->apps = switch_falcon_le32(fw + os + 16);
    return image->code_size != 0 && image->code <= image->bin_size &&
           image->code_size <= image->bin_size - image->code && image->data <= image->bin_size &&
           image->data_size <= image->bin_size - image->data;
}

typedef enum switch_falcon_result {
    SWITCH_FALCON_OK,
    SWITCH_FALCON_SCRUBBING,
    SWITCH_FALCON_DMA,
} switch_falcon_result;

// After the engine's reset: waits for IMEM/DMEM scrubbing, then DMAs the OS's data and code from the image, whose
// bin_data section the falcon reads at firmware_pa + bin_data (below 4 GiB, cleaned to DRAM). The caller logs a
// failure with the register it names (DMACTL, DMATRFCMD).
switch_falcon_result switch_falcon_load(volatile uint8_t *engine, uint64_t firmware_pa,
                                        const switch_falcon_image *image);
// falcon_boot minus its final idle wait: the caller proves the OS runs by its first method's completion.
void switch_falcon_start(volatile uint8_t *engine);
// One line of falcon state for a failure report; returns text.
const char *switch_falcon_state(const volatile uint8_t *engine, char *text, size_t size);

// A falcon that stopped finishing work, for its one line (nvgpu gk20a_falcon_dump_stats): CPUCTL (bit 4 halted, bit 5
// stopped), IDLESTATE (bit 0 the falcon, bits 1.. engine units busy), interrupts, mailboxes, the exception register,
// the DMA queue and external-error capture, the newest trace-buffer branch targets, and PC/SP read three times through
// the in-circuit debugger (one PC three times: a tight wait or a stall). Only for an engine whose job is being cancelled:
// the debugger reads disturb a running falcon.
const char *switch_falcon_stall(volatile uint8_t *engine, char *text, size_t size);

#ifdef __cplusplus
}
#endif

#endif
