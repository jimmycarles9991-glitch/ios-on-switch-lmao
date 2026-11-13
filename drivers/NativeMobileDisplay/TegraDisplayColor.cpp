// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "TegraDisplayColor.h"
#include "EDRDisplayFactor.h"
#include <string.h>

using namespace switch_color;

// DC A word indices (L4T drivers/video/tegra/dc/dc_reg.h). The CMU's LUTs and CSC are written straight to the unit, with
// no assembly copy (TegraDisplayColorSequence.h). DISP_COLOR_CONTROL is an assembly register. DBG_TIMING bit 15 is
// DBG_V_BLANK (TegraPanel.cpp reads it the same way).
static constexpr uint32_t kDcStateControl = 0x041, kDcCmuCsc = 0x32a, kDcCmuLut1 = 0x336, kDcCmuLut2 = 0x337,
                          kDcDispColorControl = 0x430, kDcDispDbgTiming = 0x4f6;
static constexpr uint32_t kGeneralActReq = 1u << 0, kGeneralUpdate = 1u << 8, kCmuEnable = 1u << 20,
                          kDbgVBlank = 1u << 15;
static constexpr uint32_t kLocationAccessibility = 2, kLocationHarmony = 9;

// NativeLegacyDisplay.cpp: FRAME_END reaches the CPU while a hold is out.
extern "C" void native_legacy_frame_end_hold(void);
extern "C" void native_legacy_frame_end_release(void);

#if DEVELOPMENT || DEBUG
// The first matrices IOMobileFramebuffer hands over, in thousandths, rows as received: what Core Animation's colour
// requests become before the CSC (one line each, the first kLoggedMatrices of a boot).
static void logMatrix(const char *call, uint32_t location, const Matrix &m)
{
    static constexpr uint32_t kLoggedMatrices = 24;
    static uint32_t logged = 0;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) >= kLoggedMatrices)
        return;
    long long v[9];
    for (int i = 0; i < 9; ++i)
        v[i] = static_cast<long long>(static_cast<__int128>(m.m[i / 3][i % 3]) * 1000 / kOne);
    IOLog("SwitchDisplay: colour %s %u: %lld %lld %lld / %lld %lld %lld / %lld %lld %lld\n", call, location, v[0], v[1],
          v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
}

// The first CSCs those requests program, signed, 256 being one, KRR KGR KBR (red out) first, with the three 16.16 scales
// folded into them: what the panel's channels get.
static void logCsc(const Tables &tables, const Inputs &in)
{
    static constexpr uint32_t kLoggedCscs = 24;
    static uint32_t logged = 0;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) >= kLoggedCscs)
        return;
    int v[kCscWords];
    for (unsigned i = 0; i < kCscWords; ++i) {
        const int word = tables.csc[i];
        v[i] = word >= 0x200 ? word - 0x400 : word;
    }
    IOLog("SwitchDisplay: colour CSC: %d %d %d / %d %d %d / %d %d %d (correction %#x, scale %#x, edr %#x%s)\n", v[0],
          v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], in.correction, in.accessibilityScale, in.edrScale,
          in.blank ? ", blank" : "");
}
#endif

