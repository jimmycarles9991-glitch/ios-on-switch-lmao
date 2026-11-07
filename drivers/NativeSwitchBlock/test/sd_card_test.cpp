// Host test of drivers/NativeSwitchBlock/SwitchSdCard.h: the SD card register decoders and command encodings the SD
// driver uses for its identification lines, CMD23 framing and the command queue, bit for bit. What the driver does with
// them is guarded by sd_card_source_test.cpp.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../SwitchSdCard.h"
#include "bazel/testing/require.h"

// Sets bits [hi:lo] of a big-endian register of `bits` bits.
static void put(uint8_t *reg, unsigned bits, unsigned hi, unsigned lo, uint64_t value)
{
    for (unsigned bit = lo; bit <= hi; ++bit) {
        const unsigned index = (bits - 1 - bit) / 8, shift = bit % 8;
        reg[index] = static_cast<uint8_t>((reg[index] & ~(1u << shift)) | (((value >> (bit - lo)) & 1) << shift));
    }
}

static void fields()
{
    std::mt19937_64 random(7);
    for (unsigned round = 0; round < 2000; ++round) {
        uint8_t reg[64] = {};
        const unsigned bits = (round % 3 == 0) ? 128 : (round % 3 == 1) ? 64 : 512;
        const unsigned lo = static_cast<unsigned>(random() % bits);
        const unsigned width = 1 + static_cast<unsigned>(random() % 32);
        const unsigned hi = lo + width - 1 < bits ? lo + width - 1 : bits - 1;
        const uint64_t value = random() & ((hi - lo == 31) ? 0xffffffffull : ((1ull << (hi - lo + 1)) - 1));
        put(reg, bits, hi, lo, value);
        REQUIRE(switchSdBits(reg, bits, hi, lo) == value);
        // Nothing outside the field.
        if (lo)
            REQUIRE(switchSdBits(reg, bits, lo - 1, 0) == 0 || lo > 32);
    }
    // MSB first: bit 127 of a 16-byte register is byte 0's top bit.
    uint8_t cid[16] = {0x80};
    REQUIRE(switchSdBits(cid, 128, 127, 127) == 1 && switchSdBits(cid, 128, 126, 0) == 0);
}

// What the controller holds after an R2: REP[119:0] = register bits [127:8], REP[127:120] = 0.
static void toRep(const uint8_t reg[16], uint32_t rep[4])
{
    uint8_t bytes[16] = {};
    for (unsigned k = 0; k < 15; ++k)
        bytes[14 - k] = reg[k];
    for (unsigned w = 0; w < 4; ++w)
        rep[w] = bytes[4 * w] | uint32_t(bytes[4 * w + 1]) << 8 | uint32_t(bytes[4 * w + 2]) << 16 |
                 uint32_t(bytes[4 * w + 3]) << 24;
}

