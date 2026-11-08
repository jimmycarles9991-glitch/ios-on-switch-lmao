// SPDX-License-Identifier: BSD-2-Clause-Patent
// r14-sdcq: the SD card's own registers and the command encodings the driver builds from them (NativeSwitchBlock.cpp),
// kept here as plain functions so the host can test every bit (tests/native_sd_card_harness.cpp).
//
// Sources: SD Physical Layer Simplified Specification 6.00 (sdcard.org; the copy at
// academy.cba.mit.edu/classes/networking_communications/SD/SD.pdf): 4.7.4 Table 4.7.4-1 (CQ commands), 4.10.2 Table 4-44
// (SD Status), 4.19 (Command Queue), 5.2/5.3 (CID/CSD), 5.6 Table 5-17/5-23 (SCR, CMD_SUPPORT), 5.7 (extension registers,
// CMD48/CMD49, General Information), 5.8.2 Table 5-30 (Performance Enhancement register set). Linux drivers/mmc/core/sd.c
// (mmc_sd_read_ext_regs, sd_parse_ext_reg*, sd_enable_cache) and the Raspberry Pi kernel's mmc_sd_cmdq_switch
// (drivers/mmc/core/sd_ops.c: byte 262 of the performance register enables CQ) read the same layouts.
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

// Power Management register set (5.8.1): byte 0 revision [3:0], byte 1 support bits.
struct SwitchSdPower {
    uint8_t revision;
    bool offNotification, sustenance, powerDown;
};

inline SwitchSdPower switchSdParsePower(const uint8_t *reg)
{
    return {static_cast<uint8_t>(reg[0] & 0xf), (reg[1] & 0x10) != 0, (reg[1] & 0x20) != 0, (reg[1] & 0x40) != 0};
}

// Performance Enhancement register set (Table 5-30), from its first byte. Byte 6 "CQ Support and Depth": 00h none,
// 01h-1Fh depth 2-32, 20h-FFh reserved (read as none). The second 256 bytes are the host's: 257 FX_EVENT enable, 258
// maintenance enables, 260 Cache Enable (bit 0), 261 Flush Cache (bit 0: the host sets it, the card clears it when the
// flush is done, busy on DAT0 meanwhile, at most 1 s: 4.17), 262 Enable CQ (bit 0) and CQ Mode (bit 1: 0 voluntary,
// 1 sequential), bits 7:2 reserved. r16-sdcq: the Enable CQ and CQ Mode bits are separate (a card may take bit 0 and keep
// voluntary mode), and 4.19.1 asks for the cache before the queue ("Cache Feature should be enabled before enabling
// Command Queue"): Linux (sd.c mmc_sd_init_card) and the Raspberry Pi kernel (sd_ops.c mmc_sd_cmdq_switch) always enable
// the cache first, and the latter takes CQ support only from cards that also offer the cache and Power Off Notification.
constexpr uint32_t kSdPerfCqSupport = 6, kSdPerfCacheControl = 260, kSdPerfFlushControl = 261, kSdPerfCqControl = 262;
constexpr uint8_t kSdCqEnable = 1, kSdCqSequential = 2, kSdCacheEnable = 1, kSdFlushCache = 1;

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
    p.cqDepth = static_cast<uint8_t>(reg[6] && reg[6] <= 0x1f ? reg[6] + 1 : 0);
    for (unsigned i = 8; i < 16; ++i)
        p.taskErrors |= reg[i];
    p.fxEnable = reg[257];
    p.maintenanceEnable = reg[258];
    p.cacheEnable = reg[260];
    p.cqControl = reg[kSdPerfCqControl];
    return p;
}

// Where the CQ control byte is, when the whole performance register set (its first 263 bytes) sits in one page so a CMD48
// of it and the CMD49 of byte 262 stay inside that page. 0 when it does not (the driver then leaves CQ alone).
inline uint32_t switchSdCqControlAddress(const SwitchSdExtFunction &perf)
{
    if (!perf.found || !switchSdExtRange(perf.address, kSdPerfCqControl + 1))
        return 0;
    return perf.address + kSdPerfCqControl;
}

// r16-sdcq: the host bytes the driver writes (SwitchCq.h): Cache Enable, Flush Cache and the CQ control byte, and
// only these values. 0 for any other byte or value (the driver panics on 0 before a command is built), else the byte's
// address under the same one-page rule as the CQ control byte (they are its neighbours).
inline uint32_t switchSdPerfWriteAddress(const SwitchSdExtFunction &perf, uint32_t byte, uint8_t value)
{
    const uint32_t cq = switchSdCqControlAddress(perf);
    if (!cq)
        return 0;
    switch (byte) {
    case kSdPerfCacheControl:
        return value == 0 || value == kSdCacheEnable ? cq - 2 : 0;
    case kSdPerfFlushControl:
        return value == kSdFlushCache ? cq - 1 : 0;
    case kSdPerfCqControl:
        return value == 0 || value == (kSdCqEnable | kSdCqSequential) ? cq : 0;
    default:
        return 0;
    }
}

