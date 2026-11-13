// SPDX-License-Identifier: BSD-2-Clause-Patent
// A window probe step's extra registers (DisplayWindowProbe.h Step::extra): words the run writes with the step's
// windows and puts back before anything else, among them registers it saves nowhere else (a window's CSC between steps,
// its palette, its BYTE_SWAP after the window's own write). Each is read before the step writes it, written as
// (old & ~mask) | (value & mask), and restored from that read when the next step shows, when the windows are restored
// (the end of the run, or a step that failed or timed out) and at the run's finish, whichever comes first; restoring
// twice writes nothing. Words are DC A register numbers (the direct window apertures: mainline dc.c tegra_plane_offset,
// WINC A 0xa00 / B 0xc00, DC_WIN A 0xb80 / B 0xd80). Plain logic over a register accessor, no IOKit.
#pragma once
#include <stdint.h>

namespace display_window_probe {

struct Extra {
    uint32_t offset, value, mask;
};

constexpr uint32_t kWincA = 0xa00, kWincB = 0xc00, kDcWinA = 0xb80, kDcWinB = 0xd80;
constexpr uint32_t kPaletteWords = 256, kCscWords = 8;
constexpr uint32_t kCscYofWord = 0x111;  // WINC + 0x111 .. + 0x118: YOF, KYRGB, KUR, KVR, KUG, KVG, KUB, KVB
constexpr unsigned kMaxExtras = 2 * kPaletteWords + 2 * kCscWords + 4;

class ExtraRegisters {
public:
    template <typename Registers> void apply(Registers &dc, const Extra *extra, unsigned count)
    {
        restore(dc);
        count_ = count < kMaxExtras ? count : kMaxExtras;
        for (unsigned i = 0; i < count_; ++i) {
            const Extra &e = extra[i];
            offset_[i] = e.offset;
            saved_[i] = dc[e.offset];
            dc[e.offset] = (saved_[i] & ~e.mask) | (e.value & e.mask);
        }
    }

    // In reverse, so a word written twice gets its first saved value back.
    template <typename Registers> void restore(Registers &dc)
    {
        for (unsigned i = count_; i-- > 0;)
            dc[offset_[i]] = saved_[i];
        count_ = 0;
    }

    unsigned pending() const
    {
        return count_;
    }

private:
    unsigned count_ = 0;
    uint32_t offset_[kMaxExtras] = {}, saved_[kMaxExtras] = {};
};

}  // namespace display_window_probe
