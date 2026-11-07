// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "TegraDisplayColor.h"
#include <string.h>

using namespace switch_color;

// DC A word indices (L4T drivers/video/tegra/dc/dc_reg.h). The CMU's LUTs and CSC are written straight to the unit, with
// no assembly copy, so they change only in the vertical blank. DISP_COLOR_CONTROL is an assembly register.
static constexpr uint32_t kDcStateControl = 0x041, kDcCmuCsc = 0x32a, kDcCmuLut1 = 0x336, kDcCmuLut2 = 0x337,
                          kDcDispColorControl = 0x430;
static constexpr uint32_t kGeneralActReq = 1u << 0, kGeneralUpdate = 1u << 8, kCmuEnable = 1u << 20;
static constexpr uint32_t kLocationAccessibility = 2, kLocationHarmony = 9;

// NativeLegacyDisplay.cpp: FRAME_END reaches the CPU while a hold is out.
extern "C" void native_legacy_frame_end_hold(void);
extern "C" void native_legacy_frame_end_release(void);

// The object comes zeroed from IOMallocType, its member initializers not run.
bool TegraDisplayColor::init()
{
    inputs = Inputs();
    identityGamma(inputs.gamma);
    for (unsigned point = 0; point < kGammaPoints; ++point)
        for (unsigned channel = 0; channel < 3; ++channel)
            gammaTable[channel * kGammaPoints + point] = gammaIdentity(point);
    build(inputs, pending);
    lock = IOSimpleLockAlloc();
    return lock != nullptr;
}

void TegraDisplayColor::free()
{
    if (lock)
        IOSimpleLockFree(lock);
    lock = nullptr;
}

void TegraDisplayColor::attach(volatile uint32_t *registers)
{
    colorControl = registers[kDcDispColorControl] & ~kCmuEnable;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    dc = registers;
    activeValid = hwEnabled = false;
    pendingDirty = true;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
}

void TegraDisplayColor::detach()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    if (dc && hwEnabled)
        writeEnable(false);
    dc = nullptr;
    activeValid = hwEnabled = false;
    const bool held = holding;
    holding = false;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    if (held)
        native_legacy_frame_end_release();
}

// The tables are built outside the lock from a snapshot; a setter that ran meanwhile builds them again, so the last
// tables handed to the filter always reflect the last inputs.
void TegraDisplayColor::commit()
{
    Tables tables;
    for (;;) {
        IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
        const Inputs snapshot = inputs;
        const uint32_t seen = generation;
        IOSimpleLockUnlockEnableInterrupt(lock, state);
        build(snapshot, tables);
        state = IOSimpleLockLockDisableInterrupt(lock);
        const bool current = seen == generation, hold = current && dc && !holding;
        if (current) {
            pending = tables;
            pendingDirty = true;
            holding = holding || hold;
        }
        IOSimpleLockUnlockEnableInterrupt(lock, state);
        if (hold)
            native_legacy_frame_end_hold();
        if (current)
            return;
    }
}

template <typename Change> static IOReturn change(IOSimpleLock *lock, uint32_t &generation, Change body)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    const IOReturn status = body();
    if (status == kIOReturnSuccess)
        ++generation;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    return status;
}

IOReturn TegraDisplayColor::getGammaTable(void *table)
{
    if (!table)
        return kIOReturnBadArgument;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    memcpy(table, gammaTable, sizeof(gammaTable));
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    return kIOReturnSuccess;
}

static uint32_t gammaWord(const void *table, unsigned index)
{
    uint32_t word;
    memcpy(&word, static_cast<const uint8_t *>(table) + index * sizeof(word), sizeof(word));
    return word;
}

