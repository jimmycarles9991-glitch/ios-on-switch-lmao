// SPDX-License-Identifier: MIT
// The channel's context init (kits/SwitchGM20BEmit gen/SwitchGM20BContext.h) as nvdec executes it, in the probe capture's
// form directlink.py reads: a T line per method the init writes and an S regs line per subchannel, its nonzero registers
// after the init (MME macros run), which the pack's baseline is laid over; then an I line per method with its relocation
// (subchannel, method, word, PackReloc), the pack's init records.
// usage: context-state > context.log
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "SwitchGM20BContext.h"
#include "nvdec.h"

int main()
{
    // One submission, traced: its T lines, then its S regs (nvdec.cpp submitEnd)
    setenv("NVDEC_TRACE", "1", 1);
    nvdec::Decoder decoder(
        [](uint64_t, uint64_t) -> const uint8_t * {
            return nullptr;
        },
        stdout);
    decoder.submitBegin();
    for (const switch_gm20b_context_method &m : switch_gm20b_context) {
        const uint32_t words[2] = {1u << 29 | 1u << 16 | uint32_t(m.subc) << 13 | uint32_t(m.mthd) >> 2, m.data};
        decoder.push(words, 2);
    }
    decoder.submitEnd();
    for (const switch_gm20b_context_method &m : switch_gm20b_context)
        printf("I %u %x %x %u\n", unsigned(m.subc), unsigned(m.mthd), unsigned(m.data), unsigned(m.reloc));
    return decoder.badHeaders ? 1 : 0;
}
