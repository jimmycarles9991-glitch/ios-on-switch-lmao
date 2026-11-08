#include "sdmmc_dma.h"
#include "sdmmc_read.h"
#include <string.h>

/* Register contract independently cross-checked with Hekate 45cd81ed and
 * Atmosphere 6b0c077b: HOST v4 SDMA uses 0x58/0x5c, not legacy sysad 0x00.
 * All protocol/error/partition/cache-off qualification stays with the owner. */
enum {
    BLOCK_SIZE = 4,
    BLOCK_COUNT = 6,
    TRANSFER = 0x0c,
    HOST_CONTROL = 0x28,
    POWER = 0x29,
    CLOCK = 0x2c,
    RESET = 0x2f,
    PRESENT = 0x24,
    STATUS = 0x30,
    HOST_CONTROL2 = 0x3e,
    CAPABILITIES = 0x40,
    DMA_LOW = 0x58,
    DMA_HIGH = 0x5c,
    VERSION = 0xfe,
    MAX_POLLS = 1000000
};

#define TRANSFER_DONE UINT32_C(2)
#define DMA_BOUNDARY  UINT32_C(8)
#define DMA_ERRORS    UINT32_C(0xffff8000)
#define BOUNDARY      UINT64_C(524288)

static int bad(struct switch_sd_dma *d, int error)
{
    d->failed = 1;
    return error;
}

static int mode(const struct switch_sd_io *io, void *c)
{
    uint32_t version = io->read16(c, VERSION) & 255;
    return (version == 2 || version == 3) && (io->read32(c, CAPABILITIES) & 0x10400000u) == 0x10400000u &&
           !(io->read8(c, HOST_CONTROL) & 0x18) && (io->read16(c, HOST_CONTROL2) & 0x3000) == 0x3000;
}

int switch_sd_dma_bind(struct switch_sd_dma *d, const struct switch_sd_io *io, void *c, uint64_t frequency)
{
    if (!d || !io || !io->read8 || !io->read16 || !io->read32 || !io->write16 || !io->write32 || !io->ticks ||
        !io->barrier || !d->write8 || !d->bus_ready || !d->cache || !d->bytes || ((uintptr_t)d->bytes & 63) ||
        !d->physical || (d->physical & 7) || d->capacity < SWITCH_SD_DMA_BYTES ||
        d->physical > UINT64_C(0x1000000000) - SWITCH_SD_DMA_BYTES || frequency < 1000 || frequency > 1000000000 ||
        d->bound || d->active || d->failed)
        return SWITCH_SD_ARGUMENT;
    uint32_t version = io->read16(c, VERSION) & 255;
    if ((version != 2 && version != 3) || (io->read32(c, CAPABILITIES) & 0x10400000u) != 0x10400000u || d->bus_ready(c))
        return bad(d, SWITCH_SD_UNQUALIFIED);
    uint32_t present = io->read32(c, PRESENT);
    if ((present & 0x307) || (present & 0x30000) != 0x30000 || io->read8(c, RESET) ||
        (io->read8(c, HOST_CONTROL) & 0x22) != 2 || !(io->read8(c, POWER) & 1) || (io->read16(c, CLOCK) & 7) != 7)
        return bad(d, SWITCH_SD_NOT_READY);
    /* Inherited bus width/high-speed/power/clock/tuning bits are preserved. */
    d->write8(c, HOST_CONTROL, (uint8_t)(io->read8(c, HOST_CONTROL) & ~0x18u));
    io->write16(c, HOST_CONTROL2, (uint16_t)(io->read16(c, HOST_CONTROL2) | 0x3000));
    io->barrier(c);
    (void)io->read16(c, CLOCK);
    if (!mode(io, c))
        return bad(d, SWITCH_SD_UNQUALIFIED);
    d->frequency = frequency;
    d->bound = 1;
    return SWITCH_SD_OK;
}