static void identity()
{
    // A SanDisk card as its CID would read: MID 03, OID "SD", PNM "SN01T", PRV 8.6, PSN, made 2023-06.
    uint8_t cid[16] = {};
    put(cid, 128, 127, 120, 0x03);
    put(cid, 128, 119, 104, 'S' << 8 | 'D');
    const char *name = "SN01T";
    for (unsigned i = 0; i < 5; ++i)
        put(cid, 128, 103 - 8 * i, 96 - 8 * i, uint8_t(name[i]));
    put(cid, 128, 63, 56, 0x86);
    put(cid, 128, 55, 24, 0x1234abcd);
    put(cid, 128, 19, 8, (23 << 4) | 6);
    put(cid, 128, 7, 0, 0x55);  // CRC7 and end bit: the controller drops them
    uint32_t rep[4];
    toRep(cid, rep);
    REQUIRE(rep[3] >> 24 == 0);
    uint8_t back[16];
    switchSdR2(rep, back);
    REQUIRE(!memcmp(back, cid, 15) && back[15] == 0);
    const SwitchSdCid c = switchSdParseCid(back);
    REQUIRE(c.mid == 3 && !strcmp(c.oid, "SD") && !strcmp(c.pnm, "SN01T") && c.prv == 0x86 && c.psn == 0x1234abcd &&
            c.year == 2023 && c.month == 6);
    REQUIRE(!strcmp(switchSdMaker(3), "SanDisk") && !strcmp(switchSdMaker(0x1b), "Samsung") && !switchSdMaker(0x5a));
    // Unprintable name bytes print as '?'.
    put(back, 128, 103, 96, 0x07);
    REQUIRE(switchSdParseCid(back).pnm[0] == '?');

    // CSD 2.0 of an "1 TB" card: C_SIZE 1815551 -> (C_SIZE + 1) * 512 KiB = 886.5 GiB.
    uint8_t csd[16] = {};
    put(csd, 128, 127, 126, 1);
    put(csd, 128, 103, 96, 0x32);
    put(csd, 128, 95, 84, 0x5b5);
    put(csd, 128, 69, 48, 1815551);
    SwitchSdCsd s = switchSdParseCsd(csd);
    REQUIRE(s.structure == 1 && s.tranSpeed == 0x32 && s.ccc == 0x5b5 && s.bytes == 1815552ull * 512 * 1024 &&
            (s.bytes >> 30) == 886);
    // CSD 1.0 (SDSC): (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN.
    uint8_t old[16] = {};
    put(old, 128, 83, 80, 9);
    put(old, 128, 73, 62, 3999);
    put(old, 128, 49, 47, 7);
    s = switchSdParseCsd(old);
    REQUIRE(s.structure == 0 && s.bytes == 4000ull * 512 * 512);
    // CSD 3.0 (SDUC): 28-bit C_SIZE.
    uint8_t uc[16] = {};
    put(uc, 128, 127, 126, 2);
    put(uc, 128, 75, 48, 0x4000000);
    REQUIRE(switchSdParseCsd(uc).bytes == (0x4000000ull + 1) * 512 * 1024);

    // SCR of a 6.x card with CMD20/23/48 support, 1- and 4-bit bus.
    uint8_t scr[8] = {};
    put(scr, 64, 59, 56, 2);
    put(scr, 64, 51, 48, 5);
    put(scr, 64, 47, 47, 1);
    put(scr, 64, 42, 42, 1);
    put(scr, 64, 41, 38, 2);
    put(scr, 64, 35, 32, kSdCmd20 | kSdCmd23 | kSdCmd48);
    SwitchSdScr r = switchSdParseScr(scr);
    REQUIRE(r.spec == 2 && r.spec3 == 1 && r.spec4 == 1 && r.specX == 2 && r.major == 6 && r.busWidths == 5 &&
            (r.cmdSupport & kSdCmd23) && (r.cmdSupport & kSdCmd48) && !(r.cmdSupport & kSdCmd58));
    // Versions by Table 5-19: 3.0x (SPEC3 only), 4.xx (SPEC4), 2.00, 1.x.
    put(scr, 64, 41, 38, 0);
    REQUIRE(switchSdParseScr(scr).major == 4);
    put(scr, 64, 42, 42, 0);
    REQUIRE(switchSdParseScr(scr).major == 3);
    put(scr, 64, 47, 47, 0);
    REQUIRE(switchSdParseScr(scr).major == 2);
    put(scr, 64, 59, 56, 1);
    REQUIRE(switchSdParseScr(scr).major == 1);
    // The byte the CMD_SUPPORT bits live in: SCR bits [39:32] are byte 3.
    uint8_t only23[8] = {0, 0, 0, kSdCmd23, 0, 0, 0, 0};
    REQUIRE(switchSdParseScr(only23).cmdSupport == kSdCmd23);

    // SD Status of an A2 card: 4-bit, class 10, U3, V30, A2, CQ field 1Fh (depth 32), cache, both maintenances, discard.
    uint8_t ssr[64] = {};
    put(ssr, 512, 511, 510, 2);
    put(ssr, 512, 447, 440, 4);
    put(ssr, 512, 399, 396, 3);
    put(ssr, 512, 391, 384, 30);
    put(ssr, 512, 339, 336, 2);
    put(ssr, 512, 335, 331, 0x1f);
    put(ssr, 512, 330, 328, 7);
    put(ssr, 512, 313, 313, 1);
    SwitchSdStatus st = switchSdParseStatus(ssr);
    REQUIRE(st.busWidth == 2 && st.speedClass == 10 && st.uhsGrade == 3 && st.videoClass == 30 && st.appClass == 2 &&
            st.cqDepth == 32 && st.cache && st.hostMaintenance && st.cardMaintenance && st.discard && !st.fule);
    put(ssr, 512, 335, 331, 0);
    put(ssr, 512, 339, 336, 1);
    st = switchSdParseStatus(ssr);
    REQUIRE(st.cqDepth == 0 && st.appClass == 1);
    put(ssr, 512, 335, 331, 1);
    REQUIRE(switchSdParseStatus(ssr).cqDepth == 2);
}