// What a read-back of the CQ control byte says, for the one line a failed enable leaves: whether the enable bit or the
// mode bit (or reserved bits) did not take.
inline const char *switchSdCqControlMeaning(uint8_t value)
{
    if (value & 0xfc)
        return "reserved bits set";
    switch (value & 3) {
    case 0:
        return "the queue stays disabled";
    case 1:
        return "enabled in voluntary mode: the card did not take sequential mode";
    case 2:
        return "sequential mode set but the queue disabled";
    default:
        return "enabled, sequential";
    }
}

// r18-sdcq: the error and event bits set in an R1 card status (Table 4-42), by name, for the queue's error lines: "-" when
// none. COM_CRC and ILLEGAL (clear condition B) describe the command before the one whose response carries them.
inline void switchSdR1ErrorNames(uint32_t r1, char *out, unsigned long size)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } kBits[] = {
        {1u << 31, "OUT_OF_RANGE"}, {1u << 30, "ADDRESS"},     {1u << 29, "BLOCK_LEN"}, {1u << 28, "ERASE_SEQ"},
        {1u << 27, "ERASE_PARAM"},  {1u << 26, "WP"},          {1u << 24, "LOCK"},      {1u << 23, "COM_CRC"},
        {1u << 22, "ILLEGAL"},      {1u << 21, "ECC"},         {1u << 20, "CC"},        {1u << 19, "ERROR"},
        {1u << 16, "CSD_OW"},       {1u << 13, "ERASE_RESET"}, {1u << 6, "FX_EVENT"},   {1u << 3, "AKE_SEQ"}};

    if (!size)
        return;
    unsigned long at = 0;
    out[0] = 0;
    for (const auto &b : kBits) {
        if (!(r1 & b.bit))
            continue;
        for (const char *c = at ? "|" : ""; *c && at + 1 < size; ++c)
            out[at++] = *c;
        for (const char *c = b.name; *c && at + 1 < size; ++c)
            out[at++] = *c;
    }
    if (!at && size > 1)
        out[at++] = '-';
    out[at] = 0;
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

// CMD17/18 (read) or CMD24/25 (write). A multi-block command stops by Auto CMD12 after the data (`cmd23` false: what
// every command did before r14-sdcq) or is preceded by Auto CMD23 SET_BLOCK_COUNT with the block count in Argument 2, so
// the card knows the length up front and no stop command follows (SDHCI 3.00+, the mode L4T's sdhci.c uses on this
// controller with SD cards whose SCR lists CMD23).
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

// ---- Command Queue (4.19) ------------------------------------------------------------------------------------------
// CMD44 Q_TASK_INFO_A: [30] direction (1 read, 0 write), [23] priority, [20:16] task id, [15:0] blocks (1..65535); [29:24]
// (extended address, SDUC only) and the rest 0. CMD45 Q_TASK_INFO_B: the start block. CMD46 Q_RD_TASK / CMD47 Q_WR_TASK:
// [20:16] task id. CMD43 Q_MANAGEMENT (R1b): [20:16] task id, [3:0] 1 abort the entire queue, 2 abort that task.
inline uint32_t switchSdCmd44(bool read, uint8_t task, uint32_t blocks)
{
    return (read ? 1u << 30 : 0u) | static_cast<uint32_t>(task & 0x1f) << 16 | (blocks & 0xffff);
}

inline uint32_t switchSdTaskArgument(uint8_t task)
{
    return static_cast<uint32_t>(task & 0x1f) << 16;
}

constexpr uint32_t kSdAbortQueue = 1, kSdAbortTask = 2;

inline uint32_t switchSdCmd43(uint32_t operation, uint8_t task)
{
    return static_cast<uint32_t>(task & 0x1f) << 16 | (operation & 0xf);
}

// CMD46 of a read task: no Auto CMD12 or CMD23, the block count was given in CMD44; the card sends that many blocks
// (4.19.7) and the host stops after BLOCK_COUNT of them.
inline SwitchSdFrame switchSdTaskFrame(uint32_t sectors)
{
    const bool multi = sectors > 1;
    const uint16_t transfer = static_cast<uint16_t>(kSdTransferDma | kSdTransferRead |
                                                    (multi ? kSdTransferBlockCount | kSdTransferMulti : 0));
    return {transfer, static_cast<uint16_t>(46u << 8 | kSdDataR1), 0, false};
}