// LUT2 is one table for all three channels: a table whose channels differ cannot be shown.
IOReturn TegraDisplayColor::setGammaTable(const void *table)
{
    if (!table)
        return kIOReturnBadArgument;
    for (unsigned point = 0; point < kGammaPoints; ++point) {
        const uint32_t red = gammaWord(table, point);
        if (red != gammaWord(table, kGammaPoints + point) || red != gammaWord(table, 2 * kGammaPoints + point))
            return kIOReturnUnsupported;
    }
    uint8_t curve[256];
    for (unsigned i = 0; i < 256; ++i) {
        const uint32_t word = gammaWord(table, i), value = (word > kGammaMax ? kGammaMax : word) + 2;
        curve[i] = uint8_t(value >> 2 > 255 ? 255 : value >> 2);
    }
    const IOReturn status = change(lock, generation, [&] {
        memcpy(gammaTable, table, sizeof(gammaTable));
        memcpy(inputs.gamma, curve, sizeof(curve));
        return kIOReturnSuccess;
    });
    commit();
    return status;
}

IOReturn TegraDisplayColor::getMatrix(uint32_t location, void *matrix)
{
    if (!matrix || (location != kLocationHarmony && location != kLocationAccessibility))
        return kIOReturnBadArgument;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    memcpy(matrix, location == kLocationHarmony ? &inputs.harmony : &inputs.accessibility, sizeof(Matrix));
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    return kIOReturnSuccess;
}

IOReturn TegraDisplayColor::setMatrix(uint32_t location, const void *matrix)
{
    if (!matrix || (location != kLocationHarmony && location != kLocationAccessibility))
        return kIOReturnBadArgument;
    Matrix value;
    memcpy(&value, matrix, sizeof(value));
    const IOReturn status = change(lock, generation, [&] {
        (location == kLocationHarmony ? inputs.harmony : inputs.accessibility) = value;
        return kIOReturnSuccess;
    });
    commit();
    return status;
}

IOReturn TegraDisplayColor::setContrast(const void *contrast)
{
    uint32_t bits = 0, value = 0;
    if (!contrast)
        return kIOReturnBadArgument;
    memcpy(&bits, contrast, sizeof(bits));
    if (!floatToUnity16(bits, &value))
        return kIOReturnBadArgument;
    const IOReturn status = change(lock, generation, [&] {
        inputs.contrast = value;
        return kIOReturnSuccess;
    });
    commit();
    return status;
}

IOReturn TegraDisplayColor::setColorRemapMode(uint32_t mode)
{
    if (mode >= kRemapModes)
        return kIOReturnBadArgument;
    uint32_t previous = 0;
    const IOReturn status = change(lock, generation, [&] {
        previous = inputs.remapMode;
        inputs.remapMode = mode;
        return kIOReturnSuccess;
    });
    if (previous != mode)
        IOLog("SwitchDisplay: colour remap mode %u -> %u\n", previous, mode);
    commit();
    return status;
}

IOReturn TegraDisplayColor::getColorRemapMode(uint32_t *mode)
{
    if (!mode)
        return kIOReturnBadArgument;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    *mode = inputs.remapMode;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    return kIOReturnSuccess;
}

IOReturn TegraDisplayColor::setBrightnessCorrection(uint32_t correction)
{
    const IOReturn status = change(lock, generation, [&] {
        inputs.correction = correction;
        return kIOReturnSuccess;
    });
    commit();
    return status;
}

// The matrix is pre-offsets, the 3x3, post-offsets (15 S31.32 words); null asks for identity. The CSC has no offsets,
// and QuartzCore always sends them zero.
IOReturn TegraDisplayColor::swapSetColorMatrix(const void *matrix, uint32_t location, uint32_t)
{
    if (location != kLocationAccessibility)
        return kIOReturnBadArgument;
    Matrix value = kIdentity;
    if (matrix) {
        Q32 words[15];
        memcpy(words, matrix, sizeof(words));
        if (words[0] || words[1] || words[2] || words[12] || words[13] || words[14])
            return kIOReturnUnsupported;
        memcpy(&value, &words[3], sizeof(value));
    }
    const IOReturn status = change(lock, generation, [&] {
        inputs.accessibility = value;
        return kIOReturnSuccess;
    });
    commit();
    return status;
}

