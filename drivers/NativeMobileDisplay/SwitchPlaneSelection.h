// SPDX-License-Identifier: BSD-2-Clause-Patent
static bool planeSame(const PlaneSet &a, const PlaneSet &b)
{
    if (a.landscape != b.landscape)
        return false;
    for (unsigned window = 0; window != 2; ++window) {
        const auto &first = a.window[window];
        const auto &second = b.window[window];
        if (first.on != second.on)
            return false;
        if (!first.on)
            continue;
        const auto &x = first.image;
        const auto &y = second.image;
        if (first.slot != second.slot || first.depth != second.depth || x.colorDepth != y.colorDepth ||
            x.position != y.position || x.size != y.size || x.prescaled != y.prescaled || x.dda != y.dda ||
            x.hInitial != y.hInitial || x.vInitial != y.vInitial || x.lineStride != y.lineStride ||
            x.hOffset != y.hOffset || x.vOffset != y.vOffset || x.surfaceKind != y.surfaceKind ||
            x.options != y.options || x.byteSwap != y.byteSwap || x.start[0] != y.start[0] ||
            x.start[1] != y.start[1] || x.matrix != y.matrix || x.fullRange != y.fullRange)
            return false;
    }
    return true;
}

uint32_t planeBuildingSlots = 0;

// Retried at the next frame end, the current frame kept: a slot still scanned (-11), a surface busy with a publication
// or a CPU writer (-16), a representation replaced under the acquire (STALE), a conversion queued (-115; its completion
// kicks too). Any other error cannot place this layer: it is dropped and the rest of the swap shows.
static bool planeRetry(int slot)
{
    return slot == -11 || slot == -16 || slot == -115 || slot == SWITCH_GPU_BACKING_STALE;
}

void planeDecide(const OriginalLegacySwapRequest *request, PlaneDecision &d)
{
    d = {};
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(request);
    const uint32_t updated = native_planes::wordAt(bytes, swapUpdated);
    if (stats && ((updated ^ native_planes::wordAt(bytes, swapReady)) & native_planes::kLayerBits))
        ++stPlaneUnready;
    OSObject *effective[native_planes::kLayers];
    memcpy(effective, bytes + swapEffectiveSurfaces, sizeof(effective));
    uint32_t width = 0;
    uint32_t height = 0;
    getDisplayDimensions(&width, &height);
    bool present[native_planes::kLayers] = {};
    native_planes::Layer layers[native_planes::kLayers];
    for (unsigned i = 0; i < native_planes::kLayers; ++i) {
        PlaneLayerRecord &r = planeLayers[i];
        OSObject *e = effective[i] && effective[i]->metaCast("IOSurface") ? effective[i] : nullptr;
        if (updated & (1u << i)) {
            r.surface = e;
            if (e)
                r.layer = planeLayerOf(bytes, i, e);
        } else if (e != r.surface) {
            // The family's inherited surface is not the one recorded (a swap retired unshown before its kick): a full-frame
            // BGRA one is the UI as it is; anything else cannot be placed and is not shown.
            r.surface = nullptr;
            if (e) {
                native_planes::Layer l;
                l.surface = planeSurfaceOf(e);
                l.kind = native_planes::kindOf(l.surface.format, l.surface.planes);
                l.src = l.dst = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
                if (native_planes::isUi(l, width, height)) {
                    r.surface = e;
                    r.layer = l;
                } else if (stats) {
                    ++stPlaneDropped;
                }
            }
        }
        present[i] = r.surface != nullptr;
        layers[i] = r.layer;
    }
    const native_planes::Roles roles = native_planes::rolesOf(present, layers, width, height);
    if (stats)
        stPlaneDropped += roles.extra;
    bool any = false;
    for (const bool p : present)
        any = any || p;
    d.blank = !any;
    d.ui = roles.ui >= 0 ? planeLayers[roles.ui].surface : nullptr;
    d.uiNew = d.blank || ((d.ui != nullptr) &&
                          (((updated & (1u << static_cast<uint32_t>(roles.ui))) != 0u) || d.ui != planeShownUi));
    if (any && roles.ui < 0 && stats)
        ++stPlaneDropped;  // no UI layer: window A keeps the last one
    if (planeLandscapeDecide(roles, width, height, d))
        return;
    PlaneSet want = {};
    switch_display::Overlay wanted[2] = {};
    const int selected[2] = {roles.plane, roles.second};

    struct BuildingSlots {
        uint32_t &slots;

        ~BuildingSlots()
        {
            slots = 0;
        }
    } building = {planeBuildingSlots};

    for (unsigned index = 0; index != 2; ++index) {
        if (selected[index] < 0)
            continue;
        const auto &layer = planeLayers[selected[index]];
        auto &overlay = wanted[index];
        bool resolved = false;
        const int slot =
            planesOn ? planeNativeSlotFor(layer.surface, layer.layer, width, height, overlay.image, resolved) : -19;
        if (slot == -2)
            continue;
        if (planeRetry(slot)) {
            d.waitSlot = true;
            return;
        }
        if (slot < 0) {
            if (stats)
                ++stPlaneDropped;
            continue;
        }
        overlay.on = true;
        overlay.slot = slot;
        overlay.depth = index + 1;
        planeBuildingSlots |= 1u << unsigned(slot);
    }
    if (planeComposeWaits(wanted, selected, width, height)) {
        d.waitSlot = true;
        return;
    }
    const switch_display::WindowState windows[2] = {planeWindowState(switch_landscape::kB),
                                                    planeWindowState(switch_landscape::kC)};
    const auto assignment = switch_display::assign(wanted, windows);
    for (unsigned index = 0; index != 2; ++index) {
        auto &overlay = wanted[index];
        if (assignment.convert & (1u << index)) {
            const auto &layer = planeLayers[selected[index]];
            const int slot = planeConvertSlot(overlay.slot, layer.layer, width, height, overlay.image, request);
            if (planeRetry(slot)) {
                d.waitSlot = true;
                return;
            }
            if (slot < 0) {
                if (stats)
                    ++stPlaneDropped;
                overlay = {};
            } else {
                overlay.slot = slot;
                planeBuildingSlots |= 1u << unsigned(slot);
            }
        }
        want.window[assignment.window[index]] = overlay;
    }
    d.planeChange = !planeSame(want, planeTarget);
    d.set = want;
}