int switch_sd_dma_prepare(struct switch_sd_dma *d, const struct switch_sd_io *io, void *c, uint32_t size,
                          uint32_t count, const uint8_t *input)
{
    if (!d || !io || !d->bound || d->failed || d->active || !size || size > 512 || (size & 3) || !count ||
        count > 128 || count > SWITCH_SD_DMA_BYTES / size)
        return SWITCH_SD_ARGUMENT;
    if (!mode(io, c) || d->bus_ready(c))
        return bad(d, SWITCH_SD_UNQUALIFIED);
    d->length = size * count;
    d->blocks = count;
    d->writing = input != 0;
    d->completed = 0;
    d->polls = 0;
    d->boundaries = 0;
    if (input)
        memcpy(d->bytes, input, d->length);
    if (d->cache(c, d->bytes, d->length, 0))
        return bad(d, SWITCH_SD_UNQUALIFIED);
    d->end = d->physical + d->length;
    d->next_boundary = (d->physical + BOUNDARY) & ~(BOUNDARY - 1);
    d->started = io->ticks(c);
    d->active = 1;
    io->write32(c, DMA_LOW, (uint32_t)d->physical);
    io->write32(c, DMA_HIGH, (uint32_t)(d->physical >> 32));
    io->write16(c, BLOCK_SIZE, (uint16_t)(size | 0x7000));
    uint16_t transfer = io->read16(c, TRANSFER);
    /* SDHCI single-block transfer ignores block count; enable count only for
     * multiple blocks, as both qualified Tegra implementations do. */
    transfer = (uint16_t)((transfer & ~2u) | 1u | (count > 1 ? 2u : 0u));
    io->write16(c, TRANSFER, transfer);
    io->barrier(c);
    return SWITCH_SD_OK;
}

int switch_sd_dma_wait(struct switch_sd_dma *d, const struct switch_sd_io *io, void *c, struct switch_sd_result *r)
{
    if (!d || !io || !d->active || d->failed || d->completed || !r)
        return SWITCH_SD_ARGUMENT;
    for (;;) {
        r->status = io->read32(c, STATUS);
        if (r->status & DMA_ERRORS)
            return bad(d, SWITCH_SD_CONTROLLER_ERROR);
        if (++d->polls > MAX_POLLS || io->ticks(c) - d->started >= d->frequency * 5)
            return bad(d, SWITCH_SD_TIMEOUT);
        if (r->status & TRANSFER_DONE) {
            /* Only multiple-block transfers define a useful remaining count. */
            if (d->blocks > 1 && io->read16(c, BLOCK_COUNT))
                return bad(d, SWITCH_SD_BUFFER_STATE);
            io->write32(c, STATUS, TRANSFER_DONE | (r->status & DMA_BOUNDARY));
            io->barrier(c);
            d->completed = 1;
            return SWITCH_SD_OK;
        }
        if (r->status & DMA_BOUNDARY) {
            /* One <=64 KiB request crosses at most one 512 KiB boundary.
             * A final-boundary interrupt may precede TC with zero blocks left. */
            if (d->boundaries || d->next_boundary > d->end ||
                (d->next_boundary == d->end && (d->blocks == 1 || io->read16(c, BLOCK_COUNT))))
                return bad(d, SWITCH_SD_BUFFER_STATE);
            io->write32(c, STATUS, DMA_BOUNDARY);
            io->write32(c, DMA_LOW, (uint32_t)d->next_boundary);
            io->write32(c, DMA_HIGH, (uint32_t)(d->next_boundary >> 32));
            io->barrier(c);
            ++d->boundaries;
        }
    }
}

int switch_sd_dma_complete(struct switch_sd_dma *d, void *c, uint8_t *output)
{
    if (!d || !d->active || !d->completed || d->failed || (!d->writing && !output))
        return SWITCH_SD_ARGUMENT;
    if (!d->writing) {
        if (d->cache(c, d->bytes, d->length, 1))
            return bad(d, SWITCH_SD_UNQUALIFIED);
        memcpy(output, d->bytes, d->length);
        d->read_bytes += d->length;
    } else {
        d->write_bytes += d->length;
    }
    d->active = 0;
    d->completed = 0;
    return SWITCH_SD_OK;
}

void switch_sd_dma_abort(struct switch_sd_dma *d)
{
    if (d)
        d->failed = 1;
}
