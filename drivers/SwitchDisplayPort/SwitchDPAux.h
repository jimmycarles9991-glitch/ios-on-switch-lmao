// SPDX-License-Identifier: GPL-2.0-only
// DPAUX1 transactions in plain C for the host tests: a request's DP_AUXCTL and FIFO words, a reply's meaning from
// DP_AUXSTAT, the retry rule, and the I2C-over-AUX requests that read one EDID block.
//
// Registers and reply codes: Linux drivers/gpu/drm/tegra/dpaux.c and dpaux.h (Copyright (C) 2013 NVIDIA Corporation,
// GPL-2.0-only), tegra_dpaux_transfer. The FIFOs hold 16 bytes each way, so no request is longer.
#ifndef SWITCH_DP_AUX_H
#define SWITCH_DP_AUX_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_DPAUX_INTR_EN_AUX         0x01u
#define SWITCH_DPAUX_INTR_AUX            0x05u
#define SWITCH_DPAUX_INTR_AUX_DONE       (1u << 3)
#define SWITCH_DPAUX_INTR_IRQ_EVENT      (1u << 2)
#define SWITCH_DPAUX_INTR_UNPLUG_EVENT   (1u << 1)
#define SWITCH_DPAUX_INTR_PLUG_EVENT     (1u << 0)
#define SWITCH_DPAUX_AUXDATA_WRITE(x)    (0x09u + ((x) << 2))
#define SWITCH_DPAUX_AUXDATA_READ(x)     (0x19u + ((x) << 2))
#define SWITCH_DPAUX_AUXADDR             0x29u
#define SWITCH_DPAUX_AUXCTL              0x2Du
#define SWITCH_DPAUX_AUXCTL_RST          (1u << 31)
#define SWITCH_DPAUX_AUXCTL_TRANSACTREQ  (1u << 16)
#define SWITCH_DPAUX_AUXCTL_ADDRESS_ONLY (1u << 8)
#define SWITCH_DPAUX_AUXSTAT             0x31u
#define SWITCH_DPAUX_AUXSTAT_HPD         (1u << 28)
#define SWITCH_DPAUX_AUXSTAT_ERRORS      0xF00u
#define SWITCH_DPAUX_HYBRID_PADCTL       0x49u
#define SWITCH_DPAUX_HYBRID_SPARE        0x4Du

#define SWITCH_DP_AUX_MAX      16u
#define SWITCH_DP_AUX_RETRIES  7u
#define SWITCH_DP_AUX_RETRY_US 500u
#define SWITCH_DP_EDID_ADDRESS 0x50u
#define SWITCH_DP_EDID_SEGMENT 0x30u

enum switch_dp_aux_op {
    SWITCH_DP_AUX_I2C_WRITE = 0x0,
    SWITCH_DP_AUX_I2C_READ = 0x1,
    SWITCH_DP_AUX_I2C_MOT_WRITE = 0x4,
    SWITCH_DP_AUX_I2C_MOT_READ = 0x5,
    SWITCH_DP_AUX_NATIVE_WRITE = 0x8,
    SWITCH_DP_AUX_NATIVE_READ = 0x9,
};

struct switch_dp_aux_request {
    enum switch_dp_aux_op op;
    uint32_t address;
    uint8_t size;  // 0: an I2C address-only transaction (start or stop)
    uint8_t data[SWITCH_DP_AUX_MAX];
};

enum switch_dp_aux_result {
    SWITCH_DP_AUX_ACK,
    SWITCH_DP_AUX_NACK,
    SWITCH_DP_AUX_DEFER,
    SWITCH_DP_AUX_I2C_NACK,
    SWITCH_DP_AUX_I2C_DEFER,
    SWITCH_DP_AUX_TIMEOUT,
    SWITCH_DP_AUX_ERROR,
    SWITCH_DP_AUX_SHORT,  // an I2C read acked with fewer bytes than asked: asked again, as Linux does (-EBUSY)
};

static inline bool switch_dp_aux_reads(enum switch_dp_aux_op op)
{
    return op == SWITCH_DP_AUX_I2C_READ || op == SWITCH_DP_AUX_I2C_MOT_READ || op == SWITCH_DP_AUX_NATIVE_READ;
}

static inline bool switch_dp_aux_is_i2c(enum switch_dp_aux_op op)
{
    return op != SWITCH_DP_AUX_NATIVE_WRITE && op != SWITCH_DP_AUX_NATIVE_READ;
}

