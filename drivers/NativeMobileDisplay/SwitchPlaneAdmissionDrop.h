// SPDX-License-Identifier: BSD-2-Clause-Patent
// A set of two overlays the display's memory admission refuses, with no latch coming to restage it: the heavier
// overlay goes, and the other stays when admission takes it alone; only then do both go. Two windows can exceed the
// 1600 MHz EMC rate's isochronous budget (SwitchWindowCaps.h's YUV fraction at its extreme) where one never does.
void planeDropUntilAdmitted(PlaneSet &p)
{
    if (p.window[0].on && p.window[1].on) {
        const unsigned heavier =
            planeAdmissionDemand(p.window[1].image) > planeAdmissionDemand(p.window[0].image) ? 1 : 0;
        PlaneSet one = p;
        planeDropOverlays(one, 1u << heavier);
        if (planeReserveBandwidth(one)) {
            p = one;
            return;
        }
    }
    planeDropOverlays(p, 3);
}
