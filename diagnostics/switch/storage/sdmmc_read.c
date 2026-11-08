#include "sdmmc_read.h"
#include "sdmmc_dma.h"

/* SDHCI register layout cross-checked against the pinned Switch U-Boot
 * tegra_mmc.h/tegra_mmc.c. Native data phases use qualified SDMA64; PIO
 * remains an inert reference fixture. No PHY, power, clock, tuning or reset writes. */
enum {
    BLOCK_SIZE = 0x04,
    BLOCK_COUNT = 0x06,
    ARGUMENT = 0x08,
    TRANSFER = 0x0c,
    COMMAND = 0x0e,
    RESPONSE = 0x10,
    STOP_RESPONSE = 0x1c,
    BUFFER = 0x20,
    PRESENT = 0x24,
    HOST_CONTROL = 0x28,
    POWER = 0x29,
    CLOCK = 0x2c,
    RESET = 0x2f,
    STATUS = 0x30,
    STATUS_ENABLE = 0x34,
    SIGNAL_ENABLE = 0x38,
    AUTO_CMD_ERROR = 0x3c,
    VERSION = 0xfe,
    COMMAND_DONE = 1,
    TRANSFER_DONE = 2,
    BUFFER_READY = 0x20,
    ERRORS = 0xffff8000u,
    OWN_EVENTS = 0x23,
    ENABLE_EVENTS = 0x03ff8023,
    /* R1: error bits only; READY_FOR_DATA/current state are not errors. */
    CARD_ERRORS = 0xfdffe008u,
    MAX_POLLS = 1000000
};

struct batch_budget {
    uint64_t start;
    uint32_t polls;
};

static int batch_expired(struct switch_sd_host *h, struct batch_budget *b)
{
    return ++b->polls > MAX_POLLS || h->io.ticks(h->context) - b->start >= h->frequency * 5;
}

static int wait_event(struct switch_sd_host *h, uint32_t event, struct switch_sd_result *r, struct batch_budget *batch,
                      int final_buffer)
{
    uint64_t started = h->io.ticks(h->context);
    for (uint32_t polls = 0; polls < MAX_POLLS; ++polls) {
        r->status = h->io.read32(h->context, STATUS);
        if (r->status & ERRORS)
            return SWITCH_SD_CONTROLLER_ERROR;
        if (batch && batch_expired(h, batch))
            return SWITCH_SD_TIMEOUT;
        if (batch && event == BUFFER_READY && (r->status & TRANSFER_DONE) &&
            !(final_buffer && (r->status & BUFFER_READY)))
            return SWITCH_SD_BUFFER_STATE; /* Short or non-final completion. */
        if (r->status & event)
            return SWITCH_SD_OK;
        /* One second, bounded also when a broken counter does not advance. */
        if (h->io.ticks(h->context) - started >= h->frequency)
            return SWITCH_SD_TIMEOUT;
    }
    return SWITCH_SD_TIMEOUT;
}

