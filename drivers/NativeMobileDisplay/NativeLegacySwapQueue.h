// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's view of the family's swap queue: in the class body (through NativePlanesDisplay.h, so that the
// driver's lines, which XNU's panic() carries, stay where they are). The queue is a tail queue in the framebuffer
// (swapQueueHead, swapQueueTail; each request linked through swapNext and swapPrevious) beside the current swap
// (swapQueueCurrent), at the offsets kits/SwitchABI's IOMobileFramebufferLegacy table gives for the IPSW kind.

// The oldest swap the family has queued and not made current yet, or none.
const OriginalLegacySwapRequest *pendingSwapRequest() const
{
    const OriginalLegacySwapRequest *request = nullptr;
    memcpy(&request, reinterpret_cast<const unsigned char *>(this) + swapQueueHead,
           sizeof(const OriginalLegacySwapRequest *));
    return request;
}

// The platform backend removes its pending head before completing it (the original completion takes the current swap).
// Taking the head moves the request a client already queued behind it up to the head (int-132: swap 3 arrived during swap
// 2's GPU copy, and taking only a lone head refused to retire swap 2, so every wait on it stayed busy). The queue's
// reference moves to the current swap for the family's completion.
bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request)
{
    auto *self = reinterpret_cast<unsigned char *>(this);
    const auto *bytes = reinterpret_cast<const unsigned char *>(request);
    const void *current = nullptr;
    const void *head = nullptr;
    const void *tail = nullptr;
    void *next = nullptr;
    const void *previous = nullptr;
    memcpy(&current, self + swapQueueCurrent, 8);
    memcpy(&head, self + swapQueueHead, 8);
    memcpy(&tail, self + swapQueueTail, 8);
    if (!request || current || head != request)
        return false;
    memcpy(&next, bytes + swapNext, 8);
    memcpy(&previous, bytes + swapPrevious, 8);
    if (previous != self + swapQueueHead)
        return false;
    if (next) {
        const void *link = self + swapQueueHead;
        memcpy(static_cast<unsigned char *>(next) + swapPrevious, &link, 8);
    } else if (tail != bytes + swapNext) {
        return false;
    }
    memcpy(self + swapQueueCurrent, &request, 8);
    if (next) {
        memcpy(self + swapQueueHead, &next, 8);
    } else {
        const void *empty = nullptr;
        const void *emptyTail = self + swapQueueHead;
        memcpy(self + swapQueueHead, &empty, 8);
        memcpy(self + swapQueueTail, &emptyTail, 8);
    }
    return true;
}

// The boot mode, where a platform backend would have put it: what getDisplayDimensions answers.
void installBootDimensions(uint32_t width, uint32_t height)
{
    setDisplayWidth(width);
    setDisplayHeight(height);
}
