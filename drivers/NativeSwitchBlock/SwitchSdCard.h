// SPDX-License-Identifier: BSD-2-Clause-Patent
// SD card register decoders, data-command framing and extension-register helpers (NativeSwitchBlock.cpp), kept here as
// plain functions so the host can test every bit (test/sd_card_test.cpp).
//
// Sources: SD Physical Layer Simplified Specification 6.00 (4.10.2 Table 4-44 SD Status, 4.17 Cache, 5.2/5.3 CID/CSD,
// 5.6 Table 5-17/5-23 SCR CMD_SUPPORT, 5.7 extension registers CMD48/CMD49 and General Information, 5.8.2 Table 5-30
// Performance Enhancement register set). Linux drivers/mmc/core/sd.c (mmc_sd_read_ext_regs, sd_parse_ext_reg*) reads
// the same layouts.
#pragma once

#include <stdint.h>

// ---- Register fields ---------------------------------------------------------------------------------------------
// Bits [hi:lo] of a `bits`-bit register as the card sends it, most significant byte first (CID/CSD 16 bytes, SCR 8, SD
// Status 64). At most 32 bits.
inline uint32_t switchSdBits(const uint8_t *reg, unsigned bits, unsigned hi, unsigned lo)
{
    uint32_t value = 0;
    for (unsigned bit = hi + 1; bit-- > lo;)
        value = value << 1 | ((static_cast<uint32_t>(reg[(bits - 1 - bit) / 8]) >> (bit % 8)) & 1u);
    return value;
}

// An R2 response (CMD2 CID, CMD9 CSD) from the SDHCI response registers (RESPONSE0..3 at 0x10..0x1c, `rep[0]` first) into
// the register's 16 bytes, most significant first. SDHCI stores R2's bits [127:8] (the CRC byte stripped) in REP[119:0],
// so register byte k (k < 15) is REP byte 14 - k; byte 15 (CRC7 and the end bit) is not kept and reads 0.
inline void switchSdR2(const uint32_t rep[4], uint8_t out[16])
{
    for (unsigned k = 0; k < 15; ++k) {
        const unsigned j = 14 - k;
        out[k] = static_cast<uint8_t>(rep[j / 4] >> (8 * (j % 4)));
    }
    out[15] = 0;
}

struct SwitchSdCid {
    uint8_t mid;          // manufacturer
    char oid[3], pnm[6];  // OEM id, product name (NUL-terminated; non-printable bytes as '?')
    uint8_t prv;          // product revision, BCD n.m
    uint32_t psn;         // serial
    uint16_t year;        // manufacturing date
    uint8_t month;
};

inline SwitchSdCid switchSdParseCid(const uint8_t cid[16])
{
    SwitchSdCid c = {};
    const auto printable = [](uint32_t ch) {
        return static_cast<char>(ch >= 0x20 && ch < 0x7f ? ch : '?');
    };
    c.mid = static_cast<uint8_t>(switchSdBits(cid, 128, 127, 120));
    c.oid[0] = printable(switchSdBits(cid, 128, 119, 112));
    c.oid[1] = printable(switchSdBits(cid, 128, 111, 104));
    for (unsigned i = 0; i < 5; ++i)
        c.pnm[i] = printable(switchSdBits(cid, 128, 103 - 8 * i, 96 - 8 * i));
    c.prv = static_cast<uint8_t>(switchSdBits(cid, 128, 63, 56));
    c.psn = switchSdBits(cid, 128, 55, 24);
    c.year = static_cast<uint16_t>(2000 + switchSdBits(cid, 128, 19, 12));
    c.month = static_cast<uint8_t>(switchSdBits(cid, 128, 11, 8));
    return c;
}

// Common SD manufacturer ids (as Linux's quirk tables and card databases name them); others print as hex.
inline const char *switchSdMaker(uint8_t mid)
{
    switch (mid) {
    case 0x01:
        return "Panasonic";
    case 0x02:
        return "Toshiba/Kioxia";
    case 0x03:
        return "SanDisk";
    case 0x1b:
        return "Samsung";
    case 0x1d:
        return "ADATA";
    case 0x27:
        return "Phison";
    case 0x28:
        return "Lexar";
    case 0x31:
        return "Silicon Power";
    case 0x41:
        return "Kingston";
    case 0x6f:
        return "STEC";
    case 0x74:
        return "Transcend";
    case 0x76:
        return "Patriot";
    case 0x82:
        return "Sony";
    case 0x9c:
        return "Angelbird";
    case 0xad:
        return "Longsys";
    default:
        return nullptr;
    }
}