// The object comes zeroed from IOMallocType, its member initializers not run.
bool TegraDisplayColor::init()
{
    inputs = Inputs();
    identityGamma(inputs.gamma);
    for (unsigned point = 0; point < kGammaPoints; ++point) {
        for (unsigned channel = 0; channel < 3; ++channel)
            gammaTable[channel * kGammaPoints + point] = gammaIdentity(point);
    }
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

void TegraDisplayColor::attach(SwitchMMIOWords registers)
{
    colorControl = registers[kDcDispColorControl] & ~kCmuEnable;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    dc = registers;
    sequence = Sequence();
    sequence.pendingDirty = true;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
}

void TegraDisplayColor::detach()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    if (dc && sequence.enableAsked)
        writeEnable(false);
    dc = SwitchMMIOWords();
    sequence = Sequence();
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
        const bool current = seen == generation;
        const bool hold = current && static_cast<bool>(dc) && !holding;
#if DEVELOPMENT || DEBUG
        const bool cscChanged = current && memcmp(pending.csc, tables.csc, sizeof(tables.csc)) != 0;
#endif
        if (current) {
            pending = tables;
            sequence.pendingDirty = true;
            holding = holding || hold;
        }
        IOSimpleLockUnlockEnableInterrupt(lock, state);
        if (hold)
            native_legacy_frame_end_hold();
#if DEVELOPMENT || DEBUG
        if (cscChanged)
            logCsc(tables, snapshot);
#endif
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
    uint32_t word = 0;
    memcpy(&word, static_cast<const uint8_t *>(table) + index * sizeof(word), sizeof(word));
    return word;
}

