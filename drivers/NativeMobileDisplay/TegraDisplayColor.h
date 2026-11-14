// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOMobileFramebuffer's colour contract, as J120AP's AppleCLCD keeps it, on DC A's colour management unit: True Tone and
// Night Shift (CoreBrightness through QuartzCore: SetMatrix location 9, SetBrightnessCorrection), the accessibility
// filters (SwapSetColorMatrix location 2, SetParameter 3 for Reduce White Point, SetColorRemapMode for Classic Invert and
// Grayscale), SetContrast and the gamma table. The CMU sits after window blending, so window B's video gets the same
// transform. Only DC A's volatile registers change: CMU LUT1/LUT2/CSC and DISP_COLOR_CONTROL's CMU_ENABLE. With nothing
// asked for the CMU is bypassed and the picture is the one hekate set up.
//
// The IOMobileFramebuffer calls come from any thread and serialize on their own lock, which nothing at a frame end takes.
// The tables change at frame ends (the filter) only where TegraDisplayColorSequence.h allows, and the enable bit latches
// at a frame boundary through GENERAL_ACT_REQ from the work loop (the action); TegraDisplayColorPublish.h hands the
// tables to the filter and the state between the three.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOReturn.h>
#include "TegraDisplayColorMath.h"
#include "TegraDisplayColorPublish.h"
#include "TegraDisplayPalette.h"
#include "../../kits/SwitchMMIO/SwitchMMIOWords.h"

class TegraDisplayColor {
public:
    bool init();
    void free();

    // Under the work-loop gate, with DC A's interrupt not yet attached, then disabled (no filter runs): DC A mapped,
    // then unmapped.
    void attach(SwitchMMIOWords registers);
    void detach();

    IOReturn getGammaTable(void *table);
    IOReturn setGammaTable(const void *table);
    IOReturn getMatrix(uint32_t location, void *matrix);
    IOReturn setMatrix(uint32_t location, const void *matrix);
    IOReturn setContrast(const void *contrast);
    IOReturn setColorRemapMode(uint32_t mode);
    IOReturn getColorRemapMode(uint32_t *mode);
    IOReturn setBrightnessCorrection(uint32_t correction);
    IOReturn swapSetColorMatrix(const void *matrix, uint32_t location, uint32_t flags);
    IOReturn setAccessibilityScale(uint32_t scale);
    // The family's SetParameter 6 (EDRDisplayFactor.h): the whole output times `factor` from the next frame end.
    void setEdrFactor(float factor);

    // CP_ENABLE once a per-channel gamma table is in the windows' palettes (TegraDisplayPalette.h), else 0. No lock.
    uint32_t windowOptions();

    // Debug kernels: the setters' lock's takes since init (counted under it), for the display's stats line.
    uint32_t lockTakes() const
    {
        return __atomic_load_n(&takes, __ATOMIC_RELAXED);
    }

    // Under the gate: a blank swap's black, from the next frame on, and its end at the first flip latched after it.
    void setBlank(bool blank);

    // DC A's frame end: the filter (primary interrupt context) writes changed tables, the action (work-loop gate) moves
    // the enable bit. Each change holds the frame end (native_legacy_frame_end_hold) until both are done. Neither locks.
    void frameEndFilter();
    void frameEndAction(bool mayRequest);

    // Under the gate, after tegra_display_power_up() reset DC A, before the resume's GENERAL_ACT_REQ: the tables and the
    // enable bit back, latching with that request.
    void restoreAfterPowerUp();

    // A panic's screen without the transform (a colour filter or a low white point could hide its text): the CMU bypassed
    // with the caller's next GENERAL_ACT_REQ. Stores only, from any context, the controller clocked.
    static void bypassForPanic(SwitchMMIOWords registers);

private:
    struct Platform {
        static void hold();
        static void release();
        static void deviceWritesDone();
    };

    void commit(bool paletteChanged);
    template <typename Change> IOReturn change(Change body);
    void noteLock();

    IOLock *lock = nullptr;  // the setters' (and attach, detach, restoreAfterPowerUp): never at a frame end
    uint32_t takes = 0;
    SwitchMMIOWords dc;
    switch_color::ColourPublication<Platform> unit;
    bool announced = false;  // the last enable was logged (a blank alone is not); the action's
    switch_color::Inputs inputs;
    uint32_t gammaTable[switch_color::kGammaWords];
    uint32_t palette[switch_color::kPaletteWords];
    bool paletteOn = false;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    uint16_t loggedCsc[switch_color::kCscWords];
#endif
};