struct SwitchSdCsd {
    uint8_t structure;  // 0 SDSC, 1 SDHC/SDXC, 2 SDUC
    uint16_t ccc;       // card command classes
    uint8_t tranSpeed;
    uint64_t bytes;  // capacity, 0 for an unknown structure
};

inline SwitchSdCsd switchSdParseCsd(const uint8_t csd[16])
{
    SwitchSdCsd c = {};
    c.structure = static_cast<uint8_t>(switchSdBits(csd, 128, 127, 126));
    c.tranSpeed = static_cast<uint8_t>(switchSdBits(csd, 128, 103, 96));
    c.ccc = static_cast<uint16_t>(switchSdBits(csd, 128, 95, 84));
    if (c.structure == 1) {
        c.bytes = (static_cast<uint64_t>(switchSdBits(csd, 128, 69, 48)) + 1) * 512 * 1024;
    } else if (c.structure == 2) {
        c.bytes = (static_cast<uint64_t>(switchSdBits(csd, 128, 75, 48)) + 1) * 512 * 1024;
    } else if (c.structure == 0) {
        c.bytes = (static_cast<uint64_t>(switchSdBits(csd, 128, 73, 62)) + 1)
                  << (switchSdBits(csd, 128, 49, 47) + 2 + switchSdBits(csd, 128, 83, 80));
    }
    return c;
}

// SCR CMD_SUPPORT (bits [35:32], Table 5-23).
constexpr uint8_t kSdCmd20 = 1, kSdCmd23 = 2, kSdCmd48 = 4, kSdCmd58 = 8;

struct SwitchSdScr {
    uint8_t structure, spec, spec3, spec4, specX, busWidths, cmdSupport;
    uint8_t major;  // physical layer version, from SD_SPEC/SPEC3/SPEC4/SPECX (Table 5-19): 1, 2, 3, 4, 5, 6...
};

inline SwitchSdScr switchSdParseScr(const uint8_t scr[8])
{
    SwitchSdScr s = {};
    s.structure = static_cast<uint8_t>(switchSdBits(scr, 64, 63, 60));
    s.spec = static_cast<uint8_t>(switchSdBits(scr, 64, 59, 56));
    s.busWidths = static_cast<uint8_t>(switchSdBits(scr, 64, 51, 48));
    s.spec3 = static_cast<uint8_t>(switchSdBits(scr, 64, 47, 47));
    s.spec4 = static_cast<uint8_t>(switchSdBits(scr, 64, 42, 42));
    s.specX = static_cast<uint8_t>(switchSdBits(scr, 64, 41, 38));
    s.cmdSupport = static_cast<uint8_t>(switchSdBits(scr, 64, 35, 32));
    if (s.spec < 2) {
        s.major = 1;
    } else if (!s.spec3) {
        s.major = 2;
    } else if (s.specX) {
        s.major = static_cast<uint8_t>(4 + s.specX);
    } else {
        s.major = s.spec4 ? 4 : 3;
    }
    return s;
}

// What the data path takes from the registers alone (setup()): ADMA2 with 128-bit descriptors when the controller
// lists ADMA2 (CAPABILITIES bit 19) and keeps v4 mode with 64-bit addressing (HOST_CONTROL2 bits 12 and 13, which
// setup() sets), and Auto CMD23 framing for ADMA2's multi-block commands when the card's SCR lists CMD23 (`scr`
// nullptr: the card's SCR was not read).
struct SwitchSdPath {
    bool adma, autoCmd23;
};

inline SwitchSdPath switchSdPath(uint32_t capabilities, uint16_t hostControl2, const SwitchSdScr *scr)
{
    const bool adma = (capabilities & (1u << 19)) != 0u && (hostControl2 & 0x3000u) == 0x3000u;
    return {adma, adma && scr && (scr->cmdSupport & kSdCmd23) != 0u};
}