// The erase fields of the 1 TB card the console boots from (SSR as SwitchSD logged it, 11-06): no DISCARD_SUPPORT, 4 MiB
// AUs, 1 s per 15 AUs plus 1 s. Its erases go 15 AUs a command within a 2 s budget.
TEST(SdCard, EraseTiming)
{
    uint8_t ssr[64] = {0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x90, 0x00, 0x0f,
                       0x05, 0x39, 0x1e, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0xfc, 0x00, 0x01};
    const SwitchSdStatus st = switchSdParseStatus(ssr);
    REQUIRE(!st.discard && st.fule);
    REQUIRE(st.auSectors == 8192 && st.eraseSize == 15 && st.eraseTimeout == 1 && st.eraseOffset == 1);
    REQUIRE(switchSdEraseTimeoutMs(st, 15) == 2000);
    REQUIRE(switchSdEraseAus(st, 2000) == 15);
    SwitchSdStatus unspecified = {};
    REQUIRE(switchSdEraseTimeoutMs(unspecified, 1) == 1000 && switchSdEraseTimeoutMs(unspecified, 8) == 2000);
    REQUIRE(switchSdEraseAus(unspecified, 2000) == 8);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = uint8_t(v >> (8 * i));
}

// General Information with a power function (FNO 1) and a performance function (FNO 2), as Table 5-31's example lays
// them out: header 16 bytes, extensions 48 bytes each (one register set).
static std::vector<uint8_t> generalInfo(uint16_t revision = 0)
{
    std::vector<uint8_t> info(1024, 0);
    put16(&info[0], revision);
    put16(&info[2], 112);
    info[4] = 2;
    put16(&info[16], 1);  // SFC: power management
    put16(&info[16 + 40], 64);
    info[16 + 42] = 1;
    put32(&info[16 + 44], 1u << 18);
    put16(&info[64], 2);  // SFC: performance enhancement
    put16(&info[64 + 40], 0);
    info[64 + 42] = 1;
    put32(&info[64 + 44], 2u << 18 | 3u << 9 | 0);  // FNO 2, page 3, offset 0
    return info;
}

