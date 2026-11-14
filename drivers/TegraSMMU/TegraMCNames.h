// SPDX-License-Identifier: BSD-2-Clause-Patent
// Names the memory controller's error and hot-reset lines print: the hot-reset clients of MC_CLIENT_HOTRESET_CTRL and
// CTRL_1 (Tegra X1 TRM 18.11.1.56 and 18.11.1.114) and the global client IDs of MC_ERR_STATUS ERR_ID (18.11.1.3).
#pragma once
#include <stdint.h>

namespace switch_mc {
constexpr uint32_t kHotResetCtrl = 0x200, kHotResetCtrl1 = 0x970;

inline const char *hotResetClient(uint32_t ctrl, uint32_t bit)
{
    static const char *const kCtrl[32] = {
        "AFI",   "AVPC",  "DC",    "DCB",   nullptr, nullptr, "HC",    "HDA",     "ISP2",      "MPCORE",   nullptr,
        "NVENC", nullptr, nullptr, "PPCS",  "SATA",  nullptr, "VI",    "VIC",     "XUSB_HOST", "XUSB_DEV", nullptr,
        "TSEC",  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "SDMMC1A", "SDMMC2A",   "SDMMC3A",
    };
    static const char *const kCtrl1[32] = {
        "SDMMC4A", "ISP2B", "GPU",   nullptr, nullptr, "NVDEC", "APE",
        "SE",      "NVJPG", nullptr, nullptr, "AXIAP", "ETR",   "TSECB",
    };
    const unsigned index = static_cast<unsigned>(__builtin_ctz(bit | 0x80000000u));
    const char *name = nullptr;
    if (ctrl == kHotResetCtrl)
        name = kCtrl[index];
    else if (ctrl == kHotResetCtrl1)
        name = kCtrl1[index];
    return name ? name : "an unnamed client";
}

inline const char *client(uint32_t id)
{
    struct Entry {
        uint8_t id;
        const char *name;
    };

    static const Entry kClients[] = {
        {0x00, "csr_ptcr (SMMU page-table cache)"},
        {0x01, "csr_display0a (display A)"},
        {0x02, "csr_display0ab (display B)"},
        {0x03, "csr_display0b (display A)"},
        {0x04, "csr_display0bb (display B)"},
        {0x05, "csr_display0c (display A)"},
        {0x06, "csr_display0cb (display B)"},
        {0x0e, "csr_afir (PCIe)"},
        {0x0f, "csr_avpcarm7r (BPMP)"},
        {0x10, "csr_displayhc (display A cursor)"},
        {0x11, "csr_displayhcb (display B cursor)"},
        {0x15, "csr_hdar (HDA)"},
        {0x16, "csr_host1xdmar (host1x)"},
        {0x17, "csr_host1xr (host1x)"},
        {0x1c, "csr_nvencsrd (NVENC)"},
        {0x1d, "csr_ppcsahbdmar (AHB DMA)"},
        {0x1e, "csr_ppcsahbslvr (AHB slave)"},
        {0x1f, "csr_satar (SATA)"},
        {0x27, "csr_mpcorer (CPU)"},
        {0x2b, "csw_nvencswr (NVENC)"},
        {0x31, "csw_afiw (PCIe)"},
        {0x32, "csw_avpcarm7w (BPMP)"},
        {0x35, "csw_hdaw (HDA)"},
        {0x36, "csw_host1xw (host1x)"},
        {0x39, "csw_mpcorew (CPU)"},
        {0x3b, "csw_ppcsahbdmaw (AHB DMA)"},
        {0x3c, "csw_ppcsahbslvw (AHB slave)"},
        {0x3d, "csw_sataw (SATA)"},
        {0x44, "csr_ispra (ISP)"},
        {0x46, "csw_ispwa (ISP)"},
        {0x47, "csw_ispwb (ISP)"},
        {0x4e, "csr_isprab (ISP B)"},
        {0x50, "csw_ispwab (ISP B)"},
        {0x51, "csw_ispwbb (ISP B)"},
        {0x54, "csr_tsecsrd (TSEC)"},
        {0x55, "csw_tsecswr (TSEC)"},
        {0x56, "csr_a9avpscr (BPMP cache)"},
        {0x57, "csw_a9avpscw (BPMP cache)"},
        {0x58, "csr_gpusrd (GPU)"},
        {0x59, "csw_gpuswr (GPU)"},
        {0x5a, "csr_displayt (display)"},
        {0x60, "csr_sdmmcra (SDMMC1)"},
        {0x61, "csr_sdmmcraa (SDMMC2)"},
        {0x62, "csr_sdmmcr (SDMMC3)"},
        {0x63, "csr_sdmmcrab (SDMMC4)"},
        {0x64, "csw_sdmmcwa (SDMMC1)"},
        {0x65, "csw_sdmmcwaa (SDMMC2)"},
        {0x66, "csw_sdmmcw (SDMMC3)"},
        {0x67, "csw_sdmmcwab (SDMMC4)"},
        {0x6c, "csr_vicsrd (VIC)"},
        {0x6d, "csw_vicswr (VIC)"},
        {0x72, "csw_viw (VI)"},
        {0x73, "csr_displayd (display)"},
        {0x78, "csr_nvdecsrd (NVDEC)"},
        {0x79, "csw_nvdecswr (NVDEC)"},
        {0x7a, "csr_aper (APE)"},
        {0x7b, "csw_apew (APE)"},
        {0x7e, "csr_nvjpgsrd (NVJPG)"},
        {0x7f, "csw_nvjpgswr (NVJPG)"},
        {0x80, "csr_sesrd (SE)"},
        {0x81, "csw_seswr (SE)"},
        {0x82, "csr_axiapr (AXI AP)"},
        {0x83, "csw_axiapw (AXI AP)"},
        {0x84, "csr_etrr (ETR)"},
        {0x85, "csw_etrw (ETR)"},
        {0x86, "csr_tsecsrdb (TSEC B)"},
        {0x87, "csw_tsecswrb (TSEC B)"},
        {0x88, "csr_gpusrd2 (GPU)"},
        {0x89, "csw_gpuswr2 (GPU)"},
    };
    for (const Entry &entry : kClients)
        if (entry.id == id)
            return entry.name;
    return "an unlisted client";
}

// MC_ERR_STATUS ERR_TYPE (bits 30:28).
inline const char *errorType(uint32_t status)
{
    switch ((status >> 28) & 7) {
    case 2:
        return "DECERR_EMEM (no DRAM at the address)";
    case 3:
        return "SECURITY_TRUSTZONE";
    case 4:
        return "SECURITY_CARVEOUT";
    case 6:
        return "INVALID_SMMU_PAGE";
    default:
        return "type unknown";
    }
}
}  // namespace switch_mc