static int read_transfer(struct switch_sd_host *h, uint64_t lba, uint32_t count, uint8_t *output,
                         struct switch_sd_result *r)
{
    if (!h || !r || !output || !h->io.read8 || !h->io.read16 || !h->io.read32 || !h->io.write16 || !h->io.write32 ||
        !h->io.ticks || !h->io.barrier || !count || count > SWITCH_SD_MAX_READ_SECTORS)
        return SWITCH_SD_ARGUMENT;
    *r = (struct switch_sd_result){0};
    if (h->blocked)
        return SWITCH_SD_ALREADY_BLOCKED;
    if (h->dma_required && (!h->dma || !h->dma->bound || h->dma->failed))
        return SWITCH_SD_UNQUALIFIED;
    if (h->block_addressing_verified > 1 ||
        (!h->block_addressing_verified &&
         (h->permitted_first_sector || h->permitted_sector_count != 1 || h->capacity_sectors != 1)) ||
        h->frequency < 1000 || h->frequency > 1000000000 || !h->capacity_sectors ||
        h->capacity_sectors > UINT64_C(0x100000000) || h->permitted_first_sector >= h->capacity_sectors ||
        !h->permitted_sector_count || h->permitted_sector_count > h->capacity_sectors - h->permitted_first_sector)
        return SWITCH_SD_UNQUALIFIED;
    if (lba < h->permitted_first_sector || lba - h->permitted_first_sector >= h->permitted_sector_count ||
        count > h->permitted_sector_count - (lba - h->permitted_first_sector))
        return SWITCH_SD_ARGUMENT;
    r->stage = 1;
    /* Sample every readiness register exactly once. A later diagnostic read
     * may differ from the value which caused a refusal. Preserve all samples
     * and all failed predicates, including when an earlier predicate fails. */
    r->present = h->io.read32(h->context, PRESENT);
    r->version = h->io.read16(h->context, VERSION);
    r->host_control = h->io.read8(h->context, HOST_CONTROL);
    r->power = h->io.read8(h->context, POWER);
    r->clock = h->io.read16(h->context, CLOCK);
    r->reset = h->io.read8(h->context, RESET);
    r->block_size = h->io.read16(h->context, BLOCK_SIZE);
    /* SDHCI 3.0/4.0 support bounded PIO with nonzero 16-bit block count;
     * Auto CMD12 is used only for multiple blocks. Accept these revisions,
     * inserted/stable card, idle command/data lines and inherited 4-bit bus. */
    uint32_t spec_version = r->version & 0xff;
    if (spec_version != 2 && spec_version != 3)
        r->readiness_failures |= SWITCH_SD_READY_VERSION;
    if (r->present & 0x307)
        r->readiness_failures |= SWITCH_SD_READY_ACTIVE;
    if ((r->present & 0x30000) != 0x30000)
        r->readiness_failures |= SWITCH_SD_READY_CARD;
    if ((r->host_control & 0x22) != 2)
        r->readiness_failures |= SWITCH_SD_READY_BUS;
    if (!(r->power & 1))
        r->readiness_failures |= SWITCH_SD_READY_POWER;
    if ((r->clock & 7) != 7)
        r->readiness_failures |= SWITCH_SD_READY_CLOCK;
    if (r->reset)
        r->readiness_failures |= SWITCH_SD_READY_RESET;
    if ((r->block_size & 0xfff) != 512)
        r->readiness_failures |= SWITCH_SD_READY_BLOCK_SIZE;
    if (r->readiness_failures)
        return SWITCH_SD_NOT_READY;
    r->status = h->io.read32(h->context, STATUS);
    r->initial_status = r->status;
    if (r->status & ERRORS)
        return SWITCH_SD_PREEXISTING_ERROR;
    uint32_t prior_enable = h->io.read32(h->context, STATUS_ENABLE);
    uint32_t prior_signal = h->io.read32(h->context, SIGNAL_ENABLE);
    uint16_t prior_size = (uint16_t)r->block_size;
    uint16_t prior_count = h->io.read16(h->context, BLOCK_COUNT);
    uint16_t prior_transfer = h->io.read16(h->context, TRANSFER);
    uint32_t prior_argument = h->io.read32(h->context, ARGUMENT);
    struct batch_budget budget = {0};
    struct batch_budget *batch = count > 1 || h->dma ? &budget : 0;
    if (batch)
        budget.start = h->io.ticks(h->context);
    h->io.write32(h->context, SIGNAL_ENABLE, 0);
    h->io.write32(h->context, STATUS_ENABLE, prior_enable | ENABLE_EVENTS | (h->dma ? 8u : 0u));
    h->io.write32(h->context, STATUS, OWN_EVENTS | (h->dma ? 8u : 0u));
    h->io.write16(h->context, BLOCK_SIZE, 512);
    h->io.write16(h->context, BLOCK_COUNT, (uint16_t)count);
    h->io.write32(h->context, ARGUMENT, (uint32_t)lba);
    /* SDHCI 4.20 sections 1.11/2.2.5/3.7.1: bounded multiple read with
     * Auto CMD12. Hekate uses the same stop mode on the Tegra controller.
     * The DMA adapter adds only its transfer bit/address/boundary controls. */
    h->io.write16(h->context, TRANSFER, count > 1 ? 0x36 : 0x12);
    int error = SWITCH_SD_OK;
    if (h->dma) {
        error = switch_sd_dma_prepare(h->dma, &h->io, h->context, 512, count, 0);
        if (error)
            goto failed;
    }
    h->io.barrier(h->context);
    /* CMD17/CMD18, 48-bit R1, index+CRC checked, data present. */
    h->io.write16(h->context, COMMAND, count > 1 ? 0x123a : 0x113a);
    r->stage = 2;
    error = wait_event(h, COMMAND_DONE, r, batch, 0);
    if (error)
        goto failed;
    r->response = h->io.read32(h->context, RESPONSE);
    if (r->response & CARD_ERRORS) {
        error = SWITCH_SD_CARD_ERROR;
        goto failed;
    }
    h->io.write32(h->context, STATUS, COMMAND_DONE);
    r->stage = 3;
    if (h->dma) {
        error = switch_sd_dma_wait(h->dma, &h->io, h->context, r);
        if (error)
            goto failed;
    } else {
        for (uint32_t block = 0; block < count; block++) {
            /* Some hosts coassert ready and done for the final buffered block.
         * Drain it only when present-state also confirms readable data. */
            error = wait_event(h, BUFFER_READY, r, batch, block + 1 == count);
            if (error)
                goto failed;
            if (!(h->io.read32(h->context, PRESENT) & 0x800)) {
                error = SWITCH_SD_BUFFER_STATE;
                goto failed;
            }
            /* Clear before draining: the next block may become ready as the last
         * FIFO word is read. Clearing afterwards would lose that new event. */
            if (batch)
                h->io.write32(h->context, STATUS, BUFFER_READY);
            for (uint32_t i = 0; i < 128; ++i) {
                uint32_t word = h->io.read32(h->context, BUFFER);
                uint32_t at = block * 512 + i * 4;
                for (uint32_t j = 0; j < 4; ++j)
                    output[at + j] = (uint8_t)(word >> (j * 8));
                r->words_read++;
            }
            if (!batch)
                h->io.write32(h->context, STATUS, BUFFER_READY);
        }
    }
    r->stage = 4;
    if (!h->dma) {
        error = wait_event(h, TRANSFER_DONE, r, batch, 0);
        if (error)
            goto failed;
        h->io.write32(h->context, STATUS, TRANSFER_DONE);
    }
    h->io.barrier(h->context);
    if (batch) {
        /* Auto CMD12 has an R1b response. Like Hekate, wait for DAT0 high
         * after data completion, and also require command/data idle. */
        uint64_t started = h->io.ticks(h->context);
        for (;;) {
            r->status = h->io.read32(h->context, STATUS);
            if (r->status & ERRORS) {
                error = SWITCH_SD_CONTROLLER_ERROR;
                goto failed;
            }
            if (batch_expired(h, batch) || h->io.ticks(h->context) - started >= h->frequency * 2) {
                error = SWITCH_SD_TIMEOUT;
                goto failed;
            }
            uint32_t present = h->io.read32(h->context, PRESENT);
            if (!(present & 0x307) && (present & 0x100000))
                break;
        }
        if (count > 1) {
            r->auto_cmd_error = h->io.read16(h->context, AUTO_CMD_ERROR);
            r->stop_response = h->io.read32(h->context, STOP_RESPONSE);
            if (r->auto_cmd_error & 0x9f) {
                error = SWITCH_SD_CONTROLLER_ERROR;
                goto failed;
            }
            if (r->stop_response & CARD_ERRORS) {
                error = SWITCH_SD_CARD_ERROR;
                goto failed;
            }
        }
    } else if (h->io.read32(h->context, PRESENT) & 0x307) {
        error = SWITCH_SD_BUFFER_STATE;
        goto failed;
    }
    if (h->dma) {
        error = switch_sd_dma_complete(h->dma, h->context, output);
        if (error)
            goto failed;
        r->words_read = count * 128;
    }
    h->io.write16(h->context, BLOCK_SIZE, prior_size);
    h->io.write16(h->context, BLOCK_COUNT, prior_count);
    h->io.write16(h->context, TRANSFER, prior_transfer);
    h->io.write32(h->context, ARGUMENT, prior_argument);
    h->io.write32(h->context, STATUS_ENABLE, prior_enable);
    h->io.write32(h->context, SIGNAL_ENABLE, prior_signal);
    h->io.barrier(h->context);
    r->stage = 5;
    return SWITCH_SD_OK;
failed:
    h->blocked = 1;
    h->write_cache_off_verified = 0;
    if (h->dma)
        switch_sd_dma_abort(h->dma);
    h->io.write32(h->context, SIGNAL_ENABLE, 0);
    h->io.barrier(h->context);
    /* Keep signals masked. In-flight hardware is not silently returned to a
     * previous owner; caller must stop storage use and report this failure. */
    return error;
}

