// SPDX-License-Identifier: BSD-2-Clause-Patent
// The memory controller's error interrupt (Tegra X1 TRM 18.5.12, registers 18.11.1.1-4): what its filter takes and the
// line its action prints. Register access is the caller's (Io: read(offset), write(offset, value)).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "TegraMCNames.h"

namespace switch_mc {
constexpr uint32_t kIntStatus = 0x0, kIntMask = 0x4, kErrStatus = 0x8, kErrAdr = 0xc;

// INTSTATUS/INTMASK bits that are errors: DECERR_EMEM 6, SECURITY_VIOLATION 8, INVALID_SMMU_PAGE 10,
// INVALID_APB_ASID_UPDATE 11, DECERR_VPR 12, SECERR_SEC 13, DECERR_MTS 16, DECERR_GENERALIZED_CARVEOUT 17.
// ARBITRATION_EMEM (9) is a performance warning that also fires while the EMC back-pressures traffic for a clock change
// (18.5.12.2): it stays masked.
constexpr uint32_t kErrors = 1u << 6 | 1u << 8 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 13 | 1u << 16 | 1u << 17;

struct Error {
    uint32_t intstatus, status, address;
};

// The interrupt's filter, interrupts off: the pending error bits, then the capture registers (they hold the first error
// until its bit is cleared, 18.11.1.3), then those bits written back to clear them so the next error is captured.
// false: no error bit is pending.
template <class Io> bool take(Io &io, Error *out)
{
    const uint32_t pending = io.read(kIntStatus) & kErrors;
    if (!pending)
        return false;
    out->intstatus = pending;
    out->status = io.read(kErrStatus);
    out->address = io.read(kErrAdr);
    io.write(kIntStatus, pending);
    return true;
}

inline const char *bitNames(uint32_t intstatus, char *text, size_t size)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } kBits[] = {
        {1u << 6, "DECERR_EMEM"},        {1u << 8, "SECURITY_VIOLATION"},
        {1u << 10, "INVALID_SMMU_PAGE"}, {1u << 11, "INVALID_APB_ASID_UPDATE"},
        {1u << 12, "DECERR_VPR"},        {1u << 13, "SECERR_SEC"},
        {1u << 16, "DECERR_MTS"},        {1u << 17, "DECERR_GENERALIZED_CARVEOUT"},
    };

    size_t used = 0;
    text[0] = 0;
    for (const auto &b : kBits) {
        if (!(intstatus & b.bit) || used >= size)
            continue;
        const int n = snprintf(text + used, size - used, "%s%s", used ? "+" : "", b.name);
        used += n > 0 ? static_cast<size_t>(n) : 0;
    }
    return text;
}

// One line: the INTSTATUS kinds, then what MC_ERR_STATUS captured: the error type, the direction, the client (ERR_ID,
// 7:0) and the address (ERR_ADR_HI 21:20 above ERR_ADR); an SMMU fault adds the page's permissions (27:25). Only
// DECERR_EMEM, SECURITY_VIOLATION and INVALID_SMMU_PAGE are captured there (18.5.12.2); the others have capture
// registers of their own.
inline const char *describe(const Error &e, char *text, size_t size)
{
    char kinds[96];
    constexpr uint32_t kCaptured = 1u << 6 | 1u << 8 | 1u << 10;
    if (!(e.intstatus & kCaptured)) {
        snprintf(text, size, "%s (not captured in MC_ERR_STATUS); INTSTATUS %#x",
                 bitNames(e.intstatus, kinds, sizeof(kinds)), e.intstatus);
        return text;
    }
    const uint32_t s = e.status;
    const uint64_t address = static_cast<uint64_t>((s >> 20) & 3) << 32 | e.address;
    char page[64] = "";
    if (((s >> 28) & 7) == 6)
        snprintf(page, sizeof(page), " (page readable %u writable %u nonsecure %u)", (s >> 27) & 1, (s >> 26) & 1,
                 (s >> 25) & 1);
    snprintf(text, size, "%s: %s%s, a %s by %s (client %#x) at %#llx; INTSTATUS %#x ERR_STATUS %#x ERR_ADR %#x",
             bitNames(e.intstatus, kinds, sizeof(kinds)), errorType(s), page, (s >> 16) & 1 ? "write" : "read",
             client(s & 0xff), s & 0xff, static_cast<unsigned long long>(address), e.intstatus, s, e.address);
    return text;
}
}  // namespace switch_mc
