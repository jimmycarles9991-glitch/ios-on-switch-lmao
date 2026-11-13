// SPDX-License-Identifier: MIT
// A push read back as the methods it writes, in order: what two emitters are compared by when their header forms differ.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

struct switch_gm20b_method {
    uint8_t subchannel;
    uint16_t method;
    uint32_t value;
};

static inline void switch_gm20b_method_set(struct switch_gm20b_method *m, uint8_t subc, uint32_t mthd, uint32_t value)
{
    m->subchannel = subc;
    m->method = (uint16_t)mthd;
    m->value = value;
}

// The methods of words[0, count) into out[0, max); -1 for a header this cannot read or a push cut short
static inline long switch_gm20b_push_decode(const uint32_t *words, size_t count, struct switch_gm20b_method *out,
                                            size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < count;) {
        const uint32_t hdr = words[i++];
        const uint32_t op = (uint32_t)NVVAL_GET(hdr, NV906F, DMA, SEC_OP);
        const uint8_t subc = (uint8_t)NVVAL_GET(hdr, NV906F, DMA, METHOD_SUBCHANNEL);
        const uint16_t mthd = (uint16_t)(NVVAL_GET(hdr, NV906F, DMA, METHOD_ADDRESS) << 2);
        const uint32_t len = (uint32_t)NVVAL_GET(hdr, NV906F, DMA, METHOD_COUNT);
        if (op == NV906F_DMA_SEC_OP_IMMD_DATA_METHOD) {
            if (n == max)
                return -1;
            switch_gm20b_method_set(&out[n++], subc, mthd, len);
            continue;
        }
        if (op != NV906F_DMA_SEC_OP_INC_METHOD && op != NV906F_DMA_SEC_OP_NON_INC_METHOD &&
            op != NV906F_DMA_SEC_OP_ONE_INC)
            return -1;
        for (uint32_t k = 0; k < len; k++) {
            if (i == count || n == max)
                return -1;
            uint32_t step = op == NV906F_DMA_SEC_OP_INC_METHOD ? k : (op == NV906F_DMA_SEC_OP_ONE_INC && k ? 1 : 0);
            switch_gm20b_method_set(&out[n++], subc, mthd + 4 * step, words[i++]);
        }
    }
    return (long)n;
}

#ifdef __cplusplus
}
#endif
