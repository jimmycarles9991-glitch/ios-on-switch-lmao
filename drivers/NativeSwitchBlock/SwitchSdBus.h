// SPDX-License-Identifier: BSD-2-Clause-Patent
// SDMMC1's bus between commands: what the command in flight still owes before the next may go out, and whether
// PRESENT_STATE agrees that it may.
//
// SD Host Controller Simplified Specification 4.20, 2.2.9 and 2.2.17: Command Inhibit (CMD) falling raises Command
// Complete, and Command Inhibit (DAT) / DAT Line Active falling raises Transfer Complete: the end of a read, the card's
// busy release after a write's last block, the end of an R1b's busy. The interrupts a command ends with are therefore
// the wait for the bus. Once they are taken PRESENT_STATE is clear at its first read; a busy bit then is the controller
// or the driver breaking that contract (a failed command, never something to wait out).
#ifndef SWITCH_SD_BUS_H
#define SWITCH_SD_BUS_H

#include <stdint.h>

enum : uint32_t {
    kSdPresentCmdInhibit = 1u << 0,
    kSdPresentDatInhibit = 1u << 1,
    kSdPresentDatActive = 1u << 2,
    kSdPresentWriteActive = 1u << 8,
    kSdPresentReadActive = 1u << 9,
    kSdPresentDat0 = 1u << 20,
    kSdCommandComplete = 1u << 0,
    kSdTransferComplete = 1u << 1,
};

// How a command uses the bus. kCommand: the CMD line only (a response without busy). kRead: data from the card. kBusy:
// an R1b without data (CMD13 sent that way is how a busy card's release is awaited). kWrite: blocks to the card (CMD25,
// CMD49), which must not be programming when they start and holds DAT0 after the last. kAbort: CMD12 after a reset, which
// the spec lets out while the DAT lines are busy (the Implementation Note to 2.2.9) and which ends like an R1b.
enum class SwitchSdUse : uint8_t {
    kCommand,
    kRead,
    kBusy,
    kWrite,
    kAbort
};

enum class SwitchSdBusState : uint8_t {
    kFree,
    kOwed,      // the previous command's interrupts were never taken (a driver bug)
    kHeld,      // its interrupts came, yet PRESENT_STATE still shows an inhibit (the controller broke the contract)
    kCardBusy,  // a data command needs the card's DAT0 release first
};

constexpr uint32_t switchSdEnds(SwitchSdUse use)
{
    return use == SwitchSdUse::kCommand ? kSdCommandComplete : kSdCommandComplete | kSdTransferComplete;
}

constexpr bool switchSdHoldsCard(SwitchSdUse use)
{
    return use == SwitchSdUse::kBusy || use == SwitchSdUse::kWrite || use == SwitchSdUse::kAbort;
}

// The PRESENT_STATE bits that must be clear before a command of this use.
constexpr uint32_t switchSdInhibits(SwitchSdUse use)
{
    if (use == SwitchSdUse::kCommand || use == SwitchSdUse::kAbort)
        return kSdPresentCmdInhibit;
    return kSdPresentCmdInhibit | kSdPresentDatInhibit | kSdPresentDatActive | kSdPresentWriteActive |
           kSdPresentReadActive;
}

struct SwitchSdBus {
    uint32_t owed = 0;
    bool card = false;

    SwitchSdBusState state(uint32_t present, SwitchSdUse next) const
    {
        if (owed != 0)
            return SwitchSdBusState::kOwed;
        if (present & switchSdInhibits(next))
            return SwitchSdBusState::kHeld;
        const bool data = next == SwitchSdUse::kRead || next == SwitchSdUse::kWrite;
        if (data && (card || (next == SwitchSdUse::kWrite && !(present & kSdPresentDat0))))
            return SwitchSdBusState::kCardBusy;
        return SwitchSdBusState::kFree;
    }

    void issued(SwitchSdUse use)
    {
        owed = switchSdEnds(use);
        card = switchSdHoldsCard(use);
    }

    // state(), and the command marked issued when it is kFree.
    SwitchSdBusState claim(uint32_t present, SwitchSdUse next)
    {
        const SwitchSdBusState verdict = state(present, next);
        if (verdict == SwitchSdBusState::kFree)
            issued(next);
        return verdict;
    }

    // The interrupt status the command ended with and PRESENT_STATE read after it. The card is released once transfer
    // complete came and DAT0 is high.
    void ended(uint32_t status, uint32_t present)
    {
        owed &= ~status;
        if (card && (status & kSdTransferComplete) && (present & kSdPresentDat0))
            card = false;
    }

    bool idle() const
    {
        return owed == 0 && !card;
    }

    // A software reset of the CMD and DAT lines (or of the whole controller) leaves nothing owed.
    void reset()
    {
        owed = 0;
        card = false;
    }
};

#endif  // SWITCH_SD_BUS_H
