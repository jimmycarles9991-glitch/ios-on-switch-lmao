// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window A's CP_ENABLE follows TegraDisplayColor's per-channel gamma (TegraDisplayPalette.h): at a DC A frame end with
// no window A request out (the CMU enable's rule), pitchOptions and window A's assembly WIN_OPTIONS take the bit, which
// latches at the next boundary; every later write of window A's options starts from pitchOptions. Windows B and C take
// it for 32-bit RGB layers where native_planes::imageOf makes their options.
void paletteFrameEnd(bool mayRequest)
{
    const uint32_t want = color->windowOptions();
    if (!pitchSaved || !mayRequest || want == (pitchOptions & switch_color::kCpEnable))
        return;
    withdrawCompletionFlip();
    pitchOptions = (pitchOptions & ~switch_color::kCpEnable) | want;
    switch_window_palette::setOptions(want);
    dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcWindowHeader] = kWindowASelect;
    dc[kWinOptions] = (dc[kWinOptions] & ~switch_color::kCpEnable) | want;
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = kWinAUpdate;
    dc[kDcStateControl] = kWinAActReq;
    IOLog("SwitchDisplay: colour palette %s: a per-channel gamma table %s the windows' palettes\n", want ? "on" : "off",
          want ? "goes through" : "no longer uses");
}
