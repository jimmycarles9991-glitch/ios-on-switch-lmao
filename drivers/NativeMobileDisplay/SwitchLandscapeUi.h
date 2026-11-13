// SPDX-License-Identifier: BSD-2-Clause-Patent
// The window the UI's flips go to (A; B in landscape, SwitchLandscapeWindows.h), by the latched composition's mode:
// in NativeLegacyDisplay's class body after SwitchLandscapeState.h.

Window uiWindow() const
{
    return switch_landscape::windowsOf(landscapeOn()).ui;
}

// The UI window's word `index` in the DC_WIN (0x700) / DC_WINBUF (0x800) numbering: window A through WINDOW_HEADER, as
// every flip before landscape wrote it; window B through its own aperture.
SwitchMMIOWord uiWord(uint32_t index)
{
    const Window w = uiWindow();
    if (w == switch_landscape::kA)
        return dc[index];
    return dc[index >= 0x800 ? switch_landscape::bufferBase(w) + (index - 0x800)
                             : switch_landscape::windowBase(w) + (index - 0x700)];
}

uint32_t uiActive(uint32_t index)
{
    if (uiWindow() == switch_landscape::kA)
        return windowActive(index);
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    const uint32_t value = uiWord(index);
    dc[kDcStateAccess] = access;
    return value;
}

uint32_t uiUpdate() const
{
    return switch_landscape::update(uiWindow());
}

uint32_t uiRequest() const
{
    return switch_landscape::actRequest(uiWindow());
}

uint32_t uiUnderflow() const
{
    return switch_landscape::underflow(uiWindow());
}

char uiName() const
{
    return switch_landscape::nameOf(uiWindow());
}