// LUT2 is one table for all three channels; a table whose channels differ goes into the windows' palettes instead.
IOReturn TegraDisplayColor::setGammaTable(const void *table)
{
    if (!table)
        return kIOReturnBadArgument;
    const bool perChannel = switch_color::channelsDiffer(table);
    uint8_t curve[256];
    uint32_t words[switch_color::kPaletteWords];
    if (perChannel) {
        identityGamma(curve);
        switch_color::paletteOf(table, words);
    } else {
        for (unsigned i = 0; i < 256; ++i)
            curve[i] = switch_color::gammaByte(gammaWord(table, i));
    }
    const IOReturn status = change(lock, generation, [&] {
        memcpy(gammaTable, table, sizeof(gammaTable));
        memcpy(inputs.gamma, curve, sizeof(curve));
        if (perChannel)
            memcpy(palette, words, sizeof(palette));
        paletteDirty = perChannel;
        paletteShown = paletteShown && perChannel;
        paletteOn = perChannel;
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
#if DEVELOPMENT || DEBUG
    logMatrix("setMatrix", location, value);
#endif
    const IOReturn status = change(lock, generation, [&] {
        (location == kLocationHarmony ? inputs.harmony : inputs.accessibility) = value;
        return kIOReturnSuccess;
    });
    commit();
    return status;
}

IOReturn TegraDisplayColor::setContrast(const void *contrast)
{
    uint32_t bits = 0;
    uint32_t value = 0;
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
#if DEVELOPMENT || DEBUG
    logMatrix("swapSetColorMatrix", location, value);
#endif
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

void TegraDisplayColor::setEdrFactor(float factor)
{
    const uint32_t scale = switch_edr_display_scale(factor);
    const IOReturn status = change(lock, generation, [&] {
        if (inputs.edrScale == scale)
            return kIOReturnNotReady;
        inputs.edrScale = scale;
        return kIOReturnSuccess;
    });
    if (status == kIOReturnSuccess)
        commit();
}

void TegraDisplayColor::writeTables(const Tables &tables, const Tables *written)
{
    for (unsigned i = 0; i < kLut1Entries; ++i) {
        if (!written || written->lut1[i] != tables.lut1[i])
            dc[kDcCmuLut1] = i | static_cast<uint32_t>(tables.lut1[i]) << 16;
    }
    for (unsigned i = 0; i < kCscWords; ++i) {
        if (!written || written->csc[i] != tables.csc[i])
            dc[kDcCmuCsc + i] = tables.csc[i];
    }
    for (unsigned k = 0; k < kLut2Entries; ++k) {
        if (!written || written->lut2[k] != tables.lut2[k])
            dc[kDcCmuLut2] = k | static_cast<uint32_t>(tables.lut2[k]) << 16;
    }
}

void TegraDisplayColor::writeCsc(const Tables &tables, const Tables &written)
{
    for (unsigned i = 0; i < kCscWords; ++i) {
        if (written.csc[i] != tables.csc[i])
            dc[kDcCmuCsc + i] = tables.csc[i];
    }
}

void TegraDisplayColor::writeEnable(bool enable)
{
    dc[kDcDispColorControl] = colorControl | (enable ? kCmuEnable : 0);
    dc[kDcStateControl] = kGeneralUpdate;
    dc[kDcStateControl] = kGeneralActReq;
}

void TegraDisplayColor::frameEndFilter()
{
    IOSimpleLockLock(lock);
    if (dc) {
        const Tables *written = sequence.activeValid ? &active : nullptr;
        const auto actRequestOut = [this] {
            return (dc[kDcStateControl] & kGeneralActReq) != 0u;
        };
        const auto inBlank = [this] {
            return (dc[kDcDispDbgTiming] & kDbgVBlank) != 0u;
        };
        const Write write = atFrameEnd(sequence, pending, written, actRequestOut, inBlank);
        if (write == Write::kAll) {
            writeTables(pending, written);
        } else if (write == Write::kCsc) {
            writeCsc(pending, active);
        }
        if (write != Write::kNothing)
            active = pending;
        if (paletteDirty && inBlank()) {
            writePalettes();
            paletteDirty = false;
            paletteShown = paletteOn;
        }
    }
    IOSimpleLockUnlock(lock);
}

void TegraDisplayColor::writePalettes()
{
    for (const uint32_t base : switch_color::kPaletteBase) {
        for (uint32_t i = 0; i < switch_color::kPaletteWords; ++i)
            dc[base + i] = palette[i];
    }
}

uint32_t TegraDisplayColor::windowOptions()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    const uint32_t options = paletteOn && paletteShown ? switch_color::kCpEnable : 0;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    return options;
}

// mayRequest false: a window A request is out, and the flip, rotation and probe checks read GENERAL_ACT_REQ as part of
// it; the enable waits for a frame end with none out. The decision and the write share the lock with the filter, which
// must never see a bypass it reads as latched before its GENERAL_ACT_REQ is written.
void TegraDisplayColor::frameEndAction(bool mayRequest)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    const bool want = pending.enabled;
    const uint32_t reasons = pending.reasons;
    const bool release = holding && settled(sequence, want);
    holding = holding && !release;
    const Enable enable = dc ? atFrameEndAction(sequence, want, mayRequest) : Enable::kKeep;
    if (enable != Enable::kKeep) {
        writeEnable(enable == Enable::kOn);
        enableWritten(sequence, enable == Enable::kOn);
    }
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    if (release)
        native_legacy_frame_end_release();
    if (enable == Enable::kKeep)
        return;
    if (enable == Enable::kOff) {
        if (announced && !want)
            IOLog("SwitchDisplay: colour CMU off\n");
        announced = announced && want;
        return;
    }
    if (announced)
        return;
    announced = ((reasons & ~kReasonBlank) != 0u);
    if (!announced)
        return;
    IOLog("SwitchDisplay: colour CMU on:%s%s%s%s%s\n", reasons & kReasonMatrix ? " matrix" : "",
          reasons & kReasonScale ? " scale" : "", reasons & kReasonRemap ? " remap" : "",
          reasons & kReasonContrast ? " contrast" : "", reasons & kReasonGamma ? " gamma" : "");
}

void TegraDisplayColor::restoreAfterPowerUp()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    sequence = Sequence();
    if (dc && paletteOn) {
        writePalettes();
        paletteDirty = false;
        paletteShown = true;
    }
    if (dc && pending.enabled) {
        writeTables(pending, nullptr);
        active = pending;
        sequence.activeValid = sequence.enableAsked = true;
        dc[kDcDispColorControl] = colorControl | kCmuEnable;
    }
    IOSimpleLockUnlockEnableInterrupt(lock, state);
}

void TegraDisplayColor::bypassForPanic(SwitchMMIOWords registers)
{
    registers[kDcDispColorControl] = registers[kDcDispColorControl] & ~kCmuEnable;
}
