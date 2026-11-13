// SPDX-License-Identifier: MIT
// The kit's headers as kernel C++ sees them: prototypes, the push builder and the decoder.
#include "SwitchGM20BEmit3D.h"
#include "SwitchGM20BEmitBarrier.h"
#include "SwitchGM20BEmitCopy.h"
#include "SwitchGM20BPushDecode.h"

extern "C" uint32_t switch_gm20b_emit_kernel_check_cxx(uint32_t *words, size_t count);

extern "C" uint32_t switch_gm20b_emit_kernel_check_cxx(uint32_t *words, size_t count)
{
    struct nv_push push;
    nv_push_init(&push, words, count, SUBC_MASK_ALL);
    switch_gm20b_pass_barrier(&push, true);
    switch_gm20b_method decoded[8];
    return static_cast<uint32_t>(switch_gm20b_push_decode(words, nv_push_dw_count(&push), decoded, 8));
}
