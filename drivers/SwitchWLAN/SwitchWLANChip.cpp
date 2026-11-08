// SPDX-License-Identifier: BSD-2-Clause-Patent
// The BCM4356A3 behind the PCIe link: its backplane through BAR0, the cores its enumeration ROM lists, their AI
// wrapper resets, and the firmware download into its RAM through BAR2.
//
// Ported from MyEcoria/nxk user/wlan/src/chip.rs and fw.rs (MIT OR Apache-2.0), themselves Linux brcmfmac
// chip.c/pcie.c; checked against CTCaer's switch-l4t-kernel-4.9 brcmfmac (the driver switchroot runs on this
// chip): brcmf_chip_dmp_erom_scan, brcmf_chip_cr4_set_passive/active, brcmf_chip_tcm_ramsize/rambase (0x180000
// for the 4356), brcmf_pcie_download_fw_nvram, brcmf_pcie_adjust_ramsize, brcmf_fw_nvram_strip.
// brcmf_pcie_reset_device's ChipCommon watchdog reset is not used: TegraPcie power-cycles WL_REG_ON instead.
#include <IOKit/IOLib.h>
#include "SwitchWLANPrivate.h"

namespace swlan {
namespace {

constexpr uint32_t kBar0Window = 0x80;        // config register: backplane address BAR0's 4 KiB view starts at
constexpr uint32_t kChipCommon = 0x18000000;  // chipid at +0, enumeration ROM pointer at +0xFC
constexpr uint16_t kCoreArmCr4 = 0x83E, kCorePcie2 = 0x83C, kCore80211 = 0x812, kCorePmu = 0x827, kCoreGci = 0x840;

// Enumeration ROM descriptors (brcmf_chip_dmp_*).
constexpr uint32_t kDescType = 0xF, kDescValid = 0x1, kDescComponent = 0x1, kDescMasterPort = 0x3;
constexpr uint32_t kDescAddress = 0x5, kDescAddrsizeGt32 = 0x8, kDescEot = 0xF;
constexpr uint32_t kSlaveSlave = 0, kSlaveSwrap = 2, kSlaveMwrap = 3, kSize4K = 0, kSize8K = 1, kSizeDesc = 3;
constexpr uint32_t kEromWords = 1024;  // the ROM is 4 KiB: a bound whatever it contains

// AI wrapper registers and core control bits.
constexpr uint32_t kAiIoctl = 0x408, kAiResetCtl = 0x800, kAiReset = 1u << 0;
constexpr uint32_t kIoctlClk = 1u << 0, kIoctlFgc = 1u << 1, kCr4CpuHalt = 1u << 5;
constexpr uint32_t kD11PhyClockEn = 1u << 2, kD11PhyReset = 1u << 3;
// ARM CR4 TCM bank registers.
constexpr uint32_t kCr4Cap = 0x04, kCr4BankIdx = 0x40, kCr4BankInfo = 0x44, kCr4Bank1K = 1u << 9;
// PCIe2 core.
constexpr uint32_t kPcie2MailboxInt = 0x48, kPcie2MailboxMask = 0x4C, kPcie2ConfigAddr = 0x120;
constexpr uint32_t kPcie2ConfigData = 0x124;

// Shared area (brcmfmac BRCMF_SHARED_*; DHD pciedev_shared_t): trap record, assert expression/file/line, console.
constexpr uint32_t kSharedTrap = 4, kSharedAssertExp = 8, kSharedAssertFile = 12, kSharedAssertLine = 16;
constexpr uint32_t kSharedConsole = 20, kSharedAssert = 0x200, kSharedTrapFlag = 0x400;
constexpr uint32_t kSharedVersionMin = 5, kSharedVersionMax = 5;  // the only layout CTCaer's brcmfmac knows
constexpr uint32_t kSharedDmaIndex = 0x10000;
constexpr uint32_t kBootTimeoutMs = 5000;

}  // namespace

bool Chip::attach(TegraPcie *pcie, uint32_t bar0PA, uint32_t bar2PA, char *why, size_t whyLength)
{
    pcie_ = pcie;
    window_ = ~0u;
    // Mapped once: a chip brought back after a reset is enumerated to the same BARs (TegraPcie::restart).
    if ((!bar0_.base && !bar0_.map(bar0PA, 0x1000)) || (!tcm_.base && !tcm_.map(bar2PA, 0x400000))) {
        snprintf(why, whyLength, "BAR mapping failed");
        return false;
    }
    const uint32_t chipid = read(kChipCommon);
    id_ = static_cast<uint16_t>(chipid);
    rev_ = static_cast<uint8_t>((chipid >> 16) & 0xF);
    if (chipid == 0xFFFFFFFF || id_ != 0x4356) {
        char state[200];
        pcie_->describe(state, sizeof(state));
        snprintf(why, whyLength, "chip id %08x, expected a BCM4356 [%s]", chipid, state);
        return false;
    }
    scanCores();
    const Core *arm = find(kCoreArmCr4);
    if (!arm || !find(kCorePcie2) || !find(kCore80211)) {
        snprintf(why, whyLength, "%u cores, ARM CR4/PCIe2/802.11 missing", coreCount_);
        return false;
    }
    ramBase_ = 0x180000;
    ramSize_ = 0;
    const uint32_t cap = read(arm->base + kCr4Cap);
    const uint32_t banks = (cap & 0xF) + ((cap >> 4) & 0xF);
    for (uint32_t i = 0; i < banks; ++i) {
        write(arm->base + kCr4BankIdx, i);
        const uint32_t info = read(arm->base + kCr4BankInfo);
        ramSize_ += ((info & 0x7F) + 1) * ((info & kCr4Bank1K) ? 1024 : 8192);
    }
    return true;
}

uint32_t Chip::read(uint32_t address)
{
    const uint32_t window = address & ~0xFFFu;
    if (window != window_) {
        pcie_->epConfigWrite(kBar0Window, window);
        window_ = window;
    }
    return bar0_.r32(address & 0xFFF);
}

void Chip::write(uint32_t address, uint32_t value)
{
    const uint32_t window = address & ~0xFFFu;
    if (window != window_) {
        pcie_->epConfigWrite(kBar0Window, window);
        window_ = window;
    }
    bar0_.w32(address & 0xFFF, value);
}

const Core *Chip::find(uint16_t coreId) const
{
    for (unsigned i = 0; i < coreCount_; ++i) {
        if (cores_[i].id == coreId)
            return &cores_[i];
    }
    return nullptr;
}

// Next descriptor and its type, address descriptors of any width folded into one type.
uint32_t Chip::erom(uint32_t &at, uint32_t &type)
{
    const uint32_t value = read(at);
    at += 4;
    type = value & kDescType;
    if ((type & ~kDescAddrsizeGt32) == kDescAddress)
        type = kDescAddress;
    return value;
}

// brcmf_chip_dmp_get_regaddr: the first 4/8 KiB register slave and the wrapper of the component being scanned.
bool Chip::regaddr(uint32_t &at, uint32_t &base, uint32_t &wrap)
{
    uint32_t type = 0;
    uint32_t wraptype = 0;
    (void)erom(at, type);
    if (type == kDescMasterPort) {
        wraptype = kSlaveMwrap;
    } else if (type == kDescAddress) {
        at -= 4;
        wraptype = kSlaveSwrap;
    } else {
        at -= 4;
        return false;
    }
    base = wrap = 0;
    while (!base || !wrap) {
        uint32_t value = 0;
        do {
            value = erom(at, type);
            if (type == kDescEot) {
                at -= 4;
                return false;
            }
        } while (type != kDescAddress && type != kDescComponent);
        if (type == kDescComponent) {
            at -= 4;
            break;
        }  // the next component starts: keep what this one had
        if (value & kDescAddrsizeGt32)
            (void)erom(at, type);
        const uint32_t size = (value >> 4) & 3;
        if (size == kSizeDesc) {
            uint32_t sizeType = 0;
            if (erom(at, sizeType) & kDescAddrsizeGt32)
                (void)erom(at, sizeType);
        }
        if (size != kSize4K && size != kSize8K)
            continue;
        const uint32_t slaveType = (value >> 6) & 3;
        if (!base && slaveType == kSlaveSlave)
            base = value & ~0xFFFu;
        if (!wrap && slaveType == wraptype)
            wrap = value & ~0xFFFu;
    }
    return true;
}

// brcmf_chip_dmp_erom_scan.
void Chip::scanCores()
{
    coreCount_ = 0;
    uint32_t at = read(kChipCommon + 0xFC);
    const uint32_t end = at + 4 * kEromWords;
    while (at < end && coreCount_ < sizeof(cores_) / sizeof(cores_[0])) {
        uint32_t type = 0;
        uint32_t value = erom(at, type);
        if (type == kDescEot)
            break;
        if (!(value & kDescValid) || type != kDescComponent)
            continue;
        const uint16_t id = static_cast<uint16_t>((value >> 8) & 0xFFF);
        value = erom(at, type);
        if (type != kDescComponent)
            break;
        const uint32_t masters = (value >> 14) & 0x1F;
        const uint32_t slaves = (value >> 19) & 0x1F;
        if (!(masters + slaves) && id != kCorePmu && id != kCoreGci)
            continue;
        uint32_t base = 0;
        uint32_t wrap = 0;
        if (regaddr(at, base, wrap))
            cores_[coreCount_++] = {id, static_cast<uint8_t>(value >> 24), base, wrap};
    }
}

// brcmf_chip_ai_coredisable: core held in reset with `reset` in its control bits.
void Chip::coreDisable(const Core &core, uint32_t prereset, uint32_t reset)
{
    if (!(read(core.wrap + kAiResetCtl) & kAiReset)) {
        write(core.wrap + kAiIoctl, prereset | kIoctlFgc | kIoctlClk);
        (void)read(core.wrap + kAiIoctl);
        write(core.wrap + kAiResetCtl, kAiReset);
        IODelay(20);
        for (int i = 0; i < 30 && read(core.wrap + kAiResetCtl) != kAiReset; ++i)
            IODelay(10);
    }
    write(core.wrap + kAiIoctl, reset | kIoctlFgc | kIoctlClk);
    (void)read(core.wrap + kAiIoctl);
}

// brcmf_chip_ai_resetcore: reset pulse, then running with `postreset`.
void Chip::coreReset(const Core &core, uint32_t prereset, uint32_t reset, uint32_t postreset)
{
    coreDisable(core, prereset, reset);
    for (int i = 0; i < 50 && (read(core.wrap + kAiResetCtl) & kAiReset); ++i) {
        write(core.wrap + kAiResetCtl, 0);
        IODelay(50);
    }
    write(core.wrap + kAiIoctl, postreset | kIoctlClk);
    (void)read(core.wrap + kAiIoctl);
}

// Bytes into RAM through BAR2, a word at a time, the tail padded with zeros.
void Chip::tcmCopy(uint32_t address, const uint8_t *data, size_t length)
{
    for (size_t at = 0; at < length; at += 4) {
        uint8_t word[4] = {};
        for (size_t i = 0; i < 4 && at + i < length; ++i)
            word[i] = data[at + i];
        tcm_.w32(static_cast<uint32_t>(address + at), le32(word));
    }
}

bool Chip::boot(const uint8_t *image, size_t imageLength, const uint8_t *nvram, size_t nvramLength, char *why,
                size_t whyLength)
{
    const Core *arm = find(kCoreArmCr4);
    const Core *pcie2 = find(kCorePcie2);
    const Core *d11 = find(kCore80211);
    // A RAM size recorded in the image ("SMAR" at 0x6c) wins over the banks (brcmf_pcie_adjust_ramsize).
    if (imageLength >= 0x6c + 8 && le32(image + 0x6c) == 0x534D4152)
        ramSize_ = le32(image + 0x70);
    if (imageLength < 4 || imageLength + nvramLength > ramSize_) {
        snprintf(why, whyLength, "%zu + %zu bytes do not fit %u of RAM", imageLength, nvramLength, ramSize_);
        return false;
    }
    // brcmf_chip_cr4_set_passive: ARM halted (only its HALT bit kept), radio core reset with its PHY clock on.
    coreReset(*arm, read(arm->wrap + kAiIoctl) & kCr4CpuHalt, kCr4CpuHalt, kCr4CpuHalt);
    coreReset(*d11, kD11PhyReset | kD11PhyClockEn, kD11PhyClockEn, kD11PhyClockEn);
    // brcmf_pcie_attach: config word 0x4E0 re-latched (it sizes the BAR1 window); brcmf_pcie_buscore_reset:
    // stale mailbox interrupts cleared; and the mailbox masked until the host serves it.
    write(pcie2->base + kPcie2ConfigAddr, 0x4E0);
    write(pcie2->base + kPcie2ConfigData, read(pcie2->base + kPcie2ConfigData));
    write(pcie2->base + kPcie2MailboxMask, 0);
    const uint32_t stale = read(pcie2->base + kPcie2MailboxInt);
    if (stale && stale != 0xFFFFFFFF)
        write(pcie2->base + kPcie2MailboxInt, stale);

    const uint64_t t0 = nowNs();
    const uint32_t top = ramBase_ + ramSize_ - 4;
    tcmCopy(ramBase_, image, imageLength);
    // The last RAM word ends the NVRAM (its length token); the firmware replaces it with its shared area's address.
    tcm_.w32(top, 0);
    tcmCopy(static_cast<uint32_t>(ramBase_ + ramSize_ - nvramLength), nvram, nvramLength);
    const uint32_t token = tcm_.r32(top);
    tcm_.w32(0, le32(image));            // reset vector (brcmf_pcie_buscore_activate)
    coreReset(*arm, kCr4CpuHalt, 0, 0);  // brcmf_chip_cr4_set_active

    uint32_t shared = token;
    for (uint32_t waited = 0; shared == token && waited < kBootTimeoutMs; waited += 50) {
        IOSleep(50);
        shared = tcm_.r32(top);
    }
    if (shared == token) {
        snprintf(why, whyLength, "firmware silent after %u ms (RAM %u KiB at %#x, ARM ioctl %08x reset %08x)",
                 kBootTimeoutMs, ramSize_ / 1024, ramBase_, read(arm->wrap + kAiIoctl), read(arm->wrap + kAiResetCtl));
        return false;
    }
    if (shared < ramBase_ || shared >= ramBase_ + ramSize_) {
        snprintf(why, whyLength, "shared area at %#x, outside RAM", shared);
        return false;
    }
    shared_ = shared;
    sharedFlags_ = tcm_.r32(shared);
    console_ = tcm_.r32(shared + kSharedConsole);
    const uint32_t version = sharedFlags_ & 0xFF;
    if (version < kSharedVersionMin || version > kSharedVersionMax || (sharedFlags_ & kSharedDmaIndex)) {
        snprintf(why, whyLength, "shared area v%u flags %#x: only v5 with indices in RAM is handled", version,
                 sharedFlags_);
        return false;
    }
    const uint64_t ms = (nowNs() - t0) / 1000000;
    IOLog("SwitchWLAN: firmware up in %llu ms (shared v%u at %#x, flags %#x, RAM %u KiB)\n", ms, version, shared,
          sharedFlags_, ramSize_ / 1024);
    return true;
}

void Chip::crash(char *out, size_t length)
{
    const auto text = [this](uint32_t address, char *buffer, size_t size) {
        size_t n = 0;
        for (uint32_t k = 0; k < 96 && n + 1 < size && address >= ramBase_ && address + k < ramBase_ + ramSize_; ++k) {
            const uint8_t c = tcm_.r8(address + k);
            if (!c)
                break;
            if (c >= 0x20 && c < 0x7F)
                buffer[n++] = static_cast<char>(c);
        }
        buffer[n] = 0;
    };
    const uint32_t flags = shared_ ? tcm_.r32(shared_) : 0;
    size_t used = 0;
    const auto append = [&](int written) {
        if (written > 0)
            used += static_cast<size_t>(written);
        if (used >= length)
            used = length - 1;
    };
    append(snprintf(out, length, "shared flags %#x", flags));
    if (flags & kSharedAssert) {
        char expression[100];
        char file[100];
        text(tcm_.r32(shared_ + kSharedAssertExp), expression, sizeof(expression));
        text(tcm_.r32(shared_ + kSharedAssertFile), file, sizeof(file));
        append(snprintf(out + used, length - used, ", assert \"%s\" at %s:%u", expression, file,
                        tcm_.r32(shared_ + kSharedAssertLine)));
    }
    if (flags & kSharedTrapFlag) {
        const uint32_t trap = tcm_.r32(shared_ + kSharedTrap);
        if (trap >= ramBase_ && trap + 80 <= ramBase_ + ramSize_) {
            // trap_t: type, epc, cpsr, spsr, r0..r15 (sp = r13, lr = r14, pc = r15).
            append(snprintf(out + used, length - used,
                            ", trap %#x pc %#x epc %#x lr %#x sp %#x cpsr %#x r0-r3 %#x %#x %#x %#x", tcm_.r32(trap),
                            tcm_.r32(trap + 76), tcm_.r32(trap + 4), tcm_.r32(trap + 72), tcm_.r32(trap + 68),
                            tcm_.r32(trap + 8), tcm_.r32(trap + 16), tcm_.r32(trap + 20), tcm_.r32(trap + 24),
                            tcm_.r32(trap + 28)));
        }
    }
}

// The newest printable bytes of the firmware's console ring (lines joined by " | "): what it said last.
void Chip::consoleTail(char *out, size_t length)
{
    out[0] = 0;
    if (!console_ || console_ < ramBase_ || console_ >= ramBase_ + ramSize_ || length < 2)
        return;
    const uint32_t buffer = tcm_.r32(console_ + 8);
    const uint32_t size = tcm_.r32(console_ + 12);
    const uint32_t index = tcm_.r32(console_ + 16);
    if (!size || size > 0x10000 || index >= size || buffer < ramBase_ || buffer + size > ramBase_ + ramSize_) {
        snprintf(out, length, "(console unreadable: buf %#x size %u index %u)", buffer, size, index);
        return;
    }
    const uint32_t want = static_cast<uint32_t>(length - 1) < size ? static_cast<uint32_t>(length - 1) : size;
    size_t n = 0;
    for (uint32_t k = size - want; k < size && n + 4 < length; ++k) {
        const uint8_t c = tcm_.r8(buffer + (index + k) % size);
        if (c == '\n') {
            if (n) {
                out[n++] = ' ';
                out[n++] = '|';
                out[n++] = ' ';
            }
        } else if (c >= 0x20 && c < 0x7F) {
            out[n++] = static_cast<char>(c);
        }
    }
    out[n] = 0;
}

size_t nvramConvert(const uint8_t *text, size_t textLength, uint8_t *out, size_t outLength)
{
    const auto printable = [](uint8_t c) {
        return c >= 0x20 && c < 0x7F && c != '#';
    };
    size_t n = 0;
    bool boardrev = false;
    size_t i = 0;
    while (i < textLength) {
        // One line: skip blanks, a `key=value` (value up to the first non-printable or '#'), the rest ignored.
        size_t lineEnd = i;
        while (lineEnd < textLength && text[lineEnd] != '\n')
            ++lineEnd;
        size_t at = i;
        while (at < lineEnd && (text[at] == ' ' || !printable(text[at])) && text[at] != '#')
            ++at;
        const size_t start = at;
        while (at < lineEnd && printable(text[at]) && text[at] != '=' && text[at] != ' ')
            ++at;
        if (at < lineEnd && text[at] == '=' && at > start) {
            while (at < lineEnd && printable(text[at]))
                ++at;
            const size_t entry = at - start;
            if (entry < 4 || memcmp(text + start, "RAW1", 4)) {  // raw OTP dumps are not variables
                if (n + entry + 1 > outLength)
                    return 0;
                if (entry >= 9 && !memcmp(text + start, "boardrev=", 9))
                    boardrev = true;
                memcpy(out + n, text + start, entry);
                n += entry;
                out[n++] = 0;
            }
        }
        i = lineEnd + 1;
    }
    static const char kBoardrev[] = "boardrev=0xff";
    if (!boardrev) {
        if (n + sizeof(kBoardrev) > outLength)
            return 0;
        memcpy(out + n, kBoardrev, sizeof(kBoardrev));
        n += sizeof(kBoardrev);
    }
    // At least one more NUL, padded to a word, then the length token: words in the low half, its complement high.
    const size_t padded = (n + 1 + 3) & ~static_cast<size_t>(3);
    if (padded + 4 > outLength)
        return 0;
    while (n < padded)
        out[n++] = 0;
    const uint32_t words = static_cast<uint32_t>(padded / 4);
    put32(out + n, (~words << 16) | (words & 0xFFFF));
    return n + 4;
}

}  // namespace swlan
