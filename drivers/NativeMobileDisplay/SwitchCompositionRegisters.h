// SPDX-License-Identifier: BSD-2-Clause-Patent
bool planeWindowRegistersMatch(Window w, const switch_display::Overlay &p)
{
    using namespace native_planes;
    const auto win = dc + switch_landscape::windowBase(w);
    const auto buf = dc + switch_landscape::bufferBase(w);
    if (!p.on)
        return !(win[kOptions] & native_planes::kWinEnable);
    const auto &m = p.image;
    const uint64_t base = uint64_t(planeSlotIova(p.slot)) - planeSlots[p.slot].low;
    const uint32_t blend = m.yuv ? kBlendBypass | p.depth : kBlendPremultiplied | p.depth;
    return win[kOptions] == m.options && win[kColorDepth] == m.colorDepth && win[kPosition] == m.position &&
           win[kSize] == m.size && win[kPrescaled] == m.prescaled && win[kDdaIncrement] == m.dda &&
           win[kHInitialDda] == m.hInitial && win[kVInitialDda] == m.vInitial && win[kBlendLayer] == blend &&
           win[kBlendMatch] == (m.yuv ? 0u : kBlendOver) && win[kBlendNomatch] == (m.yuv ? 0u : kBlendOver) &&
           win[kLineStride] == m.lineStride && buf[kStartAddr] == uint32_t(base + m.start[0]) && !buf[kStartAddrHi] &&
           !buf[kStartAddrHiU] && buf[kStartAddrU] == (m.yuv ? uint32_t(base + m.start[1]) : 0) &&
           buf[kSurfaceKind] == m.surfaceKind && buf[kHOffset] == m.hOffset && buf[kVOffset] == m.vOffset;
}

bool planeRegistersMatch(const PlaneSet &p)
{
    return planeWindowRegistersMatch(planeWindowOf(p, 0), p.window[0]) &&
           planeWindowRegistersMatch(planeWindowOf(p, 1), p.window[1]);
}

int planeScannedSet()
{
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    const int ids[] = {planeLatch.armed, planeLatch.superseded, planeLatch.active};
    int match = native_planes::PlaneLatch::kUnknown;
    for (int id : ids) {
        if (id >= 0 && planeRegistersMatch(planeSetOf(id))) {
            match = id;
            break;
        }
    }
    dc[kDcStateAccess] = access;
    return match;
}