int switch_sd_read_sector(struct switch_sd_host *h, uint64_t lba, uint8_t output[512], struct switch_sd_result *r)
{
    if (!output)
        return SWITCH_SD_ARGUMENT;
    uint8_t sector[512];
    int error = read_transfer(h, lba, 1, sector, r);
    if (!error) {
        for (uint32_t i = 0; i < 512; i++)
            output[i] = sector[i];
    }
    return error;
}

int switch_sd_read_sectors(struct switch_sd_host *h, uint64_t lba, uint32_t count, uint8_t *private_output,
                           struct switch_sd_result *r)
{
    if (count == 1)
        return switch_sd_read_sector(h, lba, private_output, r);
    return read_transfer(h, lba, count, private_output, r);
}

/* Write support deliberately keeps the established read path above unchanged.
 * SD Association Physical Layer v6, 4.17/5.6/5.7/5.8.2, and SDHCI v4.20,
 * 2.2/3.7: optional cache must be proven off before ordinary write completion
 * can be used as a persistence boundary. No extension-register write exists. */
struct write_saved {
    uint16_t size, count, transfer;
    uint32_t argument, enable, signal;
};

static int write_valid(struct switch_sd_host *h, struct switch_sd_result *r, int cache)
{
    if (!h || !r || !h->io.read8 || !h->io.read16 || !h->io.read32 || !h->io.write16 || !h->io.write32 ||
        !h->io.ticks || !h->io.barrier)
        return SWITCH_SD_ARGUMENT;
    if (h->blocked)
        return SWITCH_SD_ALREADY_BLOCKED;
    if (h->dma_required && (!h->dma || !h->dma->bound || h->dma->failed))
        return SWITCH_SD_UNQUALIFIED;
    if (h->block_addressing_verified != 1 || !h->rca || h->rca > 0xffff || h->frequency < 1000 ||
        h->frequency > 1000000000 || !h->capacity_sectors || h->capacity_sectors > UINT64_C(0x100000000) ||
        h->permitted_first_sector >= h->capacity_sectors || !h->permitted_sector_count ||
        h->permitted_sector_count > h->capacity_sectors - h->permitted_first_sector)
        return SWITCH_SD_UNQUALIFIED;
    if (!h->permit_write)
        return SWITCH_SD_WRITE_DENIED;
    if (h->card_write_protected)
        return SWITCH_SD_WRITE_PROTECTED;
    if (cache && h->write_cache_off_verified != 1)
        return SWITCH_SD_CACHE_UNQUALIFIED;
    return SWITCH_SD_OK;
}

