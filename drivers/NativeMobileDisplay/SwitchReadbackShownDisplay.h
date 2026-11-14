// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: the GPU driver's live scanout read-back (TegraGM20BReadbackABI.h) re-reads, at each offer, the frame
// the offer before it showed. A frame window A shows with no offer (a direct frame flipped from the kick, a CPU, VIC or
// blank frame) is not one it knows, so it forgets the last (tegra_gm20b_scanout_bypassed): otherwise the next read
// would compare a page Core Animation already has back, and count its next frame as a change on screen. Included in
// the class body; under the gate.

#if SWITCH_FRAME_TIMELINE
static inline bool readbackOffered = false;

// A frame accepted by tegra_gm20b_scanout_copy, taken next.
void readbackOffer()
{
    readbackOffered = true;
}

// noteTaken's step: a swap taken without an offer.
void readbackTaken()
{
    if (!readbackOffered)
        tegra_gm20b_scanout_bypassed();
    readbackOffered = false;
}
#else
void readbackOffer()
{}

void readbackTaken()
{}
#endif