static void extension()
{
    // CMD48/CMD49 arguments: Linux's sd_read_ext_reg formula, fno << 27 | page << 18 | offset << 9 | (len - 1).
    std::mt19937 random(11);
    for (unsigned round = 0; round < 5000; ++round) {
        const uint8_t fno = uint8_t(random() % 16), page = uint8_t(random() % 256);
        const uint16_t offset = uint16_t(random() % 512), length = uint16_t(1 + random() % (512 - offset));
        const uint32_t linux = uint32_t(fno) << 27 | uint32_t(page) << 18 | uint32_t(offset) << 9 | (length - 1u);
        REQUIRE(switchSdExtArgument(fno, uint32_t(page) * 512 + offset, length) == linux);
        REQUIRE(switchSdExtRange(uint32_t(page) * 512 + offset, length));
        // MIO (bit 31) and MW (bit 26) stay 0.
        REQUIRE(!(linux & (1u << 31)) &&
                !(switchSdExtArgument(fno, uint32_t(page) * 512 + offset, length) & (1u << 26)));
    }
    REQUIRE(!switchSdExtRange(0, 0) && !switchSdExtRange(0, 513) && !switchSdExtRange(300, 263) &&
            switchSdExtRange(249, 263) && !switchSdExtRange(0x20000, 1));

    std::vector<uint8_t> info = generalInfo();
    SwitchSdGeneralInfo g = switchSdParseGeneralInfo(info.data(), 512);
    REQUIRE(!g.why && g.extensions == 2 && g.power.found && g.power.fno == 1 && g.power.address == 0 && g.perf.found &&
            g.perf.fno == 2 && g.perf.address == 3 * 512);
    REQUIRE(switchSdCqControlAddress(g.perf) == 3 * 512 + 262);
    // A performance set not at the top of its page: byte 262 would be on the next page, so no CQ address.
    SwitchSdExtFunction shifted = g.perf;
    shifted.address = 3 * 512 + 250;
    REQUIRE(!switchSdCqControlAddress(shifted));
    shifted.address = 3 * 512 + 249;
    REQUIRE(switchSdCqControlAddress(shifted) == 3 * 512 + 511);
    REQUIRE(!switchSdCqControlAddress(SwitchSdExtFunction{}));
    // r16-sdcq: the host bytes the driver may write, and only these values: Cache Enable 00h/01h, Flush Cache 01h, the
    // CQ control byte 00h/03h; the CMD48/CMD49 argument of each (MIO 0, FNO 2, page 3, LEN 0: one byte).
    REQUIRE(switchSdPerfWriteAddress(g.perf, kSdPerfCacheControl, 0) == 3 * 512 + 260);
    REQUIRE(switchSdPerfWriteAddress(g.perf, kSdPerfCacheControl, kSdCacheEnable) == 3 * 512 + 260);
    REQUIRE(switchSdPerfWriteAddress(g.perf, kSdPerfFlushControl, kSdFlushCache) == 3 * 512 + 261);
    REQUIRE(switchSdPerfWriteAddress(g.perf, kSdPerfCqControl, 0) == 3 * 512 + 262);
    REQUIRE(switchSdPerfWriteAddress(g.perf, kSdPerfCqControl, kSdCqEnable | kSdCqSequential) == 3 * 512 + 262);
    REQUIRE(!switchSdPerfWriteAddress(g.perf, kSdPerfFlushControl, 0));         // the card clears it, never the host
    REQUIRE(!switchSdPerfWriteAddress(g.perf, kSdPerfCqControl, kSdCqEnable));  // voluntary mode: never
    REQUIRE(!switchSdPerfWriteAddress(g.perf, kSdPerfCacheControl, 2));
    REQUIRE(!switchSdPerfWriteAddress(g.perf, 257, 1) && !switchSdPerfWriteAddress(g.perf, 258, 0));
    REQUIRE(!switchSdPerfWriteAddress(shifted, kSdPerfCacheControl, 0) || switchSdCqControlAddress(shifted));
    REQUIRE(!switchSdPerfWriteAddress(SwitchSdExtFunction{}, kSdPerfCqControl, 0));
    REQUIRE(switchSdExtArgument(2, 3 * 512 + 261, 1) == (2u << 27 | (3u * 512 + 261) << 9));
    REQUIRE(!(switchSdExtArgument(2, 3 * 512 + 262, 1) & (1u << 31 | 1u << 26)));  // MIO 0 (memory), MW 0
    // Layouts it does not trust.
    REQUIRE(switchSdParseGeneralInfo(generalInfo(1).data(), 512).why);
    info = generalInfo();
    put16(&info[16 + 40], 16);  // points back at itself
    REQUIRE(switchSdParseGeneralInfo(info.data(), 512).why);
    info = generalInfo();
    put16(&info[2], 600);  // continues on page 1
    g = switchSdParseGeneralInfo(info.data(), 512);
    REQUIRE(g.why && !g.perf.found);
    REQUIRE(!switchSdParseGeneralInfo(info.data(), 1024).why);
    info = generalInfo();
    put16(&info[2], 100);  // the second extension does not fit
    REQUIRE(switchSdParseGeneralInfo(info.data(), 512).why);
    info = generalInfo();
    info[64 + 42] = 2;  // two register sets: skipped, as Linux does
    g = switchSdParseGeneralInfo(info.data(), 512);
    REQUIRE(!g.why && g.power.found && !g.perf.found);
    info = generalInfo();
    info[4] = 0;
    g = switchSdParseGeneralInfo(info.data(), 512);
    REQUIRE(!g.why && !g.power.found && !g.perf.found);

    // Power: off notification at byte 1 bit 4.
    uint8_t power[2] = {0x10, 0x30};
    const SwitchSdPower p = switchSdParsePower(power);
    REQUIRE(p.revision == 0 && p.offNotification && p.sustenance && !p.powerDown);

    // Performance register set (Table 5-30).
    std::vector<uint8_t> perf(512, 0);
    perf[1] = 1;
    perf[2] = 3;
    perf[4] = 1;
    perf[6] = 0x1f;
    perf[260] = 1;
    perf[262] = kSdCqEnable | kSdCqSequential;
    SwitchSdPerf f = switchSdParsePerf(perf.data());
    REQUIRE(f.fxEvent && f.cardMaintenance && f.hostMaintenance && f.cache && f.cqDepth == 32 && f.cacheEnable == 1 &&
            f.cqControl == 3 && !f.taskErrors);
    perf[6] = 0x20;  // reserved: no queue
    REQUIRE(switchSdParsePerf(perf.data()).cqDepth == 0);
    perf[6] = 0;
    REQUIRE(switchSdParsePerf(perf.data()).cqDepth == 0);
    perf[6] = 1;
    perf[9] = 0x40;
    f = switchSdParsePerf(perf.data());
    REQUIRE(f.cqDepth == 2 && f.taskErrors == 0x40);
}

