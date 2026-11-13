// SPDX-License-Identifier: BSD-2-Clause-Patent
// The display-buffer probe's check (SwitchNVDECProbeABI.h). What NVDEC 2.0 writes in disp_output_mode 2 or 3 is unknown,
// so every buffer is bounded for the most it could be: the job without its display buffer, read as mode 1, must pass
// tegra_nvdec_check (picture slots and filter rows of 16-bit samples), and each display plane must hold the current
// picture's plane in 16-bit samples. SET_DISPLAY_BUF_SIZE stays refused ("value method"): no driver we can read sets it,
// so its unit is unknown and no bound on it can be proven.
#pragma once
#include "TegraNVDECCheck.h"

// `plain` and `plainAvail` are scratch for the job without its display buffer. NULL, or why the probe is refused.
inline const char *switch_nvdec_check_display_probe(const TegraNVDECSubmit *s, const uint64_t *avail,
                                                    TegraNVDECSubmit *plain, uint64_t *plainAvail)
{
    if (s->codec != TEGRA_NVDEC_CODEC_HEVC || s->setup_bytes != sizeof(nvdec_hevc_pic_s) ||
        s->entry_count > TEGRA_NVDEC_MAX_ENTRIES)
        return "probe: not an HEVC submit";
    const auto *p = reinterpret_cast<const nvdec_hevc_pic_s *>(s->setup);
    if (p->bit_depth_luma != 10 || (p->disp_output_mode != 2 && p->disp_output_mode != 3))
        return "probe: not Main 10 in mode 2 or 3";
    *plain = *s;
    plain->entry_count = 0;
    uint64_t display[2] = {0, 0};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry &e = s->entries[i];
        const int plane = e.method == NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET     ? 0
                          : e.method == NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET ? 1
                                                                             : -1;
        if (plane < 0) {
            plainAvail[plain->entry_count] = avail[i];
            plain->entries[plain->entry_count++] = e;
            continue;
        }
        if (e.kind != TEGRA_NVDEC_ENTRY_RELOC || (e.offset & 0xff))
            return "probe: display buffer relocation";
        if (display[plane])
            return "probe: repeated display buffer";
        display[plane] = avail[i] ? avail[i] : 1;
    }
    if (!display[0] || !display[1])
        return "probe: display buffer not bound";
    reinterpret_cast<nvdec_hevc_pic_s *>(plain->setup)->disp_output_mode = 1;
    if (const char *why = tegra_nvdec_check(plain, plainAvail))
        return why;
    const uint64_t luma = uint64_t(p->framestride[0]) * 2 * ((p->pic_height_in_luma_samples + 63u) & ~63u);
    if (display[0] < luma || display[1] < luma / 2)
        return "probe: display plane smaller than the 16-bit picture's";
    return nullptr;
}
