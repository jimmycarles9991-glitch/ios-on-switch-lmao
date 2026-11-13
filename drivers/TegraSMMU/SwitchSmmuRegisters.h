// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraSMMUAttachments.h"

namespace switch_smmu {
constexpr uint32_t kPtbAsid = 0x1c, kPtbData = 0x20, kTlbFlush = 0x30, kPtcFlush = 0x34;
constexpr uint32_t kAsidEnable = 1u << 31, kTlbAsidMatch = 1u << 31;

template <class Registers> void flush(Registers &io, uint32_t asid)
{
    io.write(kPtcFlush, 0);
    (void)io.read(kPtbAsid);
    io.write(kTlbFlush, kTlbAsidMatch | (asid << 24));
    (void)io.read(kPtbAsid);
}

template <class Registers> int program(Registers &io, const tegra_smmu_attachment &a, char *line, size_t size)
{
    io.write(kPtbAsid, a.asid);
    io.write(kPtbData, a.ptb);
    (void)io.read(kPtbAsid);
    io.write(kPtbAsid, a.asid);
    const uint32_t directory = io.read(kPtbData);
    if (directory != a.ptb) {
        snprintf(line, size, "ASID %u directory did not take (PTB_DATA %#x, wrote %#x)", a.asid, directory, a.ptb);
        return -1;
    }
    flush(io, a.asid);
    io.write(a.translationReg, io.read(a.translationReg) | a.translationBits);
    if ((io.read(a.translationReg) & a.translationBits) != a.translationBits) {
        snprintf(line, size, "swgroup translation bits did not take (MC %#x)", a.translationReg);
        return -1;
    }
    io.write(a.asidReg, kAsidEnable | a.asid);
    const uint32_t selected = io.read(a.asidReg);
    if (selected != (kAsidEnable | a.asid)) {
        snprintf(line, size, "swgroup ASID did not take (MC %#x = %#x)", a.asidReg, selected);
        return -1;
    }
    flush(io, a.asid);
    return 0;
}

template <class Registers>
int restore(Registers &io, const tegra_smmu_attachments &attachments, uint32_t asid, uint32_t ptb, uint32_t asidReg,
            char *line, size_t size)
{
    for (uint32_t i = 0; i < attachments.count; ++i) {
        const auto &a = attachments.entry[i];
        if (a.asidReg != asidReg)
            continue;
        if (a.asid == asid && a.ptb == ptb)
            return program(io, a, line, size);
        break;
    }
    snprintf(line, size, "no retained directory for ASID %u swgroup %#x", asid, asidReg);
    return -1;
}
}