// SD Status (ACMD13, 512 bits, Table 4-44).
struct SwitchSdStatus {
    uint8_t busWidth;    // 0 1-bit, 2 4-bit
    uint8_t speedClass;  // class number: 0, 2, 4, 6, 10
    uint8_t uhsGrade;    // U0, U1, U3
    uint8_t videoClass;  // V6, V10, V30, V60, V90 (the field holds the number)
    uint8_t appClass;    // 0 none, 1 A1, 2 A2
    uint8_t cqDepth;     // PERFORMANCE_ENHANCE b[335:331]: 0 none, else the queue depth (field + 1)
    bool cache, hostMaintenance, cardMaintenance, discard, fule;
    uint32_t auSectors;    // AU_SIZE b[431:428] in 512-byte sectors, 0 when the card names none
    uint16_t eraseSize;    // ERASE_SIZE b[423:408]: AUs that ERASE_TIMEOUT covers, 0 when unspecified
    uint8_t eraseTimeout;  // ERASE_TIMEOUT b[407:402], seconds
    uint8_t eraseOffset;   // ERASE_OFFSET b[401:400], seconds
};

inline SwitchSdStatus switchSdParseStatus(const uint8_t ssr[64])
{
    SwitchSdStatus s = {};
    static const uint8_t kClasses[] = {0, 2, 4, 6, 10};
    s.busWidth = static_cast<uint8_t>(switchSdBits(ssr, 512, 511, 510));
    const uint32_t speed = switchSdBits(ssr, 512, 447, 440);
    s.speedClass = speed < 5 ? kClasses[speed] : 0xff;
    s.uhsGrade = static_cast<uint8_t>(switchSdBits(ssr, 512, 399, 396));
    s.videoClass = static_cast<uint8_t>(switchSdBits(ssr, 512, 391, 384));
    s.appClass = static_cast<uint8_t>(switchSdBits(ssr, 512, 339, 336));
    const uint32_t queue = switchSdBits(ssr, 512, 335, 331);
    s.cqDepth = static_cast<uint8_t>(queue ? queue + 1 : 0);
    s.cache = (switchSdBits(ssr, 512, 330, 330) != 0u);
    s.hostMaintenance = (switchSdBits(ssr, 512, 329, 329) != 0u);
    s.cardMaintenance = (switchSdBits(ssr, 512, 328, 328) != 0u);
    s.discard = (switchSdBits(ssr, 512, 313, 313) != 0u);
    s.fule = (switchSdBits(ssr, 512, 312, 312) != 0u);
    // 16 KiB to 4 MiB in powers of two, then 8, 12, 16, 24, 32 and 64 MiB (Table 4-47).
    static const uint32_t kAuKiB[] = {0,    16,   32,   64,    128,   256,   512,   1024,
                                      2048, 4096, 8192, 12288, 16384, 24576, 32768, 65536};
    s.auSectors = kAuKiB[switchSdBits(ssr, 512, 431, 428)] * 2;
    s.eraseSize = static_cast<uint16_t>(switchSdBits(ssr, 512, 423, 408));
    s.eraseTimeout = static_cast<uint8_t>(switchSdBits(ssr, 512, 407, 402));
    s.eraseOffset = static_cast<uint8_t>(switchSdBits(ssr, 512, 401, 400));
    return s;
}

// The busy an erase of `aus` allocation units may take (ms): ERASE_OFFSET plus ERASE_TIMEOUT per ERASE_SIZE AUs, else
// 250 ms an AU, never under a second (Linux mmc_sd_erase_timeout).
inline uint64_t switchSdEraseTimeoutMs(const SwitchSdStatus &s, uint64_t aus)
{
    const uint64_t ms = s.eraseSize && s.eraseTimeout
                            ? static_cast<uint64_t>(s.eraseTimeout) * 1000 * aus / s.eraseSize +
                                  static_cast<uint64_t>(s.eraseOffset) * 1000
                            : 250 * aus;
    return ms < 1000 ? 1000 : ms;
}

// The most AUs one erase may cover while its specified busy stays within budgetMs (at least one).
inline uint32_t switchSdEraseAus(const SwitchSdStatus &s, uint64_t budgetMs)
{
    uint32_t aus = 1;
    while (aus < 1024 && switchSdEraseTimeoutMs(s, aus + 1) <= budgetMs)
        ++aus;
    return aus;
}