static int write_fail(struct switch_sd_host *h, int error)
{
    h->blocked = 1;
    if (h->dma)
        switch_sd_dma_abort(h->dma);
    h->write_cache_off_verified = 0;
    /* A failed in-flight operation is never returned to another owner. */
    h->io.write32(h->context, SIGNAL_ENABLE, 0);
    h->io.barrier(h->context);
    return error;
}

static int write_prepare(struct switch_sd_host *h, struct switch_sd_result *r, struct write_saved *s)
{
    r->present = h->io.read32(h->context, PRESENT);
    r->version = h->io.read16(h->context, VERSION);
    r->host_control = h->io.read8(h->context, HOST_CONTROL);
    r->power = h->io.read8(h->context, POWER);
    r->clock = h->io.read16(h->context, CLOCK);
    r->reset = h->io.read8(h->context, RESET);
    r->block_size = h->io.read16(h->context, BLOCK_SIZE);
    r->readiness_failures = 0;
    if ((r->version & 0xff) != 2 && (r->version & 0xff) != 3)
        r->readiness_failures |= SWITCH_SD_READY_VERSION;
    if (r->present & 0x307)
        r->readiness_failures |= SWITCH_SD_READY_ACTIVE;
    if ((r->present & 0x30000) != 0x30000)
        r->readiness_failures |= SWITCH_SD_READY_CARD;
    if ((r->host_control & 0x22) != 2)
        r->readiness_failures |= SWITCH_SD_READY_BUS;
    if (!(r->power & 1))
        r->readiness_failures |= SWITCH_SD_READY_POWER;
    if ((r->clock & 7) != 7)
        r->readiness_failures |= SWITCH_SD_READY_CLOCK;
    if (r->reset)
        r->readiness_failures |= SWITCH_SD_READY_RESET;
    if ((r->block_size & 0xfff) != 512)
        r->readiness_failures |= SWITCH_SD_READY_BLOCK_SIZE;
    if (r->readiness_failures)
        return SWITCH_SD_NOT_READY;
    /* SDHCI Write Protect Switch Pin Level: 1 means writes are allowed. */
    if (!(r->present & 0x80000))
        return SWITCH_SD_WRITE_PROTECTED;
    if (!(r->present & 0x100000))
        return SWITCH_SD_NOT_READY;
    r->status = h->io.read32(h->context, STATUS);
    r->initial_status = r->status;
    if (r->status & ERRORS)
        return SWITCH_SD_PREEXISTING_ERROR;
    s->size = (uint16_t)r->block_size;
    s->count = h->io.read16(h->context, BLOCK_COUNT);
    s->transfer = h->io.read16(h->context, TRANSFER);
    s->argument = h->io.read32(h->context, ARGUMENT);
    s->enable = h->io.read32(h->context, STATUS_ENABLE);
    s->signal = h->io.read32(h->context, SIGNAL_ENABLE);
    h->io.write32(h->context, SIGNAL_ENABLE, 0);
    h->io.write32(h->context, STATUS_ENABLE, s->enable | 0x03ff8033u | (h->dma ? 8u : 0u));
    h->io.write32(h->context, STATUS, 0x33 | (h->dma ? 8u : 0u));
    return SWITCH_SD_OK;
}