// The data command framing issue() programs: without CMD23 exactly what every command used before r14-sdcq; with CMD23,
// Auto CMD23 (TRANSFER bits 3:2 = 10b) and the block count in Argument 2, no stop command.
static void framing()
{
    for (uint32_t sectors = 1; sectors <= 2048; ++sectors) {
        for (int write = 0; write < 2; ++write) {
            const bool multi = sectors > 1;
            const SwitchSdFrame old = switchSdBlockFrame(sectors, write, false);
            REQUIRE(old.transfer == (1 | (multi ? 0x26 : 0) | (write ? 0 : 0x10)));
            REQUIRE(old.command == (((write ? (multi ? 25 : 24) : (multi ? 18 : 17)) << 8) | 0x3a));
            REQUIRE(old.argument2 == 0 && old.autoStop == multi);
            const SwitchSdFrame with = switchSdBlockFrame(sectors, write, true);
            REQUIRE(with.command == old.command);
            if (!multi) {
                // A single block never has an auto command either way.
                REQUIRE(with.transfer == old.transfer && with.argument2 == 0 && !with.autoStop);
                continue;
            }
            REQUIRE(with.transfer == ((old.transfer & ~kSdTransferAutoCmd12) | kSdTransferAutoCmd23));
            REQUIRE(!(with.transfer & kSdTransferAutoCmd12) && with.argument2 == sectors && !with.autoStop);
        }
    }
    // The register values a 4 KiB read and a 1 MiB write program.
    SwitchSdFrame f = switchSdBlockFrame(8, false, true);
    REQUIRE(f.transfer == 0x3b && f.command == 0x123a && f.argument2 == 8);
    f = switchSdBlockFrame(8, false, false);
    REQUIRE(f.transfer == 0x37 && f.command == 0x123a && f.argument2 == 0 && f.autoStop);
    f = switchSdBlockFrame(2048, true, true);
    REQUIRE(f.transfer == 0x2b && f.command == 0x193a && f.argument2 == 2048);
    f = switchSdBlockFrame(1, true, true);
    REQUIRE(f.transfer == 0x01 && f.command == 0x183a && f.argument2 == 0);
    // r14-sdcq: a command-queue task's CMD46: a read, DMA, the block count, no auto command, never a write.
    for (uint32_t sectors = 1; sectors <= 512; ++sectors) {
        const SwitchSdFrame t = switchSdTaskFrame(sectors);
        REQUIRE(t.command == 0x2e3a && t.argument2 == 0 && !t.autoStop);
        REQUIRE(!(t.transfer & (kSdTransferAutoCmd12 | kSdTransferAutoCmd23)) && (t.transfer & kSdTransferRead) &&
                (t.transfer & kSdTransferDma));
        REQUIRE(t.transfer == (sectors > 1 ? 0x33 : 0x11));
    }
}

TEST(SdCard, RegisterFields)
{
    fields();
}

TEST(SdCard, Identity)
{
    identity();
}

TEST(SdCard, ExtensionRegisters)
{
    extension();
}

TEST(SdCard, BlockFraming)
{
    framing();
}