// ---- Extension registers (5.7) -------------------------------------------------------------------------------------
// CMD48/CMD49 argument: [31] MIO (0 memory), [30:27] FNO, [26] reserved / MW (mask write; 0), [25:9] ADDR (17 bits: page
// [25:18] and offset [17:9]), [8:0] LEN - 1. `address` is the linear page * 512 + offset; a command never crosses a page
// (5.7.1), which the caller keeps (switchSdExtRange).
inline uint32_t switchSdExtArgument(uint8_t fno, uint32_t address, uint32_t length)
{
    return static_cast<uint32_t>(fno & 0xf) << 27 | (address & 0x1ffff) << 9 | ((length - 1) & 0x1ff);
}

// Whether [address, address + length) is a valid single-page access.
inline bool switchSdExtRange(uint32_t address, uint32_t length)
{
    return (length != 0u) && length <= 512 && address <= 0x1ffff && (address & 0x1ff) + length <= 512;
}

struct SwitchSdExtFunction {
    bool found;
    uint8_t fno;
    uint32_t address;  // linear: page * 512 + offset
};

struct SwitchSdGeneralInfo {
    uint16_t revision, length;
    uint8_t extensions;
    SwitchSdExtFunction power, perf;  // Standard Function Codes 1 and 2
    const char *why;                  // nullptr, or why the layout is not one this parser trusts
};

inline uint16_t switchLe16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | static_cast<uint32_t>(p[1]) << 8);
}