static void write_restore(struct switch_sd_host *h, const struct write_saved *s)
{
    h->io.write16(h->context, BLOCK_SIZE, s->size);
    h->io.write16(h->context, BLOCK_COUNT, s->count);
    h->io.write16(h->context, TRANSFER, s->transfer);
    h->io.write32(h->context, ARGUMENT, s->argument);
    h->io.write32(h->context, STATUS_ENABLE, s->enable);
    h->io.write32(h->context, SIGNAL_ENABLE, s->signal);
    h->io.barrier(h->context);
}

static int write_idle(struct switch_sd_host *h, struct switch_sd_result *r, struct batch_budget *budget)
{
    uint64_t started = h->io.ticks(h->context);
    for (;;) {
        r->status = h->io.read32(h->context, STATUS);
        if (r->status & ERRORS)
            return SWITCH_SD_CONTROLLER_ERROR;
        if (batch_expired(h, budget) || h->io.ticks(h->context) - started >= h->frequency * 2)
            return SWITCH_SD_TIMEOUT;
        uint32_t present = h->io.read32(h->context, PRESENT);
        if ((present & 0x30000) != 0x30000)
            return SWITCH_SD_NOT_READY;
        if (!(present & 0x80000))
            return SWITCH_SD_WRITE_PROTECTED;
        if (!(present & 0x307) && (present & 0x100000))
            return SWITCH_SD_OK;
    }
}

/* Size is 0 for R1 commands, 8 for SCR or 512 for extension/media data.
 * Only the CMD24/25 call site can supply input. Caller serializes the host. */
