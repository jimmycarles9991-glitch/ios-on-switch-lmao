// SPDX-License-Identifier: BSD-2-Clause-Patent
// The memory controller's error interrupt (Tegra X1 TRM 18.5.12, registers 18.11.1.1-4): what its filter takes and the
// line its action prints. Register access is the caller's (Io: read(offset), write(offset, value)).
#pragma once
#include <stdarg.h>
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
// The ones that must reach the CPU: Linux's tegra210 mc driver unmasks exactly these (tegra210.c
// tegra210_mc_intmasks). DECERR_GENERALIZED_CARVEOUT (17) is unmasked too, but nothing says this MC implements it.
constexpr uint32_t kRequired = kErrors & ~(1u << 17);
// The kinds that panic: a memory client went where it may not (an SMMU page it was not given, an address with no DRAM, a
// carveout or security region). The MC refuses the access, but the DMA that asked for it was built on a wrong address,
// and the next one may land unchecked in another client's memory. INVALID_APB_ASID_UPDATE is a CPU's swgroup write the
// MC dropped (a secure ASID); the writer reads its register back, so it is only logged.
constexpr uint32_t kFatal = kErrors & ~(1u << 11);

// The kinds with capture registers of their own (TRM 18.5.12.2), laid out as MC_ERR_VPR_STATUS and MC_ERR_SEC_STATUS
// (18.11.1.92, .101): ADR_HI 21:20, RW 16, the client 7:0 (SEC's 6:0), the address's low 32 bits in the ADR register.
// VPR's and SEC's offsets are the TRM's; MTS's and the generalized carveout's are Linux's tegra20_mc_regs, the table its
// tegra210.c uses. Listed in the order take() prefers when several are pending.
struct OwnCapture {
    uint32_t bit, status, address, clientMask;
    const char *registers;
};

constexpr OwnCapture kOwnCaptures[] = {
    {1u << 13, 0x67c, 0x680, 0x7f, "ERR_SEC"},
    {1u << 12, 0x654, 0x658, 0xff, "ERR_VPR"},
    {1u << 16, 0x9b0, 0x9b4, 0xff, "ERR_MTS"},
    {1u << 17, 0xc00, 0xc04, 0xff, "ERR_GENERALIZED_CARVEOUT"},
};

struct Error {
    uint32_t intstatus, status, address;
    uint32_t ownBit, ownStatus, ownAddress;  // the first pending kind of kOwnCaptures and its capture; ownBit 0: none
};

// The interrupt's filter, interrupts off: the pending error bits, then the capture registers (they hold the first error
// until its bit is cleared, 18.11.1.3), MC_ERR_STATUS's and a pending kind's own, then those bits written back to
// clear them so the next error is captured. false: no error bit is pending.
template <class Io> bool take(Io &io, Error *out)
{
    const uint32_t pending = io.read(kIntStatus) & kErrors;
    if (!pending)
        return false;
    *out = {pending, io.read(kErrStatus), io.read(kErrAdr), 0, 0, 0};
    for (const OwnCapture &c : kOwnCaptures) {
        if (pending & c.bit) {
            out->ownBit = c.bit;
            out->ownStatus = io.read(c.status);
            out->ownAddress = io.read(c.address);
            break;
        }
    }
    io.write(kIntStatus, pending);
    return true;
}

inline const OwnCapture *ownCapture(uint32_t bit)
{
    for (const OwnCapture &c : kOwnCaptures) {
        if (c.bit == bit)
            return &c;
    }
    return nullptr;
}

constexpr uint32_t kDecerrEmem = 1u << 6, kInvalidSmmuPage = 1u << 10, kErrTypeInvalidSmmuPage = 6u;

// A deliberate fault's error (TegraSMMU.h switch_mc_errors_expect): an SMMU fault captured for `client` at an address
// in [low, high) below 4 GiB. On T210/T210B01 silicon, an unmapped IOVA below EMEM latches DECERR_EMEM alongside
// INVALID_SMMU_PAGE while MC_ERR_STATUS captures INVALID_SMMU_PAGE (type 6).
inline bool expected(const Error &e, uint32_t client, uint32_t low, uint32_t high)
{
    return (e.intstatus == kInvalidSmmuPage || e.intstatus == (kInvalidSmmuPage | kDecerrEmem)) &&
           ((e.status >> 28) & 7u) == kErrTypeInvalidSmmuPage && (e.status & 0xff) == client &&
           !((e.status >> 20) & 3) && e.address >= low && e.address < high;
}

