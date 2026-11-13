// SPDX-License-Identifier: BSD-2-Clause-Patent
// The first client frame once the data migration's console hold drops (switch_console_hold, kern_exec.c). While the
// console held the screen, kickHeld retired every swap undrawn, so Core Animation believes its last one is on screen
// and an idle compositor sends nothing more: the screen kept the console until something on it changed. XNU's hold
// release calls native_legacy_console_released, and the next kick with no swap queued takes the family's current swap
// (the one Core Animation believes shown, its surfaces still the family's) through the ordinary present, once. It is
// never retired again: it is no longer in the family's queue, and the first swap the compositor sends after it untakes
// it. Only while swaps retire at the latch (direct scanout): the other retirement (finishTransfer's retireSwap) would
// refuse a swap that is not pending and leave the next one waiting. Included in the class body after
// SwitchRetireDisplay.h; under the gate.

const OriginalLegacySwapRequest *consoleReshown = nullptr;
bool consoleReleased = false;

// kickOnce's request: the queued one, or after the release the current one until a kick has taken it.
const OriginalLegacySwapRequest *consoleReleasedFrame(const OriginalLegacySwapRequest *request)
{
    if (request || !consoleReleased) {
        if (request && consoleReshown) {
            untake(consoleReshown);
            removeValidated(consoleReshown);
            consoleReshown = nullptr;
        }
        consoleReleased = consoleReleased && !request;
        return request;
    }
    const OriginalLegacySwapRequest *current = nullptr;
    memcpy(&current, reinterpret_cast<const unsigned char *>(this) + swapQueueCurrent, sizeof(current));
    if (!latchRetire || !current || (current == consoleReshown && seqOf(current))) {
        consoleReleased = false;
        return nullptr;
    }
    if (current != consoleReshown) {
        consoleReshown = current;
        addValidated(current, mach_absolute_time());
        IOLog("SwitchDisplay: the console released the screen: the compositor's current frame goes to it\n");
    }
    return current;
}

void consoleReleasedKick()
{
    consoleReleased = true;
    kickSwapQueue();
}