inline uint32_t switchLe32(const uint8_t *p)
{
    return p[0] | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

// The General Information (FNO 0, page 0 and, when its length says so, page 1; `available` bytes of `info` were read: 512
// or 1024). Each extension: SFC at +0, Pointer to Next Extension at +40 (offset from the top, lower 10 bits), Number of
// Register Sets at +42, the first Register Set Address at +44 (FNO [21:18], page [16:9], offset [8:0]). As Linux, only
// extensions with exactly one register set are taken; a layout this does not understand gives `why`.
inline SwitchSdGeneralInfo switchSdParseGeneralInfo(const uint8_t *info, uint32_t available)
{
    SwitchSdGeneralInfo g = {};
    g.revision = switchLe16(info);
    g.length = switchLe16(info + 2);
    g.extensions = info[4];
    if (g.revision != 0)
        return g.why = "general information revision not 0", g;
    if (g.length > 1024 || g.length < 16)
        return g.why = "general information length out of range", g;
    if (g.length > available)
        return g.why = "general information continues on page 1", g;
    uint32_t ext = 16;
    for (unsigned i = 0; i < g.extensions; ++i) {
        if (ext + 48 > g.length)
            return g.why = "an extension runs past the general information", g;
        const uint16_t sfc = switchLe16(info + ext);
        const uint32_t next = switchLe16(info + ext + 40) & 0x3ff;
        if (info[ext + 42] == 1) {
            const uint32_t set = switchLe32(info + ext + 44);
            const SwitchSdExtFunction f = {true, static_cast<uint8_t>(set >> 18 & 0xf), set & 0x1ffff};
            if (sfc == 1 && !g.power.found)
                g.power = f;
            if (sfc == 2 && !g.perf.found)
                g.perf = f;
        }
        if (i + 1 == g.extensions)
            break;
        if (next <= ext)
            return g.why = "an extension pointer goes backwards", g;
        ext = next;
    }
    return g;
}

// Power Management register set (5.8.1, Table 5-28): byte 0 revision [3:0], byte 1 status (support [6:4], ready
// [2:0]; bit 4 POFS, bit 0 POFR), byte 2 setting (bit 0 POFN).
constexpr uint32_t kSdPowerRevision = 0, kSdPowerStatus = 1, kSdPowerSetting = 2;
constexpr uint32_t kSdPowerLength = kSdPowerSetting + 1;
constexpr uint8_t kSdPowerOffReady = 1u << 0, kSdPowerOffSupport = 1u << 4, kSdPowerOffNotify = 1u << 0;
constexpr uint64_t kSdPowerOffReadyLimitNs = 1500000000ULL;
constexpr uint32_t kSdPowerOffSleepMs = 10;

struct SwitchSdPower {
    uint8_t revision;
    bool offNotification, sustenance, powerDown;
};

inline SwitchSdPower switchSdParsePower(const uint8_t *reg)
{
    return {static_cast<uint8_t>(reg[kSdPowerRevision] & 0xf), (reg[kSdPowerStatus] & kSdPowerOffSupport) != 0,
            (reg[kSdPowerStatus] & 0x20) != 0, (reg[kSdPowerStatus] & 0x40) != 0};
}

inline bool switchSdPowerFits(const SwitchSdExtFunction &power)
{
    return power.found && switchSdExtRange(power.address, kSdPowerLength);
}

// The only Power Management write this driver may issue: Power Off Notification (byte 2 = 01h).
inline uint32_t switchSdPowerWriteAddress(const SwitchSdExtFunction &power, uint32_t byte, uint8_t value)
{
    if (!switchSdPowerFits(power))
        return 0;
    if (byte == kSdPowerSetting && value == kSdPowerOffNotify)
        return power.address + kSdPowerSetting;
    return 0;
}

// Performance Enhancement register set (Table 5-30), from its first byte. Byte 6 "CQ Support and Depth": 00h none,
// 01h-1Fh depth 2-32, 20h-FFh reserved (read as none). The second 256 bytes are the host's: 257 FX_EVENT enable, 258
// maintenance enables, 260 Cache Enable (bit 0), 261 Flush Cache (bit 0: the host sets it, the card clears it when the
// flush is done, busy on DAT0 meanwhile, at most 1 s: 4.17), 262 CQ control (read for identification only).
constexpr uint32_t kSdPerfCqSupport = 6, kSdPerfCacheControl = 260, kSdPerfFlushControl = 261, kSdPerfCqControl = 262;
constexpr uint32_t kSdPerfLength = kSdPerfCqControl + 1;
constexpr uint8_t kSdCacheEnable = 1, kSdFlushCache = 1;
constexpr uint64_t kSdFlushCacheLimitNs = 1500000000ULL;
constexpr uint32_t kSdFlushCacheSleepMs = 1;
constexpr uint32_t kSdR1Errors = 0xfdf90008u;

struct SwitchSdPerf {
    uint8_t revision;
    bool fxEvent, cardMaintenance, hostMaintenance, cache;
    uint8_t cqDepth;                                              // 0 none, else 2..32
    uint8_t taskErrors;                                           // bytes 8-15 nonzero
    uint8_t fxEnable, maintenanceEnable, cacheEnable, cqControl;  // bytes 257, 258, 260, 262 as read
};

inline SwitchSdPerf switchSdParsePerf(const uint8_t *reg)
{
    SwitchSdPerf p = {};
    p.revision = reg[0];
    p.fxEvent = ((reg[1] & 1) != 0);
    p.cardMaintenance = ((reg[2] & 1) != 0);
    p.hostMaintenance = (reg[2] & 2) != 0;
    p.cache = ((reg[4] & 1) != 0);
    p.cqDepth =
        static_cast<uint8_t>(reg[kSdPerfCqSupport] && reg[kSdPerfCqSupport] <= 0x1f ? reg[kSdPerfCqSupport] + 1 : 0);
    for (unsigned i = 8; i < 16; ++i)
        p.taskErrors |= reg[i];
    p.fxEnable = reg[257];
    p.maintenanceEnable = reg[258];
    p.cacheEnable = reg[kSdPerfCacheControl];
    p.cqControl = reg[kSdPerfCqControl];
    return p;
}

// Whether the performance register set (its first 263 bytes) sits inside one 512-byte extension page.
inline bool switchSdPerfFits(const SwitchSdExtFunction &perf)
{
    return perf.found && switchSdExtRange(perf.address, kSdPerfLength);
}

// Permitted Performance Enhancement writes: Cache Enable (byte 260 = 00h or 01h) and Flush Cache (byte 261 = 01h).
inline uint32_t switchSdPerfWriteAddress(const SwitchSdExtFunction &perf, uint32_t byte, uint8_t value)
{
    if (!switchSdPerfFits(perf))
        return 0;
    if (byte == kSdPerfFlushControl && value == kSdFlushCache)
        return perf.address + kSdPerfFlushControl;
    if (byte == kSdPerfCacheControl && (value == 0 || value == kSdCacheEnable))
        return perf.address + kSdPerfCacheControl;
    return 0;
}

enum class SwitchSdCacheOnStep : uint8_t {
    kSkipped,
    kDone,
    kBadAddress,
    kEnableFailed,
    kReadBackFailed,
    kNotEnabled,
};

struct SwitchSdCacheOn {
    SwitchSdCacheOnStep step;
    int error;
    uint32_t r1;
    uint8_t cacheEnable;
};

// SD 6.00 4.17: enable the card's volatile write cache (byte 260 bit 0 = 1) and verify via read-back.
template <typename Bus>
inline SwitchSdCacheOn switchSdEnableCache(bool havePerf, const SwitchSdExtFunction &perfFunc, const SwitchSdPerf &perf,
                                           Bus &bus)
{
    if (!havePerf || !perf.cache)
        return {SwitchSdCacheOnStep::kSkipped, 0, 0, perf.cacheEnable};
    const uint32_t cacheAddress = switchSdPerfWriteAddress(perfFunc, kSdPerfCacheControl, kSdCacheEnable);
    if (!cacheAddress || !switchSdExtRange(cacheAddress, 1))
        return {SwitchSdCacheOnStep::kBadAddress, 0, 0, perf.cacheEnable};
    uint32_t r1 = 0;
    if (const int err = bus.writeByte(perfFunc.fno, cacheAddress, kSdCacheEnable, &r1); err || (r1 & kSdR1Errors))
        return {SwitchSdCacheOnStep::kEnableFailed, err, r1, perf.cacheEnable};
    uint8_t readBack = 0;
    r1 = 0;
    if (const int err = bus.readBytes(perfFunc.fno, cacheAddress, 1, &readBack, &r1); err || (r1 & kSdR1Errors))
        return {SwitchSdCacheOnStep::kReadBackFailed, err, r1, perf.cacheEnable};
    if (!(readBack & kSdCacheEnable))
        return {SwitchSdCacheOnStep::kNotEnabled, 0, 0, readBack};
    return {SwitchSdCacheOnStep::kDone, 0, 0, readBack};
}

enum class SwitchSdFlushStep : uint8_t {
    kSkipped,
    kDone,
    kBadAddress,
    kFlushFailed,
    kReadBackFailed,
    kStillPending,
};

struct SwitchSdFlush {
    SwitchSdFlushStep step;
    int error;
    uint32_t r1;
    uint8_t flushCache;
};

// SD 6.00 4.17 / 5.7.2.2 / Table 5-30: set Flush Cache (byte 261 bit 0 = 1), wait for CMD49 DAT0 busy end, then
// confirm bit 0 cleared via CMD48 (required by spec: cards may release DAT0 before flushing asynchronously).
template <typename Bus>
inline SwitchSdFlush switchSdFlushCache(bool havePerf, const SwitchSdExtFunction &perfFunc, Bus &bus)
{
    if (!havePerf)
        return {SwitchSdFlushStep::kSkipped, 0, 0, 0};
    const uint32_t flushAddress = switchSdPerfWriteAddress(perfFunc, kSdPerfFlushControl, kSdFlushCache);
    if (!flushAddress || !switchSdExtRange(flushAddress, 1))
        return {SwitchSdFlushStep::kBadAddress, 0, 0, 0};
    uint32_t r1 = 0;
    if (const int err = bus.writeByte(perfFunc.fno, flushAddress, kSdFlushCache, &r1); err || (r1 & kSdR1Errors))
        return {SwitchSdFlushStep::kFlushFailed, err, r1, 0};
    const uint64_t busyEndedNs = bus.nowNs();
    uint32_t waitMs = kSdFlushCacheSleepMs;
    for (;;) {
        uint8_t status = 0;
        r1 = 0;
        if (const int err = bus.readBytes(perfFunc.fno, flushAddress, 1, &status, &r1); err || (r1 & kSdR1Errors))
            return {SwitchSdFlushStep::kReadBackFailed, err, r1, 0};
        if (!(status & kSdFlushCache))
            return {SwitchSdFlushStep::kDone, 0, 0, status};
        const uint64_t now = bus.nowNs();
        if (now >= busyEndedNs && now - busyEndedNs >= kSdFlushCacheLimitNs)
            return {SwitchSdFlushStep::kStillPending, 0, 0, status};
        bus.sleepMs(waitMs);
        if (waitMs < 16u)
            waitMs *= 2u;
    }
}

enum class SwitchSdCacheOffStep : uint8_t {
    kSkipped,
    kDone,
    kBadAddress,
    kFlushFailed,
    kDisableFailed,
    kReadBackFailed,
    kStillOn,
};

struct SwitchSdCacheOff {
    SwitchSdCacheOffStep step;
    int error;
    uint32_t r1;
    uint8_t cacheEnable;
    uint8_t flushCache;
};

// If an earlier stage left the card's cache on, flush it, disable it, and confirm both bits read 0 in one read-back
// (5.7.2.2: CMD49 busy ends when complete). Flush runs first because 6.00 does not say clearing Cache Enable flushes.
// Sends no commands when the performance set was not read or Cache Enable already reads 0.
template <typename Bus>
inline SwitchSdCacheOff switchSdDisableFoundCache(bool havePerf, const SwitchSdExtFunction &perfFunc,
                                                  const SwitchSdPerf &perf, Bus &bus)
{
    if (!havePerf || !(perf.cacheEnable & kSdCacheEnable))
        return {SwitchSdCacheOffStep::kSkipped, 0, 0, perf.cacheEnable, 0};
    const uint32_t flushAddress = switchSdPerfWriteAddress(perfFunc, kSdPerfFlushControl, kSdFlushCache);
    const uint32_t cacheAddress = switchSdPerfWriteAddress(perfFunc, kSdPerfCacheControl, 0);
    if (!flushAddress || !cacheAddress || !switchSdExtRange(cacheAddress, 2))
        return {SwitchSdCacheOffStep::kBadAddress, 0, 0, perf.cacheEnable, 0};
    uint32_t r1 = 0;
    if (const int err = bus.writeByte(perfFunc.fno, flushAddress, kSdFlushCache, &r1); err || (r1 & kSdR1Errors))
        return {SwitchSdCacheOffStep::kFlushFailed, err, r1, perf.cacheEnable, 0};
    r1 = 0;
    if (const int err = bus.writeByte(perfFunc.fno, cacheAddress, 0, &r1); err || (r1 & kSdR1Errors))
        return {SwitchSdCacheOffStep::kDisableFailed, err, r1, perf.cacheEnable, 0};
    uint8_t readBack[2] = {};
    r1 = 0;
    if (const int err = bus.readBytes(perfFunc.fno, cacheAddress, sizeof(readBack), readBack, &r1);
        err || (r1 & kSdR1Errors)) {
        return {SwitchSdCacheOffStep::kReadBackFailed, err, r1, perf.cacheEnable, 0};
    }
    if ((readBack[0] & kSdCacheEnable) || (readBack[1] & kSdFlushCache))
        return {SwitchSdCacheOffStep::kStillOn, 0, 0, readBack[0], readBack[1]};
    return {SwitchSdCacheOffStep::kDone, 0, 0, readBack[0], readBack[1]};
}

enum class SwitchSdPowerOffStep : uint8_t {
    kSkipped,
    kDone,
    kBadAddress,
    kNotifyFailed,
    kReadBackFailed,
    kNotReady,
};

struct SwitchSdPowerOff {
    SwitchSdPowerOffStep step;
    int error;
    uint32_t r1;
    uint8_t status;
};

// SD 6.00 5.8.1.3 / Fig. 5-19: set POFN=1 (byte 2), wait for CMD49 busy end, then read byte 1 immediately.
// If POFR=1, return at once with zero sleeps; if POFR=0, sleep and re-read until POFR=1 or 1.5 s elapses
// (500 ms margin over the spec's 1 s post-busy window).
template <typename Bus>
inline SwitchSdPowerOff switchSdPowerOffNotify(bool havePower, const SwitchSdExtFunction &powerFunc,
                                               const SwitchSdPower &power, Bus &bus)
{
    if (!havePower || !power.offNotification)
        return {SwitchSdPowerOffStep::kSkipped, 0, 0, 0};
    const uint32_t notifyAddress = switchSdPowerWriteAddress(powerFunc, kSdPowerSetting, kSdPowerOffNotify);
    const uint32_t statusAddress = powerFunc.address + kSdPowerStatus;
    if (!notifyAddress || !switchSdExtRange(statusAddress, 1))
        return {SwitchSdPowerOffStep::kBadAddress, 0, 0, 0};
    uint32_t r1 = 0;
    if (const int err = bus.writeByte(powerFunc.fno, notifyAddress, kSdPowerOffNotify, &r1); err || (r1 & kSdR1Errors))
        return {SwitchSdPowerOffStep::kNotifyFailed, err, r1, 0};
    const uint64_t busyEndedNs = bus.nowNs();
    for (;;) {
        uint8_t status = 0;
        r1 = 0;
        if (const int err = bus.readBytes(powerFunc.fno, statusAddress, 1, &status, &r1); err || (r1 & kSdR1Errors))
            return {SwitchSdPowerOffStep::kReadBackFailed, err, r1, 0};
        if (status & kSdPowerOffReady)
            return {SwitchSdPowerOffStep::kDone, 0, 0, status};
        const uint64_t now = bus.nowNs();
        if (now >= busyEndedNs && now - busyEndedNs >= kSdPowerOffReadyLimitNs)
            return {SwitchSdPowerOffStep::kNotReady, 0, 0, status};
        bus.sleepMs(kSdPowerOffSleepMs);
    }
}

// ---- Manual DDR200 tap tuning (Tegra X1 SDMMC1) -------------------------------------------------------------------
// Tegra X1 has no hardware tuning in DDR mode; manual tuning sweeps CMD19 across 128 taps and picks the midpoint of
// the widest passing window, rejecting windows narrower than kSdMinTuneWindow (DLL-only cards fail this check).
constexpr uint32_t kSdManualTuneTaps = 128;
constexpr uint32_t kSdMinTuneWindow = 8;

struct SwitchSdTapWindow {
    bool ok;
    uint32_t tap;
    uint32_t window;
};

inline SwitchSdTapWindow switchSdBestTapWindow(const uint32_t results[4], uint32_t count = kSdManualTuneTaps,
                                               uint32_t minWindow = kSdMinTuneWindow)
{
    uint32_t runStart = count, runLen = 0, bestTap = 0, bestLen = 0;
    for (uint32_t i = 0; i <= count; ++i) {
        const bool pass = i < count && ((results[i / 32] >> (i % 32)) & 1u) != 0u;
        if (pass) {
            if (runStart == count)
                runStart = i;
            ++runLen;
        } else if (runStart != count) {
            if (runLen > bestLen) {
                bestLen = runLen;
                bestTap = (runStart + (i - 1)) / 2;
            }
            runStart = count;
            runLen = 0;
        }
    }
    return {bestTap != 0 && bestLen >= minWindow, bestTap, bestLen};
}

// ---- Data command framing ------------------------------------------------------------------------------------------
// SDHCI TRANSFER_MODE (0x0c) bits and the COMMAND (0x0e) low byte of a data command with an R1 response (48-bit, CRC and
// index checked, data present).
constexpr uint32_t kSdTransferDma = 0x01, kSdTransferBlockCount = 0x02, kSdTransferAutoCmd12 = 0x04,
                   kSdTransferAutoCmd23 = 0x08, kSdTransferRead = 0x10, kSdTransferMulti = 0x20;
constexpr uint32_t kSdDataR1 = 0x3a;

struct SwitchSdFrame {
    uint16_t transfer, command;
    uint32_t argument2;  // the Auto CMD23 argument (SDHCI 0x00 "Argument 2"), 0 when unused
    bool autoStop;       // an Auto CMD12 follows the data (its response and error bits mean something)
};

// CMD17/18 (read) or CMD24/25 (write). A multi-block command stops by Auto CMD12 after the data (`cmd23` false) or is
// preceded by Auto CMD23 SET_BLOCK_COUNT with the block count in Argument 2, so the card knows the length up front and
// no stop command follows (SDHCI 3.00+, the mode L4T's sdhci.c uses on this controller with SD cards whose SCR lists
// CMD23).
inline SwitchSdFrame switchSdBlockFrame(uint32_t sectors, bool write, bool cmd23)
{
    const bool multi = sectors > 1;
    uint16_t transfer = static_cast<uint16_t>(kSdTransferDma | (write ? 0 : kSdTransferRead));
    if (multi) {
        transfer = static_cast<uint16_t>(transfer | kSdTransferBlockCount | kSdTransferMulti |
                                         (cmd23 ? kSdTransferAutoCmd23 : kSdTransferAutoCmd12));
    }
    const uint32_t writeIndex = multi ? 25 : 24;
    const uint32_t readIndex = multi ? 18 : 17;
    const uint32_t index = write ? writeIndex : readIndex;
    return {transfer, static_cast<uint16_t>(index << 8 | kSdDataR1), multi && cmd23 ? sectors : 0, multi && !cmd23};
}
