// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "TegraDisplayColor.h"
#include "EDRDisplayFactor.h"
#include <string.h>

using namespace switch_color;

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

// Under the lock.
void TegraDisplayColor::noteLock()
{
#if DEVELOPMENT || DEBUG
    __atomic_store_n(&takes, takes + 1, __ATOMIC_RELAXED);
#endif
}

void TegraDisplayColor::Platform::hold()
{
    native_legacy_frame_end_hold();
}

void TegraDisplayColor::Platform::release()
{
    native_legacy_frame_end_release();
}

// The action's CMU_ENABLE and GENERAL_ACT_REQ, read back, have reached DC A before the filter may see the bypass.
void TegraDisplayColor::Platform::deviceWritesDone()
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

// The object comes zeroed from IOMallocType, its member initializers not run.
bool TegraDisplayColor::init()
{
    inputs = Inputs();
    identityGamma(inputs.gamma);
    for (unsigned point = 0; point < kGammaPoints; ++point) {
        for (unsigned channel = 0; channel < 3; ++channel)
            gammaTable[channel * kGammaPoints + point] = gammaIdentity(point);
    }
    build(inputs, unit.initial().tables);
    unit.initialPublished();
    lock = IOLockAlloc();
    return lock != nullptr;
}

void TegraDisplayColor::free()
{
    if (lock)
        IOLockFree(lock);
    lock = nullptr;
}

void TegraDisplayColor::attach(SwitchMMIOWords registers)
{
    IOLockLock(lock);
    noteLock();
    dc = registers;
    unit.reset(registers[kDcDispColorControl] & ~kCmuEnable);
    IOLockUnlock(lock);
}

void TegraDisplayColor::detach()
{
    IOLockLock(lock);
    noteLock();
    if (dc)
        unit.stop(dc);
    dc = SwitchMMIOWords();
    IOLockUnlock(lock);
}

// Under the lock: the inputs' tables go to the filter as the newest snapshot.
void TegraDisplayColor::commit(bool paletteChanged)
{
    Snapshot &next = unit.spare();
    build(inputs, next.tables);
    memcpy(next.palette, palette, sizeof(palette));
#if DEVELOPMENT || DEBUG
    if (memcmp(loggedCsc, next.tables.csc, sizeof(loggedCsc)) != 0) {
        memcpy(loggedCsc, next.tables.csc, sizeof(loggedCsc));
        logCsc(next.tables, inputs);
    }
#endif
    unit.publish(paletteOn, paletteChanged, static_cast<bool>(dc));
}

template <typename Change> IOReturn TegraDisplayColor::change(Change body)
{
    IOLockLock(lock);
    noteLock();
    const IOReturn status = body();
    IOLockUnlock(lock);
    return status;
}

IOReturn TegraDisplayColor::getGammaTable(void *table)
{
    if (!table)
        return kIOReturnBadArgument;
    return change([&] {
        memcpy(table, gammaTable, sizeof(gammaTable));
        return kIOReturnSuccess;
    });
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
    return change([&] {
        memcpy(gammaTable, table, sizeof(gammaTable));
        memcpy(inputs.gamma, curve, sizeof(curve));
        if (perChannel)
            memcpy(palette, words, sizeof(palette));
        paletteOn = perChannel;
        commit(perChannel);
        return kIOReturnSuccess;
    });
}

IOReturn TegraDisplayColor::getMatrix(uint32_t location, void *matrix)
{
    if (!matrix || (location != kLocationHarmony && location != kLocationAccessibility))
        return kIOReturnBadArgument;
    return change([&] {
        memcpy(matrix, location == kLocationHarmony ? &inputs.harmony : &inputs.accessibility, sizeof(Matrix));
        return kIOReturnSuccess;
    });
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
    return change([&] {
        (location == kLocationHarmony ? inputs.harmony : inputs.accessibility) = value;
        commit(false);
        return kIOReturnSuccess;
    });
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
    return change([&] {
        inputs.contrast = value;
        commit(false);
        return kIOReturnSuccess;
    });
}

IOReturn TegraDisplayColor::setColorRemapMode(uint32_t mode)
{
    if (mode >= kRemapModes)
        return kIOReturnBadArgument;
    uint32_t previous = 0;
    const IOReturn status = change([&] {
        previous = inputs.remapMode;
        inputs.remapMode = mode;
        commit(false);
        return kIOReturnSuccess;
    });
    if (previous != mode)
        IOLog("SwitchDisplay: colour remap mode %u -> %u\n", previous, mode);
    return status;
}

IOReturn TegraDisplayColor::getColorRemapMode(uint32_t *mode)
{
    if (!mode)
        return kIOReturnBadArgument;
    return change([&] {
        *mode = inputs.remapMode;
        return kIOReturnSuccess;
    });
}

IOReturn TegraDisplayColor::setBrightnessCorrection(uint32_t correction)
{
    return change([&] {
        inputs.correction = correction;
        commit(false);
        return kIOReturnSuccess;
    });
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
    return change([&] {
        inputs.accessibility = value;
        commit(false);
        return kIOReturnSuccess;
    });
}

void TegraDisplayColor::setBlank(bool blank)
{
    (void)change([&] {
        if (inputs.blank != blank) {
            inputs.blank = blank;
            commit(false);
        }
        return kIOReturnSuccess;
    });
}

IOReturn TegraDisplayColor::setAccessibilityScale(uint32_t scale)
{
    return change([&] {
        inputs.accessibilityScale = scale;
        commit(false);
        return kIOReturnSuccess;
    });
}

void TegraDisplayColor::setEdrFactor(float factor)
{
    const uint32_t scale = switch_edr_display_scale(factor);
    (void)change([&] {
        if (inputs.edrScale != scale) {
            inputs.edrScale = scale;
            commit(false);
        }
        return kIOReturnSuccess;
    });
}

void TegraDisplayColor::frameEndFilter()
{
    if (dc)
        unit.frameEnd(dc);
}

uint32_t TegraDisplayColor::windowOptions()
{
    return unit.windowOptions();
}

// mayRequest false: a window A request is out, and the flip, rotation and probe checks read GENERAL_ACT_REQ as part of
// it; the enable waits for a frame end with none out.
void TegraDisplayColor::frameEndAction(bool mayRequest)
{
    if (!dc)
        return;
    const auto step = unit.frameEndAction(dc, mayRequest);
    if (step.enable == Enable::kKeep)
        return;
    if (step.enable == Enable::kOff) {
        if (announced && !step.want)
            IOLog("SwitchDisplay: colour CMU off\n");
        announced = announced && step.want;
        return;
    }
    if (announced)
        return;
    const uint32_t reasons = step.reasons;
    announced = (reasons & ~kReasonBlank) != 0u;
    if (!announced)
        return;
    IOLog("SwitchDisplay: colour CMU on:%s%s%s%s%s\n", reasons & kReasonMatrix ? " matrix" : "",
          reasons & kReasonScale ? " scale" : "", reasons & kReasonRemap ? " remap" : "",
          reasons & kReasonContrast ? " contrast" : "", reasons & kReasonGamma ? " gamma" : "");
}

void TegraDisplayColor::restoreAfterPowerUp()
{
    IOLockLock(lock);
    noteLock();
    if (dc)
        unit.restore(dc);
    IOLockUnlock(lock);
}

void TegraDisplayColor::bypassForPanic(SwitchMMIOWords registers)
{
    registers[kDcDispColorControl] = registers[kDcDispColorControl] & ~kCmuEnable;
}