void TegraDisplayColor::setBlank(bool blank)
{
    const IOReturn status = change(lock, generation, [&] {
        if (inputs.blank == blank)
            return kIOReturnNotReady;
        inputs.blank = blank;
        return kIOReturnSuccess;
    });
    if (status == kIOReturnSuccess)
        commit();
}

IOReturn TegraDisplayColor::setAccessibilityScale(uint32_t scale)
{
    const IOReturn status = change(lock, generation, [&] {
        inputs.accessibilityScale = scale;
        return kIOReturnSuccess;
    });
    commit();
    return status;
}

void TegraDisplayColor::writeTables(const Tables &tables, const Tables *written)
{
    for (unsigned i = 0; i < kLut1Entries; ++i)
        if (!written || written->lut1[i] != tables.lut1[i])
            dc[kDcCmuLut1] = i | uint32_t(tables.lut1[i]) << 16;
    for (unsigned i = 0; i < kCscWords; ++i)
        if (!written || written->csc[i] != tables.csc[i])
            dc[kDcCmuCsc + i] = tables.csc[i];
    for (unsigned k = 0; k < kLut2Entries; ++k)
        if (!written || written->lut2[k] != tables.lut2[k])
            dc[kDcCmuLut2] = k | uint32_t(tables.lut2[k]) << 16;
}

void TegraDisplayColor::writeEnable(bool enable)
{
    dc[kDcDispColorControl] = colorControl | (enable ? kCmuEnable : 0);
    dc[kDcStateControl] = kGeneralUpdate;
    dc[kDcStateControl] = kGeneralActReq;
}

// Tables for a disabled CMU are never written: the ones it scans stay until the disable latches.
void TegraDisplayColor::frameEndFilter()
{
    IOSimpleLockLock(lock);
    if (dc && pendingDirty) {
        if (pending.enabled) {
            writeTables(pending, activeValid ? &active : nullptr);
            active = pending;
            activeValid = true;
        }
        pendingDirty = false;
    }
    IOSimpleLockUnlock(lock);
}

// mayRequest false: a window A request is out, and the flip, rotation and probe checks read GENERAL_ACT_REQ as part of
// it; the enable waits for a frame end with none out.
void TegraDisplayColor::frameEndAction(bool mayRequest)
{
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    const bool want = pending.enabled, ready = !pendingDirty && activeValid, attached = dc != nullptr;
    const uint32_t reasons = pending.reasons;
    const bool settled = !pendingDirty && want == hwEnabled, release = holding && settled;
    holding = holding && !settled;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    if (release)
        native_legacy_frame_end_release();
    if (!attached || !mayRequest || want == hwEnabled || (want && !ready))
        return;
    writeEnable(want);
    hwEnabled = want;
    if (!want) {
        if (announced)
            IOLog("SwitchDisplay: colour CMU off\n");
        announced = false;
        return;
    }
    announced = reasons & ~kReasonBlank;
    if (!announced)
        return;
    IOLog("SwitchDisplay: colour CMU on:%s%s%s%s%s\n", reasons & kReasonMatrix ? " matrix" : "",
          reasons & kReasonScale ? " scale" : "", reasons & kReasonRemap ? " remap" : "",
          reasons & kReasonContrast ? " contrast" : "", reasons & kReasonGamma ? " gamma" : "");
}

void TegraDisplayColor::restoreAfterPowerUp()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    hwEnabled = activeValid = false;
    if (dc && pending.enabled) {
        writeTables(pending, nullptr);
        active = pending;
        activeValid = hwEnabled = true;
        pendingDirty = false;
        dc[kDcDispColorControl] = colorControl | kCmuEnable;
    }
    IOSimpleLockUnlockEnableInterrupt(lock, state);
}

void TegraDisplayColor::bypassForPanic(volatile uint32_t *registers)
{
    registers[kDcDispColorControl] = registers[kDcDispColorControl] & ~kCmuEnable;
}