// DP_AUXCTL for a request, without TRANSACTREQ (set by a second write once the FIFO is filled). 0 for a request the
// controller cannot carry.
static inline uint32_t switch_dp_auxctl(const struct switch_dp_aux_request *request)
{
    if (request->size > SWITCH_DP_AUX_MAX || (request->size == 0 && !switch_dp_aux_is_i2c(request->op))) {
        return 0;
    }
    const uint32_t length = request->size == 0 ? SWITCH_DPAUX_AUXCTL_ADDRESS_ONLY : (uint32_t)(request->size - 1u);
    return ((uint32_t)request->op << 12) | length;
}

// The write FIFO's four words, little-endian bytes; unused bytes zero.
static inline void switch_dp_aux_fifo_words(const struct switch_dp_aux_request *request, uint32_t words[4])
{
    for (unsigned i = 0; i < 4; ++i) {
        words[i] = 0;
    }
    for (unsigned i = 0; i < request->size && i < SWITCH_DP_AUX_MAX; ++i) {
        words[i / 4] |= (uint32_t)request->data[i] << (8u * (i % 4));
    }
}

static inline enum switch_dp_aux_result switch_dp_aux_decode(uint32_t auxstat,
                                                             const struct switch_dp_aux_request *request)
{
    if ((auxstat & (1u << 8)) != 0) {
        return SWITCH_DP_AUX_TIMEOUT;
    }
    if ((auxstat & (7u << 9)) != 0) {
        return SWITCH_DP_AUX_ERROR;
    }
    switch ((auxstat >> 16) & 0xFu) {
    case 0x0:
        break;
    case 0x1:
        return SWITCH_DP_AUX_NACK;
    case 0x2:
        return SWITCH_DP_AUX_DEFER;
    case 0x4:
        return SWITCH_DP_AUX_I2C_NACK;
    case 0x8:
        return SWITCH_DP_AUX_I2C_DEFER;
    default:
        return SWITCH_DP_AUX_ERROR;
    }
    if (switch_dp_aux_reads(request->op) && request->size > 0 && (auxstat & 0xFFu) != request->size) {
        return SWITCH_DP_AUX_SHORT;
    }
    return SWITCH_DP_AUX_ACK;
}

// Microseconds before asking again, or 0 to give up: defers, timeouts and short reads are asked again after 500 us up
// to seven times (a sink leaving D3 takes about 1 ms to answer); NACKs and line errors are final.
static inline uint32_t switch_dp_aux_retry_us(enum switch_dp_aux_result result, unsigned attempt)
{
    const bool transient = result == SWITCH_DP_AUX_DEFER || result == SWITCH_DP_AUX_I2C_DEFER ||
                           result == SWITCH_DP_AUX_TIMEOUT || result == SWITCH_DP_AUX_SHORT;
    return transient && attempt < SWITCH_DP_AUX_RETRIES ? SWITCH_DP_AUX_RETRY_US : 0;
}

static inline struct switch_dp_aux_request switch_dp_aux_make(enum switch_dp_aux_op op, uint32_t address, uint8_t size)
{
    struct switch_dp_aux_request request = {op, address, size, {0}};
    return request;
}

#define SWITCH_DP_EDID_REQUESTS 11u

// The requests that read EDID block `block` (128 bytes, blocks 2 and up behind the E-DDC segment pointer): the offset
// written with MOT, eight 16-byte reads with MOT, then an address-only read to stop. Read request k (k = 0..7) lands at
// byte 16 * k of the block. Returns the count.
static inline unsigned switch_dp_edid_requests(unsigned block, struct switch_dp_aux_request *requests)
{
    unsigned n = 0;
    if (block >= 2) {
        requests[n] = switch_dp_aux_make(SWITCH_DP_AUX_I2C_MOT_WRITE, SWITCH_DP_EDID_SEGMENT, 1);
        requests[n++].data[0] = (uint8_t)(block / 2);
    }
    requests[n] = switch_dp_aux_make(SWITCH_DP_AUX_I2C_MOT_WRITE, SWITCH_DP_EDID_ADDRESS, 1);
    requests[n++].data[0] = (uint8_t)((block % 2) * 128u);
    for (unsigned k = 0; k < 8; ++k) {
        requests[n++] = switch_dp_aux_make(SWITCH_DP_AUX_I2C_MOT_READ, SWITCH_DP_EDID_ADDRESS, 16);
    }
    requests[n++] = switch_dp_aux_make(SWITCH_DP_AUX_I2C_READ, SWITCH_DP_EDID_ADDRESS, 0);
    return n;
}

#endif
