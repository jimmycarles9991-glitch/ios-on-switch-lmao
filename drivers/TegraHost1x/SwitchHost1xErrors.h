// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's general interrupt (T210 TRM 14.4.6.2; SPI 67, tegra210.dtsi host1x@50000000 "host"): each channel's command
// processor reports an illegal opcode in HINTSTATUS_EXT bits 13:0 (14.9.7). With CMDPROC_CTRL's DROP_ILLEGAL_OPCODES at
// its reset 0 (14.9.10) the channel stops there, and its bit may latch again for as long as it stays stopped, so the
// filter masks a channel's bit when it takes its error and the channel's restart unmasks it. The channel's jobs fail at
// once (SwitchHost1xChannels.h switch_host1x_channels_failed) instead of at their bound, 2 to 30 s later.
// Four masks stand between a channel's illegal opcode and the CPU: its HINTMASK_EXT bit, HINTMASK's EXT enable, then
// INTC0MASK's HOST_INT bit (14.9.3: host1x's own interrupt is a "module" of CPU0's signal, as L4T's t20_intr_resume
// sets it) and INTMASK's CPU0 master. Only the illegal opcodes reach the line: IP_BUSY_TIMEOUT is 0, so the IP read and
// write timeouts never fire; configure sets HINTMASK to the EXT enable alone and INTC0MASK to HOST_INT alone, whatever
// the boot chain left there. The other INTC0MASK bits are the engines' interrupts, which reach the GIC on lines of
// their own (14.4.6.1) and can only be cleared in the engine (14.9.1). A line that still fires with none of ours
// pending panics: it is a source this driver set up wrong, and the level line could not be left on or turned off
// without losing every later stop.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
class IOWorkLoop;

namespace switch_host1x_errors {
// Sync-unit registers (TRM 14.9.2, 14.9.5-14.9.8, 14.9.11).
constexpr uint32_t kIntStatus = 0x000, kIntMask = 0x004, kIntC0Mask = 0x008, kHintStatus = 0x020, kHintMask = 0x024,
                   kHintStatusExt = 0x028, kHintMaskExt = 0x02c, kCmdprocStat = 0x0a8;
constexpr uint32_t kCpu0All = 1u << 0, kHostInt = 1u << 0, kExtEnable = 1u << 31, kChannels = 14,
                   kIllegalAll = (1u << kChannels) - 1;

// What configure found and what read back after it wrote.
struct Configured {
    uint32_t latched;      // illegal opcodes latched before (cleared)
    uint32_t intc0Was;     // INTC0MASK as the boot chain or SC7 left it
    uint32_t intc0;        // read back after HOST_INT alone was written
    uint32_t intmask;      // read back after CPU0's master enable was set
    uint32_t hintmask;     // read back after the EXT enable alone was written
    uint32_t hintmaskExt;  // read back after every channel's illegal opcode was unmasked
};

// Registers: read(offset), write(offset, value), sync-unit relative. The caller serialises take() (the filter) and
// rearm() (a restart) against each other.
template <class Registers> struct Unit {
    uint32_t masked = 0;  // channels whose error was taken and whose restart has not come yet

    // After the sync unit came out of reset (boot, SC7), every channel freshly programmed: what latched before is
    // cleared, every channel's illegal opcode unmasked, HINTMASK set to the EXT enable alone and INTC0MASK to HOST_INT
    // alone, then INTMASK set to CPU0's master enable alone, as L4T's t20_intr_resume writes it: bit 4 (CPU TrustZone
    // violations, reset 1) goes off, since no host1x register reports one for the filter to take.
    Configured configure(Registers &r)
    {
        Configured c = {};
        c.latched = r.read(kHintStatusExt) & kIllegalAll;
        r.write(kHintStatusExt, c.latched);
        masked = 0;
        r.write(kHintMaskExt, kIllegalAll);
        r.write(kHintMask, kExtEnable);
        c.intc0Was = r.read(kIntC0Mask);
        r.write(kIntC0Mask, kHostInt);
        r.write(kIntMask, kCpu0All);
        c.hintmask = r.read(kHintMask);
        c.hintmaskExt = r.read(kHintMaskExt);
        c.intc0 = r.read(kIntC0Mask);
        c.intmask = r.read(kIntMask);
        return c;
    }

    // The filter, interrupts off: the channels whose illegal opcode is pending and unmasked, masked and their status
    // cleared. 0: none of ours.
    uint32_t take(Registers &r)
    {
        const uint32_t pending = r.read(kHintStatusExt) & kIllegalAll & ~masked;
        if (!pending)
            return 0;
        masked |= pending;
        r.write(kHintMaskExt, kIllegalAll & ~masked);
        r.write(kHintStatusExt, pending);
        (void)r.read(kHintStatusExt);  // the clear reaches host1x before the filter returns and the line is re-enabled
        return pending;
    }

    // The channel restarted on an empty stream: what its stopped command processor latched meanwhile is cleared, and
    // its illegal opcode unmasked again.
    void rearm(Registers &r, uint32_t channel)
    {
        const uint32_t bit = 1u << channel;
        if (channel >= kChannels || !(masked & bit))
            return;
        masked &= ~bit;
        r.write(kHintStatusExt, bit);
        r.write(kHintMaskExt, kIllegalAll & ~masked);
    }
};
}  // namespace switch_host1x_errors

// Thread context, once the channels are programmed: the interrupt attached on host1x's work loop and unmasked. One that
// cannot be attached, or a mask among the four that does not read back as written, panics (here and at the SC7 resume).
void switch_host1x_errors_start(IOWorkLoop *loop, volatile uint8_t *sync);
// SC7: off before the sync unit powers down, configured and on again once it is back and the channels programmed.
void switch_host1x_errors_stop();
void switch_host1x_errors_resume();
#endif

#ifdef __cplusplus
extern "C" {
#endif
// SwitchHost1xChannels.cpp's restart of `channel` (submit lock held).
void switch_host1x_errors_rearm(uint32_t channel);
#ifdef __cplusplus
}
#endif