inline void append(char *text, size_t size, size_t &used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

inline void append(char *text, size_t size, size_t &used, const char *format, ...)
{
    if (used >= size)
        return;
    va_list values;
    va_start(values, format);
    const int n = vsnprintf(text + used, size - used, format, values);
    va_end(values);
    used += n > 0 ? static_cast<size_t>(n) : 0;
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
// DECERR_EMEM, SECURITY_VIOLATION and INVALID_SMMU_PAGE are captured there (18.5.12.2); a kind with capture registers
// of its own (kOwnCaptures) adds its direction, client and address from them.
inline const char *describe(const Error &e, char *text, size_t size)
{
    char kinds[96];
    bitNames(e.intstatus, kinds, sizeof(kinds));
    constexpr uint32_t kCaptured = 1u << 6 | 1u << 8 | 1u << 10;
    const OwnCapture *own = ownCapture(e.ownBit);
    if (!(e.intstatus & kCaptured) && !own) {
        snprintf(text, size, "%s (not captured in MC_ERR_STATUS); INTSTATUS %#x", kinds, e.intstatus);
        return text;
    }
    size_t used = 0;
    text[0] = 0;
    append(text, size, used, "%s: ", kinds);
    if (e.intstatus & kCaptured) {
        const uint32_t s = e.status;
        const uint64_t address = static_cast<uint64_t>((s >> 20) & 3) << 32 | e.address;
        char page[64] = "";
        if (((s >> 28) & 7) == 6) {
            snprintf(page, sizeof(page), " (page readable %u writable %u nonsecure %u)", (s >> 27) & 1, (s >> 26) & 1,
                     (s >> 25) & 1);
        }
        append(text, size, used, "%s%s, a %s by %s (client %#x) at %#llx; ", errorType(s), page,
               (s >> 16) & 1 ? "write" : "read", client(s & 0xff), s & 0xff, static_cast<unsigned long long>(address));
    }
    if (own) {
        char ownKind[48];
        const uint32_t s = e.ownStatus;
        const uint64_t address = static_cast<uint64_t>((s >> 20) & 3) << 32 | e.ownAddress;
        append(text, size, used, "%s: a %s by %s (client %#x) at %#llx; ", bitNames(own->bit, ownKind, sizeof(ownKind)),
               (s >> 16) & 1 ? "write" : "read", client(s & own->clientMask), s & own->clientMask,
               static_cast<unsigned long long>(address));
    }
    append(text, size, used, "INTSTATUS %#x", e.intstatus);
    if (e.intstatus & kCaptured)
        append(text, size, used, " ERR_STATUS %#x ERR_ADR %#x", e.status, e.address);
    if (own)
        append(text, size, used, " %s_STATUS %#x %s_ADR %#x", own->registers, e.ownStatus, own->registers,
               e.ownAddress);
    return text;
}

// A fault storm, bounded per client: the first kFullLines errors of a client's run print in full and the rest are only
// counted, and the run ends with one count line naming why: displaced (kQuietErrors errors by other clients since its
// last, or kOpen other runs open), its swgroup's detach, or SC7 resetting the MC. Nothing here keeps time. The client
// is ERR_ID for the kinds MC_ERR_STATUS captures, one shared key (kUncaptured) for the others. The action's alone.
class Runs {
public:
    static constexpr uint32_t kFullLines = 4, kQuietErrors = 16, kUncaptured = 0x100;
    static constexpr unsigned kOpen = 8;

    enum class Cause : uint8_t {
        kDisplaced,
        kDetach,
        kSC7
    };

    struct Ended {
        uint32_t client, errors;
        Error last;
        Cause cause;
    };

    static uint32_t clientOf(const Error &e)
    {
        constexpr uint32_t kCaptured = 1u << 6 | 1u << 8 | 1u << 10;
        return e.intstatus & kCaptured ? e.status & 0xff : kUncaptured;
    }

    // An error: true when it prints in full. The runs it ends (quiet ones, or the least recent when kOpen are open)
    // with more errors than printed go to `ended` (room for kOpen); their count is returned in `endedCount`.
    bool error(const Error &e, Ended *ended, unsigned &endedCount)
    {
        endedCount = 0;
        const uint32_t client = clientOf(e);
        ++seen;
        Run *run = nullptr;
        Run *free = nullptr;
        Run *oldest = nullptr;
        for (Run &r : runs) {
            if (!r.open) {
                free = free ? free : &r;
                continue;
            }
            if (r.client == client) {
                run = &r;
                continue;
            }
            if (seen - r.lastAt > kQuietErrors) {
                end(r, Cause::kDisplaced, ended, endedCount);
                free = free ? free : &r;
                continue;
            }
            oldest = !oldest || r.lastAt < oldest->lastAt ? &r : oldest;
        }
        if (!run) {
            if (!free) {
                end(*oldest, Cause::kDisplaced, ended, endedCount);
                free = oldest;
            }
            run = free;
            *run = {client, 0, 0, {}, true};
        }
        ++run->errors;
        run->lastAt = seen;
        run->last = e;
        return run->errors <= kFullLines;
    }

    // The runs of the clients `match` takes end now, by `cause` (a detach, SC7); as error()'s `ended`.
    template <class Match> unsigned endWhere(Match match, Cause cause, Ended *ended)
    {
        unsigned count = 0;
        for (Run &r : runs) {
            if (r.open && match(r.client))
                end(r, cause, ended, count);
        }
        return count;
    }

private:
    struct Run {
        uint32_t client, errors;
        uint64_t lastAt;
        Error last;
        bool open;
    };

    static void end(Run &r, Cause cause, Ended *ended, unsigned &count)
    {
        if (r.errors > kFullLines)
            ended[count++] = {r.client, r.errors, r.last, cause};
        r.open = false;
    }

    Run runs[kOpen] = {};
    uint64_t seen = 0;
};

inline const char *causeName(Runs::Cause cause)
{
    switch (cause) {
    case Runs::Cause::kDisplaced:
        return "displaced";
    case Runs::Cause::kDetach:
        return "detach";
    case Runs::Cause::kSC7:
        return "SC7";
    }
    return "displaced";
}

// The count line of a run that printed only its first kFullLines errors.
inline const char *describeRun(const Runs::Ended &run, char *text, size_t size)
{
    char last[384];
    describe(run.last, last, sizeof(last));
    if (run.client == Runs::kUncaptured) {
        snprintf(text, size,
                 "%u errors of kinds MC_ERR_STATUS does not capture, the first %u printed, ended by %s; the last %s",
                 run.errors, Runs::kFullLines, causeName(run.cause), last);
    } else {
        snprintf(text, size, "%u errors by %s (client %#x), the first %u printed, ended by %s; the last %s", run.errors,
                 client(run.client), run.client, Runs::kFullLines, causeName(run.cause), last);
    }
    return text;
}
}  // namespace switch_mc