static int write_command(struct switch_sd_host *h, uint16_t command, uint32_t argument, uint32_t size, uint32_t count,
                         uint8_t *output, const uint8_t *input, struct switch_sd_result *r, struct batch_budget *budget)
{
    struct write_saved saved;
    int error = write_prepare(h, r, &saved);
    if (error)
        return error;
    h->io.write16(h->context, BLOCK_SIZE, (uint16_t)(size ? size : 512));
    h->io.write16(h->context, BLOCK_COUNT, (uint16_t)(size ? count : 0));
    h->io.write32(h->context, ARGUMENT, argument);
    uint16_t transfer = 0x12;
    if (!size) {
        transfer = 0;
    } else if (input) {
        transfer = count > 1 ? 0x26 : 0x02;
    }
    h->io.write16(h->context, TRANSFER, transfer);
    if (size && h->dma) {
        error = switch_sd_dma_prepare(h->dma, &h->io, h->context, size, count, input);
        if (error)
            return write_fail(h, error);
    }
    h->io.barrier(h->context);
    h->io.write16(h->context, COMMAND, command);
    error = wait_event(h, COMMAND_DONE, r, budget, 0);
    if (error)
        return write_fail(h, error);
    r->response = h->io.read32(h->context, RESPONSE);
    if (r->response & CARD_ERRORS)
        return write_fail(h, SWITCH_SD_CARD_ERROR);
    h->io.write32(h->context, STATUS, COMMAND_DONE);
    if (size) {
        if (h->dma) {
            error = switch_sd_dma_wait(h->dma, &h->io, h->context, r);
            if (error)
                return write_fail(h, error);
        } else {
            for (uint32_t block = 0; block < count; block++) {
                if (input) {
                    /* Completion before the last word is accepted is always short,
                 * even when BUFFER_WRITE_READY is coasserted. */
                    uint64_t started = h->io.ticks(h->context);
                    for (;;) {
                        r->status = h->io.read32(h->context, STATUS);
                        if (r->status & ERRORS)
                            return write_fail(h, SWITCH_SD_CONTROLLER_ERROR);
                        if (batch_expired(h, budget) || h->io.ticks(h->context) - started >= h->frequency)
                            return write_fail(h, SWITCH_SD_TIMEOUT);
                        if (r->status & TRANSFER_DONE)
                            return write_fail(h, SWITCH_SD_BUFFER_STATE);
                        if (r->status & 0x10)
                            break;
                    }
                    if (!(h->io.read32(h->context, PRESENT) & 0x400))
                        return write_fail(h, SWITCH_SD_BUFFER_STATE);
                    h->io.write32(h->context, STATUS, 0x10);
                    /* Buffer-ready reserves this complete block's FIFO space.
                 * Device/status reads and serializing timer samples per word
                 * stall PIO; check latched errors after the bounded block. */
                    for (uint32_t word = 0; word < size / 4; word++) {
                        uint32_t at = block * size + word * 4;
                        uint32_t value = 0;
                        for (uint32_t j = 0; j < 4; j++)
                            value |= (uint32_t)input[at + j] << (8 * j);
                        h->io.write32(h->context, BUFFER, value);
                        r->words_written++;
                    }
                    r->status = h->io.read32(h->context, STATUS);
                    if (r->status & ERRORS)
                        return write_fail(h, SWITCH_SD_CONTROLLER_ERROR);
                    if ((r->status & TRANSFER_DONE) && block + 1 < count)
                        return write_fail(h, SWITCH_SD_BUFFER_STATE);
                    if (batch_expired(h, budget))
                        return write_fail(h, SWITCH_SD_TIMEOUT);
                } else {
                    error = wait_event(h, BUFFER_READY, r, budget, 1);
                    if (error)
                        return write_fail(h, error);
                    if (!(h->io.read32(h->context, PRESENT) & 0x800))
                        return write_fail(h, SWITCH_SD_BUFFER_STATE);
                    h->io.write32(h->context, STATUS, BUFFER_READY);
                    for (uint32_t word = 0; word < size / 4; word++) {
                        uint32_t value = h->io.read32(h->context, BUFFER);
                        for (uint32_t j = 0; j < 4; j++)
                            output[block * size + word * 4 + j] = (uint8_t)(value >> (8 * j));
                        r->words_read++;
                    }
                }
            }
        }
        if (!h->dma) {
            error = wait_event(h, TRANSFER_DONE, r, budget, 0);
            if (error)
                return write_fail(h, error);
            h->io.write32(h->context, STATUS, TRANSFER_DONE);
        }
    }
    h->io.barrier(h->context);
    error = write_idle(h, r, budget);
    if (error)
        return write_fail(h, error);
    if (input && count > 1) {
        r->auto_cmd_error = h->io.read16(h->context, AUTO_CMD_ERROR);
        r->stop_response = h->io.read32(h->context, STOP_RESPONSE);
        if (r->auto_cmd_error & 0x9f)
            return write_fail(h, SWITCH_SD_CONTROLLER_ERROR);
        if (r->stop_response & CARD_ERRORS)
            return write_fail(h, SWITCH_SD_CARD_ERROR);
    }
    if (size && h->dma) {
        error = switch_sd_dma_complete(h->dma, h->context, output);
        if (error)
            return write_fail(h, error);
        if (input) {
            r->words_written = size * count / 4;
        } else {
            r->words_read += size * count / 4;
        }
    }
    write_restore(h, &saved);
    return SWITCH_SD_OK;
}

