// SPDX-License-Identifier: BSD-2-Clause-Patent
// A register block its documentation numbers in 32-bit words (Tegra's display controller, DSI, MIPI_CAL), indexed as
// the TRM numbers it: regs[DC_CMD_STATE_CONTROL] = value, regs[reg] |= bits, uint32_t value = regs[reg]. Every read is
// switch_mmio_read32 (a load-acquire, Cortex-A57 erratum 832075) and every write switch_mmio_write32. C++ only.
#ifndef SWITCH_MMIO_WORDS_H
#define SWITCH_MMIO_WORDS_H

#include <stddef.h>
#include <stdint.h>
#include "SwitchMMIO.h"

// One register: reading it converts to its value, assigning writes it. A copy of a register is a read of one and a
// write of the other, never a copy of the reference.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions): a reference, not a value
class SwitchMMIOWord {
public:
    explicit SwitchMMIOWord(volatile uint8_t *at) : at_(at)
    {}

    SwitchMMIOWord(const SwitchMMIOWord &) = default;

    // NOLINTNEXTLINE(misc-explicit-constructor): reads as the register it stands for
    operator uint32_t() const
    {
        return switch_mmio_read32(at_, 0);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-c-copy-assignment-signature,misc-unconventional-assign-operator): a store
    SwitchMMIOWord &operator=(uint32_t value)
    {
        switch_mmio_write32(at_, 0, value);
        return *this;
    }

    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp): a self-copy reads and rewrites the register
    SwitchMMIOWord &operator=(const SwitchMMIOWord &other)
    {
        return *this = static_cast<uint32_t>(other);
    }

    SwitchMMIOWord &operator|=(uint32_t bits)
    {
        return *this = static_cast<uint32_t>(*this) | bits;
    }

    SwitchMMIOWord &operator&=(uint32_t bits)
    {
        return *this = static_cast<uint32_t>(*this) & bits;
    }

private:
    volatile uint8_t *at_;
};

class SwitchMMIOWords {
public:
    SwitchMMIOWords() = default;

    explicit SwitchMMIOWords(volatile uint8_t *base) : base_(base)
    {}

    explicit SwitchMMIOWords(uintptr_t address) : base_(reinterpret_cast<volatile uint8_t *>(address))
    {}

    SwitchMMIOWord operator[](uint32_t index) const
    {
        return SwitchMMIOWord(base_ + static_cast<size_t>(index) * 4);
    }

    // The block `words` registers in: a window's registers within the display controller's, say.
    SwitchMMIOWords operator+(uint32_t words) const
    {
        return SwitchMMIOWords(base_ + static_cast<size_t>(words) * 4);
    }

    explicit operator bool() const
    {
        return base_ != nullptr;
    }

private:
    volatile uint8_t *base_ = nullptr;
};

#endif
