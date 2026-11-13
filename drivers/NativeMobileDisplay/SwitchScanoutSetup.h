// SPDX-License-Identifier: BSD-2-Clause-Patent

bool scanoutQualification = false;

void initializeScanout()
{
    if (primary && !captureDisplayMemory())
        IOLog("SwitchDisplay: the display timing cannot be read: no memory admission, overlays off\n");
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    int qualification = 0;
    PE_parse_boot_argn("switch-display-qualification", &qualification, sizeof(qualification));
    scanoutQualification = qualification != 0;
    if (scanoutQualification)
        return;
#endif
    probeRan = planeProbeRan = true;
    if (primary && dc)
        SwitchDisplayPriority::initialize(dc);
    if (!primary || !doubleBuffer || !probeTables.memory || !SWITCH_PORTRAIT_NATIVE)
        return;
    if (otherWindowEnabled()) {
        IOLog("SwitchDisplay: scanout domain refused: firmware left another window active\n");
        return;
    }
    bool ready = probeDomain.init(probeTables.va, probeTables.pa, kProbeTables);
    for (unsigned i = 0; ready && i < ring.buffers; ++i)
        ready = probeDomain.identity(surfaceAddress(static_cast<int>(i)), kNativeBlockBytes, false);
    if (ready && tapeAttached)
        ready = probeDomain.identity(tapeAddress, kTapeBytes, false);
    ready = ready && probeDomain.reserve(kProbeSlot, kSlots * kSlotSpan) &&
            probeDomain.reserve(kPlaneSlot0, static_cast<uint64_t>(kPlaneSlots) * kPlaneSlotSpan);
    if (!ready) {
        IOLog("SwitchDisplay: scanout domain refused: insufficient page tables\n");
        return;
    }
    cleanDomain();
    char why[160];
    if (tegra_smmu_attach(TEGRA_SMMU_ASID_DISPLAY, probeDomain.ptb(), TEGRA_SMMU_DC_ASID,
                          TEGRA_SMMU_TRANSLATION_ENABLE_0,
                          kDisplay0aTranslation | kDisplay0bTranslation | kDisplay0cTranslation, why, sizeof(why))) {
        IOLog("SwitchDisplay: scanout domain refused: %s\n", why);
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        planeBlendSaved[i] = dc[kWinABase + native_planes::kBlendLayer + i];
        gPanicRestore.blend[i] = planeBlendSaved[i];
    }
    planeStartRegisters();
    chainPanicHook();
    gPanicRestore.planes = true;
    directAttached = planesAttached = latchRetire = true;
    directOn = gpuCopy && gpuDoneSource;
    __atomic_store_n(&planesOn, true, __ATOMIC_RELEASE);
}