static int write_ready(struct switch_sd_host *h, struct switch_sd_result *r, struct batch_budget *budget)
{
    int error = write_idle(h, r, budget);
    if (error)
        return write_fail(h, error);
    for (;;) {
        error = write_command(h, 0x0d1a, h->rca << 16, 0, 0, 0, 0, r, budget);
        if (error)
            return write_fail(h, error);
        r->card_status = r->response;
        uint32_t state = (r->card_status >> 9) & 15;
        if (state == 4 && (r->card_status & 0x100))
            return SWITCH_SD_OK;
        if (state != 7 && state != 4)
            return write_fail(h, SWITCH_SD_CARD_STATE);
        if (batch_expired(h, budget))
            return write_fail(h, SWITCH_SD_TIMEOUT);
    }
}

static uint32_t write_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t write_le32(const uint8_t *p)
{
    return write_le16(p) | write_le16(p + 2) << 16;
}

static int write_cache_probe(struct switch_sd_host *h, struct switch_sd_result *r, struct batch_budget *budget)
{
    uint8_t bytes[512];
    uint8_t general[512];
    int error = write_command(h, 0x371a, h->rca << 16, 0, 0, 0, 0, r, budget);
    if (error)
        return error;
    if (!(r->response & 0x20))
        return SWITCH_SD_CARD_STATE;
    error = write_command(h, 0x333a, 0, 8, 1, bytes, 0, r, budget);
    if (error)
        return error;
    uint64_t scr = 0;
    for (uint32_t i = 0; i < 8; i++)
        scr = scr << 8 | bytes[i];
    r->scr_high = (uint32_t)(scr >> 32);
    r->scr_low = (uint32_t)scr;
    /* Existing SCR layout and mandatory 1/4-bit bus support. Reserved SCR
     * structures/spec encodings are not proof that an optional cache is off. */
    uint32_t spec = (uint32_t)(scr >> 56) & 15;
    if ((scr >> 60) != 0 || spec != 2 || ((scr >> 48) & 5) != 5 ||
        ((scr & (UINT64_C(1) << 35)) && !(scr & (UINT64_C(1) << 34))))
        return SWITCH_SD_CACHE_UNQUALIFIED;
    if (!(scr & UINT64_C(1) << 34)) {
        r->cache_evidence = SWITCH_SD_CACHE_NO_CMD48;
        return SWITCH_SD_OK;
    }
    if (spec != 2 || !(scr & (UINT64_C(1) << 47)))
        return SWITCH_SD_CACHE_UNQUALIFIED;
    error = write_command(h, 0x303a, 0x1ff, 512, 1, general, 0, r, budget);
    if (error)
        return error;
    uint32_t length = write_le16(general + 2);
    uint32_t total = general[4];
    uint32_t ext = 16;
    if (write_le16(general) != 0 || length < 16 || length > 512 || !total || total > 10)
        return SWITCH_SD_CACHE_UNQUALIFIED;
    uint32_t seen[10] = {0};
    uint32_t perf = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (ext < 16 || ext > length || length - ext < 48)
            return SWITCH_SD_CACHE_UNQUALIFIED;
        for (uint32_t j = 0; j < i; j++) {
            if (ext < seen[j] + 48 && seen[j] < ext + 48)
                return SWITCH_SD_CACHE_UNQUALIFIED;
        }
        seen[i] = ext;
        uint32_t next = write_le16(general + ext + 40);
        uint32_t registers = general[ext + 42];
        if (registers != 1 || (i + 1 == total ? next != 0 : next == 0))
            return SWITCH_SD_CACHE_UNQUALIFIED;
        if (write_le16(general + ext) == 2) {
            if (perf++)
                return SWITCH_SD_CACHE_UNQUALIFIED;
            uint32_t address = write_le32(general + ext + 44);
            uint32_t fno = (address >> 18) & 15;
            uint32_t page = (address >> 9) & 255;
            /* Standard performance register is one 512-byte page. A nonzero
             * starting offset or reserved address bit needs a larger parser. */
            if ((address & 0xffc201ffu) || !fno)
                return SWITCH_SD_CACHE_UNQUALIFIED;
            error = write_command(h, 0x303a, fno << 27 | page << 18 | 0x1ff, 512, 1, bytes, 0, r, budget);
            if (error)
                return error;
            if (bytes[0] != 0 || (bytes[260] & 1) || (bytes[262] & 1))
                return SWITCH_SD_CACHE_UNQUALIFIED;
            r->cache_evidence = (bytes[4] & 1) ? SWITCH_SD_CACHE_OBSERVED_OFF : SWITCH_SD_CACHE_UNSUPPORTED;
        }
        ext = next;
    }
    if (!perf)
        r->cache_evidence = SWITCH_SD_CACHE_NO_PERF;
    return SWITCH_SD_OK;
}

int switch_sd_qualify_writes(struct switch_sd_host *h, struct switch_sd_result *r)
{
    if (r)
        *r = (struct switch_sd_result){0};
    int error = write_valid(h, r, 0);
    if (error)
        return error;
    h->write_cache_off_verified = 0;
    r->stage = 10;
    struct batch_budget budget = {h->io.ticks(h->context), 0};
    error = write_ready(h, r, &budget);
    if (error)
        return error;
    r->stage = 11;
    error = write_cache_probe(h, r, &budget);
    if (error)
        return write_fail(h, error);
    error = write_ready(h, r, &budget);
    if (error)
        return error;
    h->write_cache_off_verified = 1;
    r->stage = 12;
    return SWITCH_SD_OK;
}

int switch_sd_sync(struct switch_sd_host *h, struct switch_sd_result *r)
{
    if (r)
        *r = (struct switch_sd_result){0};
    int error = write_valid(h, r, 1);
    if (error)
        return error;
    r->stage = 30;
    struct batch_budget budget = {h->io.ticks(h->context), 0};
    error = write_ready(h, r, &budget);
    if (!error)
        r->stage = 31;
    return error;
}

int switch_sd_write_sectors(struct switch_sd_host *h, uint64_t lba, uint32_t count, const uint8_t *input,
                            struct switch_sd_result *r)
{
    if (r)
        *r = (struct switch_sd_result){0};
    if (!input || !count || count > SWITCH_SD_MAX_WRITE_SECTORS)
        return SWITCH_SD_ARGUMENT;
    int error = write_valid(h, r, 1);
    if (error)
        return error;
    if (lba < h->permitted_first_sector || lba - h->permitted_first_sector >= h->permitted_sector_count ||
        count > h->permitted_sector_count - (lba - h->permitted_first_sector))
        return SWITCH_SD_ARGUMENT;
    /* No reads, timer sampling or register writes can precede this exact-range
     * authorization. A partition-sized allowance alone is never sufficient. */
    if (h->permit_write(h->write_context, lba, count) != 1)
        return SWITCH_SD_WRITE_DENIED;
    r->stage = 20;
    struct batch_budget budget = {h->io.ticks(h->context), 0};
    error = write_command(h, count > 1 ? 0x193a : 0x183a, (uint32_t)lba, 512, count, 0, input, r, &budget);
    if (error)
        return error;
    r->stage = 21;
    error = write_ready(h, r, &budget);
    if (!error)
        r->stage = 22;
    return error;
}
