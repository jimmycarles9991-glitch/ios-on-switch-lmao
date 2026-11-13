// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 SDMMC1 raw block device: exposes the SD card's single APFS GPT
// partition as a whole-disk IOBlockStorageDevice and lets IOKit/APFS do the rest.
//
// The card is inherited from U-Boot already initialized, selected and in transfer
// state, at 3.3 V high speed, 4-bit, 48 MHz (~24 MB/s raw): switchroot U-Boot's
// tegra_mmc.c caps the bus at f_max 48 MHz with only HS host caps, and IMG_5553 read
// back hc=0x02 clock=0x0007 (4-bit, CAR clock passed straight through). Unless boot-arg
// switch-no-sd-uhs keeps that mode, setup() takes the card back through SD initialization
// into UHS-I SDR104 (1.8 V, 199.68 MHz, hardware-tuned, ~100 MB/s raw; slower tuned clocks if
// that has no sampling window) the way hekate's bdk does on this SoC. Data moves by SDHCI v4 ADMA2
// straight between the card and the requests' own pages (SwitchAdma.h), or by SDMA through
// one physically contiguous bounce buffer for what ADMA2 cannot take (and for everything if
// setup()'s ADMA2 check fails), CMD18/CMD25 with Auto CMD12, with
// queued requests that continue each other on the card merged into one command;
// completion is signalled by the SDMMC1 interrupt (an interrupt that cannot be had or
// never arrives panics) on a
// dedicated worker thread so callers never block or recurse.
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <os/log.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <sys/types.h>
#include <sys/param.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraI2C/TegraI2C.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchTime/SwitchTime.h"
#include "../SwitchBrightness/SwitchBrightness.h"
#include "../SwitchPower/SwitchPower.h"
#include "block_backend.h"
#include "NativeSwitchBlock.h"
#include "SwitchClientCopy.h"
#include "SwitchAdma.h"
#include "SwitchQueue.h"
#include "SwitchSdBus.h"
#include "SwitchSdCard.h"
#include "SwitchSdTrim.h"
#include "SwitchCq.h"

extern "C" {
// The ADMA2 path's page checks and cache maintenance (osfmk/arm/pmap/pmap.c, arm64/arm_vm_init.c): a page's cache
// attributes (VM_WIMG_DEFAULT 0x2, osfmk/arm/memory_types.h: write-back cacheable) and its address in the kernel's
// physical aperture.
unsigned int pmap_cache_attributes(ppnum_t pn);
vm_offset_t phystokv(uint64_t pa);
extern uint64_t gPhysBase, gPhysSize;  // the RAM phystokv covers (it panics on anything else)
}

namespace {
constexpr uint64_t kControllerPA = 0x700B0000;  // SDMMC1
constexpr uint64_t kMemoryControllerPA = 0x70019000;
// What the SDR104 bring-up touches besides the controller (hekate bdk/soc/t210.h:47/58/64): the
// clock and reset controller, APB_MISC (pinmux at +0x3000) and the PMC's page (PMC is at 0x7000E400).
constexpr uint64_t kCarPA = 0x60006000, kApbMiscPA = 0x70000000, kPmcPagePA = 0x7000E000;
// SDMMC1's interrupt: GIC_SPI 14, level-high (mainline tegra210.dtsi mmc@700b0000; Atmosphere uses 46 too).
constexpr uint32_t kInterrupt = 46;
// Longest single interrupt wait before the worker checks STATUS for a lost interrupt (see command()).
constexpr uint64_t kInterruptCheckNs = 100000000;
// The longest a write's programming or an R1b's busy may hold DAT0 once its own transfer complete came (awaitRelease).
constexpr uint64_t kReleaseLimitNs = 2000000000ULL;
constexpr uint32_t kSector = 512;
// r8-sdboot: one physically contiguous 1 MiB bounce buffer, aligned to the 512 KiB SDMA boundary. A transfer of more
// than 512 KiB pauses at each boundary with the DMA interrupt and resumes when the next address is written (command(),
// as Linux sdhci_data_irq does). setup() checks that on this controller with a 1 MiB read against four 256 KiB ones
// (which never reach a boundary) before allowing more than kSafeSectors per command; otherwise commands stay at
// 256 KiB as before. Larger commands halve or quarter the per-command card latency, copy and completion turn-around
// that held 11-04's 512 KiB prefetch runs (two 256 KiB commands each) to ~67 MB/s on a ~95 MB/s bus.
// Writes stay at kSafeSectors: the check only proves the boundary resume for reads, so nothing larger is ever written
// until a write check exists (a 256 KiB write from the start of the boundary-aligned bounce never reaches a boundary).
constexpr uint32_t kBounceBytes = 1024 * 1024;
constexpr uint32_t kBounceAlign = 512 * 1024;
constexpr uint32_t kSdmaBoundary = 512 * 1024;
constexpr uint32_t kBounceSectors = kBounceBytes / kSector;
constexpr uint32_t kSafeSectors = 256 * 1024 / kSector;
// Most requests one merged command carries (APFS issues >= 4 KiB, so 256 still fills the buffer).
constexpr uint32_t kMaxChain = 256;
// r12-sdio: how many queued requests a read chain looks through for reads that continue it (SwitchQueue.h): as many as
// one chain may hold, so it never takes fewer than the old rule did from a queue of reads right behind each other.
constexpr unsigned kMergeScan = kMaxChain;
// r10-adma2: with ADMA2, reads go up to kBounceSectors per command (the cap SDMA had once its boundary check passed) and
// writes up to kDirectWriteSectors. The first kWriteChecks ADMA2 writes of every boot are read back and compared
// (proveWrite). r12-sdio: writes up to 1 MiB too. The 256 KiB cap existed for the SDMA boundary resume (0d184ec), which
// ADMA2 never uses; r7p5 verified its 32 ADMA2 writes, and a longer ADMA2 write differs only in its block count and
// descriptor count. Still, the first kLargeWriteChecks writes over 256 KiB of every boot are read back too; one that
// differs is written again through the bounce buffer and writes longer than 256 KiB take the bounce path's 256 KiB SDMA
// commands from then on (writeSectors; the advertised maximum cannot shrink once IOBlockStorageDriver has read it).
constexpr uint32_t kDirectWriteSectors = kBounceSectors;
constexpr uint32_t kWriteChecks = 32, kLargeWriteChecks = 16;
// r14-sdcq: multi-block ADMA2 commands announce their length with Auto CMD23 (SET_BLOCK_COUNT) instead of ending with
// Auto CMD12, once setup()'s check (checkCmd23) has read kCmd23Pairs 4 KiB blocks both ways with equal bytes. The first
// kCmd23WriteChecks such writes of every boot are read back as proveWrite does for ADMA2's; one that differs is written
// again through the bounce buffer (CMD12 framing) and writes go back to CMD12 until reboot.
constexpr uint32_t kCmd23Pairs = 16, kCmd23WriteChecks = 16;
// r14-sdcq: a read chain also takes queued reads that start up to kGapSectors past its end (or end that far before its
// start), and the card reads straight through the gap into a sink nobody reads (SwitchQueue.h). On r7r a command cost
// ~0.58 ms plus ~10 us per KiB moved (r12-sdio's model, 4 KiB reads at 0.62 ms): a 32 KiB gap is ~0.32 ms of transfer
// against ~0.58 ms for the separate command it saves, so even the longest gap taken saves ~0.26 ms; kGapBudget keeps one
// command's read-through to 64 KiB (~0.64 ms, against ~1.16 ms for the two commands its two longest gaps save). The first
// kGapProofs such chains of every boot are read again by SDMA and compared before they complete (proveGaps).
constexpr uint32_t kGapSectors = 32 * 1024 / kSector, kGapBudget = 64 * 1024 / kSector, kGapProofs = 8;
// r14-sdcq: the card's command queue (SwitchCq.h), for reads. A task is at most kCqTaskSectors (256 KiB: what one SDMA
// command into the bounce buffer moves without reaching its 512 KiB boundary, so a task whose pages cannot go straight
// still goes in one CMD46); a group stops taking chains at kCqGroupSectors (1 MiB, one legacy command's worth: a page
// fault queued behind a group waits about as long as behind one large read) or at the card's depth (kCqMaxTasks at
// most). The first kCqProofs tasks of every boot are read again the legacy way and compared before they complete.
constexpr uint32_t kCqTaskSectors = kSafeSectors, kCqGroupSectors = kBounceSectors, kCqMaxTasks = 32, kCqProofs = 32;
// issue()'s `task` when the command is not a CQ task's CMD46.
constexpr uint32_t kNoTask = 0xffffffffu;

// What a read's pages hold before the card writes them (prepareDirect). kNone: nothing to keep (the data path), so dirty
// lines are dropped. kCpuFill: a check's own fill, which must reach DRAM so a byte the engine never delivered shows.
// kPoisonHigh: the same with the pages above 4 GiB filled with kPoison first (proveHigh).
enum class ReadFill : uint8_t {
    kNone,
    kCpuFill,
    kPoisonHigh
};

// cqCommand()'s data: none, one 512-byte block from the card, or to it.
enum : int {
    kRegNone,
    kRegRead,
    kRegWrite
};

// r12-sdio: what a read that proves ADMA2 above 4 GiB fills those pages with first (prepareDirect, proveHigh).
constexpr uint8_t kPoison = 0x5a;
// Unmap (stepUnmap): SD discard (CMD38 argument 1, SD 4.3.5) frees any range with a short busy (Linux allows 250 ms
// whatever its length, mmc_sd_erase_timeout): 1 GiB a command. A discard waits for an idle queue, or for
// kUnmapEveryChains chains if reads and writes keep coming: APFS trims its free space from a thread at every mount.
constexpr uint64_t kDiscardSectors = 1u << 21;
constexpr uint32_t kSdDiscardArgument = 1;
constexpr uint64_t kDiscardLimitNs = 1500000000ULL;
constexpr unsigned kUnmapEveryChains = 8;
constexpr uint64_t kUnmapRefusal = switch_refusal_key("SwitchSD", "discard");
// A card without discard (SwitchSdTrim.h): the AUs APFS unmapped are erased later, one command of at most kTrimRunMs of
// specified busy at a time, only once System Boot Complete has come and the queue has been quiet for kTrimQuietNs
// (kTrimQuietLitNs with the display on), and kTrimBootSectors a boot at most. A request waits behind at most that one
// erase. The cursor survives reboots in NVRAM, so successive boots' budgets cover the whole card.
constexpr uint64_t kTrimRunMs = 1100, kTrimMarginNs = 500000000ULL;
constexpr uint64_t kTrimQuietNs = 10000000000ULL, kTrimQuietLitNs = 60000000000ULL;
constexpr uint64_t kTrimBootSectors = 8ULL << 21;
constexpr uint64_t kTrimCursorEveryAus = 64;
constexpr uint32_t kSdEraseArgument = 0;
constexpr uint64_t kEraseRefusal = switch_refusal_key("SwitchSD", "erase");
constexpr char kTrimCursorVariable[] = "switch-sd-trim-cursor";
// r12-sdio: the most separate stretches of a read chain staged through the bounce buffer (SwitchAdmaStaging; consecutive
// segments of one request make one): one per page of a 1 MiB chain of 4 KiB pages. More: the whole chain bounces.
constexpr uint32_t kStages = 256;
using Staging = SwitchAdmaStaging<IOMemoryDescriptor>;
// Descriptor table: 16 KiB, contiguous, below 4 GiB; 1024 descriptors, four times what a 1 MiB chain of scattered 4 KiB
// pages needs (256). A chain that needs more takes the bounce buffer.
constexpr uint32_t kAdmaDescriptors = 1024;
constexpr uint32_t kAdmaTableBytes = kAdmaDescriptors * sizeof(SwitchAdmaDescriptor);
// command()'s step beyond the SDMA ones: the ADMA engine stopped on an error (ADMA error interrupt): the host's DMA,
// not the card or the bus.
constexpr int kStepAdmaError = 11;
const char kRootUUID[] = SWITCH_OS_VOLUME_UUID;
const uint8_t kApfsType[16] = {0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                               0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac};
// The GPT name install-ios.bin gives the partition, UTF-16LE in the entry's 72-byte name field.
const char kPartitionName[] = "iOS";
// Fixed development identifier consumed by the owner boot-manifest code.
const uint8_t kRootIdentifier[48] = {0xfd, 0x86, 0xb9, 0x5b, 0x70, 0x33, 0xe3, 0x52, 0x84, 0x3f, 0x98, 0xd1,
                                     0x20, 0x4a, 0xde, 0x29, 0x45, 0x9b, 0xf6, 0x08, 0xa7, 0xe1, 0xcc, 0xb1,
                                     0xa9, 0xb0, 0x4a, 0xc9, 0x35, 0x33, 0x29, 0x2e, 0xb3, 0x9d, 0xe7, 0xc3,
                                     0x34, 0xeb, 0xe9, 0x8b, 0x43, 0xd0, 0xdb, 0x47, 0x0f, 0xa1, 0x6e, 0x77};

// SDHCI register offsets and bits.
enum : uint32_t {
    ARGUMENT2 = 0x00,  // SDMA System Address / Argument 2; in v4 mode (SDMA at 0x58) only the Auto CMD23 argument
    BLOCK_SIZE = 0x04,
    BLOCK_COUNT = 0x06,
    ARGUMENT = 0x08,
    TRANSFER = 0x0c,
    COMMAND = 0x0e,
    RESPONSE = 0x10,
    STOP_RESPONSE = 0x1c,  // also RESPONSE word 3 of an R2
    PRESENT = 0x24,
    HOST_CONTROL = 0x28,
    POWER = 0x29,
    CLOCK = 0x2c,
    TIMEOUT = 0x2e,
    RESET = 0x2f,
    STATUS = 0x30,
    STATUS_ENABLE = 0x34,
    SIGNAL_ENABLE = 0x38,
    AUTO_CMD_ERROR = 0x3c,
    HOST_CONTROL2 = 0x3e,
    CAPABILITIES = 0x40,
    CAPABILITIES_HI = 0x44,
    ADMA_ERROR = 0x54,
    DMA_LOW = 0x58,
    DMA_HIGH = 0x5c,
    VERSION = 0xfe,
    // Tegra vendor block (hekate bdk/storage/sdmmc_t210.h:90-119).
    VENDOR_CLOCK = 0x100,
    VENDOR_IO_TRIM = 0x1ac,
    VENDOR_TUNING0 = 0x1c0,
    VENDOR_TUNING1 = 0x1c4,
    PAD_CONTROL = 0x1e0,
    AUTOCAL_CONFIG = 0x1e4,
    AUTOCAL_STATUS = 0x1ec,
    IO_SPARE = 0x1f0,
};

enum : uint32_t {
    INT_COMMAND = 1u << 0,
    INT_TRANSFER = 1u << 1,
    INT_DMA = 1u << 3,
    INT_READ_READY = 1u << 5,
    INT_ERROR = 1u << 15,
    INT_ADMA_ERROR = 1u << 25,
    INT_ERRORS = 0xffff8000u,
    // HOST_CONTROL DMA select (bits 4:3): SDMA, or ADMA2 (with HOST_CONTROL2's v4 + 64-bit addressing: 128-bit
    // descriptors; sdhci.c selects SDHCI_CTRL_ADMA32 for any v4 host).
    DMA_SELECT = 0x18,
    DMA_ADMA2 = 0x10,
    CONTROL2_V4_64BIT = 0x3000,
    // Error signal enables are the upper half of SIGNAL_ENABLE (normal bit 15 is fixed 0 in SDHCI): the error
    // bits STATUS_ENABLE latches, 16..25.
    ERROR_SIGNALS = 0x03ff0000u,
    PRESENT_BUSY = 0x307u,
    PRESENT_DAT = 0xfu << 20,
    PRESENT_CARD = 0x30000u,
    // R1 error bits (Linux's set plus SD AKE_SEQ and CID/CSD overwrite). A stop
    // after reading a card's final block may report OUT_OF_RANGE: ignore it there.
    R1_ERRORS = 0xfdf90008u,
    R1_STOP_ERRORS = 0x7df90008u,
    R1_APP_CMD = 1u << 5,
    // COMMAND low byte per response type: R1/R6/R7 48-bit + CRC + index, R1b the same with busy,
    // R2 136-bit + CRC, R3 48-bit unchecked; DATA marks a data command.
    RSP_NONE = 0x00,
    RSP_R1 = 0x1a,
    RSP_R1B = 0x1b,
    RSP_R2 = 0x09,
    RSP_R3 = 0x02,
    CMD_DATA = 0x20,
    // ACMD41 argument/response (hekate bdk/storage/sd_def.h:91-104).
    OCR_VDD_32_33 = 1u << 20,
    OCR_S18 = 1u << 24,
    OCR_XPC = 1u << 28,
    OCR_CCS = 1u << 30,
    OCR_READY = 1u << 31,
    // HOST_CONTROL2 (hekate sdmmc_driver.h:122-132): UHS mode 2:0, 1.8 V signalling, tuning.
    UHS_SDR12 = 0,
    UHS_SDR25 = 1,
    UHS_SDR104 = 3,
    CONTROL2_1V8 = 1u << 3,
    CONTROL2_EXEC_TUNING = 1u << 6,
    CONTROL2_TUNED = 1u << 7,
};

// Clock and reset controller (hekate bdk/soc/clock.h:25/73/106/107/148/149/177/179/196/197; CLK_L_SDMMC1 = 14
// at :488), APB_MISC/pinmux (t210.h:242, pinmux.h:24/26/30-35/130) and PMC (pmc_t210.h:113-114) offsets.
enum : uint32_t {
    CAR_RST_DEVICES_L = 0x004,
    CAR_RST_DEV_L_SET = 0x300,
    CAR_RST_DEV_L_CLR = 0x304,
    CAR_CLK_SOURCE_SDMMC1 = 0x150,
    CAR_CLK_ENB_L_SET = 0x320,
    CAR_CLK_ENB_L_CLR = 0x324,
    CAR_PLLC4_BASE = 0x5a4,
    CAR_PLLC4_MISC = 0x5a8,
    CAR_SDMMC1 = 1u << 14,
    PLL_LOCK = 1u << 27,
    PLL_ENABLE = 1u << 30,
    PLLC4_IDDQ = 1u << 18,
    PLLC4_LOCK_DETECT = 1u << 30,
    // PLLC4 = 38.4 MHz / M 4 * N 104 = 998.4 MHz, P 1; OUT2 is VCO / 5 = 199.68 MHz (hekate clock.c:602).
    PLLC4_CONFIG = (104u << 8) | 4u,
    PLLC4_DIVIDERS = 0x00f8ffffu,
    // CLK_SOURCE bits 31:29 (hekate clock.c:725-726); PLLP_OUT0 is 408 MHz with a U7.1 divider below.
    SOURCE_PLLP = 0,
    SOURCE_PLLC4_OUT2 = 3,
    PLLP_KHZ = 408000,
    APB_GP_HIDREV = 0x804,
    APB_SDMMC1_CLK_LOOPBACK = 0x8d4,
    APB_SDMMC1_PAD_CONFIG = 0xa98,
    PINMUX_SDMMC1 = 0x3000,
    PINMUX_SCHMITT = 1u << 12,  // CLK, CMD, DAT3, DAT2, DAT1, DAT0 at +0..+0x14
    PMC_PWR_DET_VAL = 0x400 + 0xe4,
    PMC_SDMMC1_3V3 = 1u << 12,
    PMC_NO_IOPOWER = 0x400 + 0x44,
    PMC_SDMMC1_NO_POWER = 1u << 12,
};

// System sleep (hekate sdmmc1_disable_power / _sdmmc_config_sdmmc1 / sdmmc_init): the card's own supply is switched by
// GPIO PE4 (DMIC3_CLK), its I/O supply is LDO2; U-Boot's card clock is PLLP / 8.5 = 48 MHz (CLK_SOURCE U7.1 divider 15),
// and B01's SDMMC1 trimmer is 14 (VENDOR_CLOCK 28:24, hekate trim_values_t210b01).
constexpr unsigned kCardPowerGpio = TEGRA_GPIO(E, 4);
constexpr uint32_t kPllpDivider48MHz = 15, kTrimB01 = 14;
// MAX77620 LDO2 supplies the SDMMC1 pads (vddio_sdmmc1): LDO2_CFG 0x27, bits 5:0 = (uV - 0.8 V) / 50 mV, bits
// 7:6 power mode (hekate power/max77620.h:14/144/179, max7762x.c:83).
constexpr unsigned kPmicBus = 5;
constexpr uint8_t kPmicAddress = 0x3c, kPmicLdo2 = 0x27, kLdoVoltage = 0x3f, kLdo3V3 = 50, kLdo1V8 = 20;
// T210B01 SDMMC1 default sampling tap (hekate sdmmc_driver.c:131; U-Boot tegra_mmc.c:540 the same).
constexpr uint32_t kTapB01 = 11;

// SDR104 clocks, fastest first. The card stays in the SDR104 mode it switched to and the host in SDR104
// timing, re-tuned at each clock, as hekate's own fallback SDR82 does (sdmmc.c:1556-1561 sets the card to
// SDR104, sdmmc_driver.c:340 the host to SDR104 timing, clock.c:772-774 PLLP / 2.5, sdmmc_driver.c:771
// tunes it; sd.c:96-117 steps down to it after errors). Below that, the SDHCI divider halves
// PLLC4_OUT2. carDivider is CLK_SOURCE's U7.1 field (hekate CLK_SRC_DIV(d) = 2 * (d - 1),
// clock.h:202), divider the SDHCI one (base / 2n, 0 = base).
struct Speed {
    uint32_t source, carDivider, divider, kHz;
};

constexpr Speed kSpeeds[] = {{SOURCE_PLLC4_OUT2, 0, 0, 199680},
                             {SOURCE_PLLP, 3, 0, 163200},
                             {SOURCE_PLLC4_OUT2, 0, 1, 99840},
                             {SOURCE_PLLC4_OUT2, 0, 2, 49920}};
constexpr unsigned kSpeedCount = sizeof(kSpeeds) / sizeof(kSpeeds[0]);

inline uint32_t le32(const uint8_t *p)
{
    return p[0] | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

inline uint64_t le64(const uint8_t *p)
{
    return le32(p) | static_cast<uint64_t>(le32(p + 4)) << 32;
}

bool partitionNamed(const uint8_t *entry, const char *name)
{
    const uint8_t *field = entry + 56;
    unsigned i = 0;
    for (; name[i]; ++i) {
        if (i == 36 || field[2 * static_cast<size_t>(i)] != static_cast<uint8_t>(name[i]) || field[2 * i + 1])
            return false;
    }
    return i == 36 || ((field[2 * static_cast<size_t>(i)] == 0u) && (field[2 * i + 1] == 0u));
}

struct Request {
    Request *next;
    IOMemoryDescriptor *buffer;
    uint64_t lba;  // absolute card LBA
    uint32_t sectors;
    bool write;
    IOStorageCompletion completion;
    // r6-lows: the request's IOStoragePriority class (IOStorageAttributes priority >> 6: 0 high (I/O tier 0, the UI's page-ins),
    // 1 default, 2 low, 3 background) and when it was queued.
    uint8_t klass;
    uint64_t queuedNs;
    bool chained;  // r12-sdio: taken into the chain being picked (SwitchQueue.h), under queueLock
};

// One doUnmap call's extents (device blocks), discarded by the worker one command at a time between chains
// (stepUnmap) while its caller waits.
struct Unmap {
    Unmap *next;
    const IOBlockStorageDeviceExtent *extents;
    uint32_t count;
    uint32_t index;    // the next extent to start
    uint64_t at, end;  // what is left of the current one, card LBAs
    IOReturn result;
    bool finished;
};

// r12-sdio: one chain's way through the worker (worker()): picked, begun (beginBatch: the table built and, when the chain is
// one ADMA2 command, its pages synced and the command issued), ended (endBatch: that command waited for, retried and
// checked; or the whole bounce path), then completed (completeBatch). The worker begins the next chain before it completes
// the previous one, so the completion callbacks (IOStorageFamily, APFS, the VM's page-in wake-ups: on r7p5 100-300 us of
// card idle per command while requests queued) run while the card moves the next chain's data, not between commands.
struct Batch {
    Request *first;  // the chain, in card order
    uint32_t sectors, count;
    bool write, direct, proving, issued, staged;
    bool cmd23;  // r14-sdcq: framed with Auto CMD23 (a multi-block ADMA2 command while that is on for its direction)
    uint32_t gapSectors;  // r14-sdcq: of `sectors`, those in gaps between the requests (read and discarded)
    SwitchAdmaRefusal why;
    int issueStep;                // issue()'s result when the command never reached the card
    uint64_t beganNs, overlapNs;  // when beginBatch started; the previous chain's completions that ran inside this one
    IOReturn result;              // endBatch's, for every request of the chain
};

// r6-lows: a high-class read may be taken before older requests of lower classes (worker), never before an older request it
// overlaps, and never once the oldest request has waited kAgeNs (then FIFO again, so nothing starves). Writes keep FIFO order.
constexpr unsigned kClasses = 4;
constexpr uint64_t kAgeNs = 100000000;  // 100 ms

// Queue time (submit to the command that carries it) per class over one throughput line's interval.
struct ClassWait {
    uint64_t n, sumNs, maxNs;
};

struct Stats {
    uint64_t reads, readBytes, writes, writeBytes, errors, retries;
    uint64_t maxLatencyNs;
    uint32_t lastError, lastStatus;
    // Throughput: card commands after merging and the worker time they took (bus, card and copies; the
    // completion callbacks that run on the worker afterwards are completeNs), when the first request
    // arrived, requests that began where the previous one ended (sequential) or rode in another's
    // command (merged), requests of at most 4 KiB, SDMMC1 interrupts taken, SDR104 re-tunes, and the
    // deepest the queue got.
    uint64_t commands, busyNs, completeNs, firstNs, sequential, merged, small, interrupts, retunes;
    // Worker time copying between the bounce buffer and the requests' memory (part of busyNs).
    uint64_t copyNs;
    uint32_t maxQueued;
    // r10-adma2: requests that went straight to their own pages by ADMA2 and their descriptors, requests that went
    // through the bounce buffer by why (SwitchAdmaRefusal; kAdmaOk: ADMA2 off), ADMA2 writes read back and found
    // equal, ADMA errors, worker time on the ADMA2 path's cache maintenance (part of busyNs), and busy time per direction.
    uint64_t direct, descriptors, bounced[kAdmaRefusals], verified, admaErrors, syncNs, readNs, writeNs;
    // r12-sdio: commands issued before the previous chain's completions ran, and of those the ones already finished when
    // the worker came back to them (no interrupt taken). Read chains that went by ADMA2 with some segments staged through
    // the bounce buffer, and those bytes.
    uint64_t overlapped, early, stagedChains, stagedBytes;
    // r14-sdcq: data commands framed with Auto CMD23; read chains with gaps, the gaps' bytes read through and discarded,
    // and of those chains the ones compared with SDMA and found equal.
    uint64_t cmd23, gapChains, gapBytes, gapProven;
    // r14-sdcq: command-queue tasks executed, and the worker time of their groups (submission, every task, proofs).
    uint64_t cqTasks, cqNs;
    // r16-sdcq: writes flushed because the card's cache would not go off (commitWrite), those whose flush failed, and
    // their worker time.
    uint64_t flushes, flushErrors, flushNs;
    // Erase-based trim: erase commands, the sectors they freed and the time the card was busy with them.
    uint64_t trimErases, trimSectors, trimNs;
#if DEVELOPMENT || DEBUG
    // SwitchSdBus.h: commands refused because the bus was not free at their one PRESENT_STATE read, and DAT0 releases
    // awaited by CMD13 with busy (the card still programming after a transfer complete). ADMA2 chains that reached a
    // non-cacheable page (dmaPage).
    uint64_t busRefused, releases, uncachedChains;
#endif
};
}

#if DEVELOPMENT || DEBUG
#define SWITCH_SD_DEBUG_COUNT(field) (++stats.field)
#else
#define SWITCH_SD_DEBUG_COUNT(field) ((void)0)
#endif

class NativeSwitchBlock;

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs external linkage (kalloc type view)
class NativeSwitchBlockDevice : public IOBlockStorageDevice {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(NativeSwitchBlockDevice) NativeSwitchBlock *owner = nullptr;
    uint64_t blocks = 0;

public:
    bool configure(NativeSwitchBlock *, uint64_t sectors);

    IOReturn doEjectMedia() override
    {
        return kIOReturnUnsupported;
    }

    IOReturn doFormatMedia(UInt64) override
    {
        return kIOReturnUnsupported;
    }

    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override
    {
        return 0;
    }

    IOReturn doUnmap(IOBlockStorageDeviceExtent *extents, UInt32 count, IOStorageUnmapOptions) override;

    char *getVendorString() override
    {
        static char vendor[] = "Switch";
        return vendor;
    }

    char *getProductString() override
    {
        static char product[] = "SD APFS";
        return product;
    }

    char *getRevisionString() override
    {
        static char revision[] = "2.0";
        return revision;
    }

    char *getAdditionalDeviceInfoString() override
    {
        static char info[] = "SDMMC1 APFS partition";
        return info;
    }

    IOReturn reportBlockSize(UInt64 *value) override
    {
        *value = kSector;
        return kIOReturnSuccess;
    }

    IOReturn reportMaxValidBlock(UInt64 *value) override
    {
        *value = blocks - 1;
        return kIOReturnSuccess;
    }

    IOReturn reportEjectability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn reportRemovability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn reportWriteProtection(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn reportMediaState(bool *present, bool *changed) override
    {
        *present = true;
        if (changed)
            *changed = false;
        return kIOReturnSuccess;
    }

    // Writes complete only after the card releases busy and with no card cache enabled, so every completed write is
    // already durable. r16-sdcq: the card's cache is on only inside a CQ session, which carries reads only; a cache that
    // would not go off has every write flushed before it completes (commitWrite). Write-through either way.
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override
    {
        return kIOReturnSuccess;
    }

    IOReturn doSynchronizeCache() override
    {
        return kIOReturnSuccess;
    }

    IOReturn getWriteCacheState(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn setWriteCacheState(bool) override
    {
        return kIOReturnUnsupported;
    }

    IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
                              IOStorageCompletion *) override;
};

class NativeSwitchBlock : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(NativeSwitchBlock) volatile uint8_t *regs = nullptr;
    volatile uint8_t *car = nullptr, *apb = nullptr, *pmc = nullptr;  // SDR104 bring-up only
    volatile uint8_t *mc = nullptr;                                   // memory controller (SMMU state for SDMMC1)
    switch_dma_buffer bounceMemory = {};
    uint8_t *bounce = nullptr;
    uint64_t bouncePA = 0;
    IOLock *queueLock = nullptr;
    Request *head = nullptr, *tail = nullptr;
    Unmap *unmaps = nullptr;  // under queueLock, oldest first
    // Erase-based trim (cards without discard): the map and the quiet-period timer, under queueLock.
    SwitchSdTrimMap trim;
    uint64_t *trimBits = nullptr;
    size_t trimBytes = 0;
    thread_call_t trimCall = nullptr;
    uint64_t lastIoNs = 0, trimBootSectors = 0, trimSinceSave = 0;
    bool trimCursorLoaded = false, trimFailed = false;
    unsigned chainsSinceUnmap = 0;         // under queueLock: chains picked while a discard waited
    uint32_t queued = 0;                   // requests waiting, under queueLock
    uint32_t queuedReads[kClasses] = {};   // r6-lows: reads waiting per class, under queueLock
    ClassWait waits[kClasses] = {};        // r6-lows: per class since the last throughput line, under queueLock
    uint64_t passed = 0, passedTotal = 0;  // r6-lows: reads taken before older requests (this line, since boot)
    uint64_t submitted = 0, serviced = 0;  // for the root-wait report
    uint64_t lastEnd = 0;                  // card LBA after the previous request (sequential count)
    static constexpr uint64_t statsEveryNs = 10 * 1000000000ULL;
    uint64_t statsLastNs = 0;
    static constexpr bool merge = true;
    // r12-sdio: reads merge with continuing reads anywhere in the queue (SwitchQueue.h) until a chain fails its check.
    bool mergeAnywhere = true;
    // Requests' bytes through IOMemoryDescriptor::readBytes/writeBytes, with the clean+invalidate of every destination
    // line they add on this kernel (SwitchClientCopy.h), for A/B builds; it keeps ADMA2 off too, so every request takes
    // the bounce buffer as before r10-adma2.
    static constexpr bool copyFlush = false;
    // r10-adma2: the ADMA2 descriptor table (one, rebuilt for every ADMA2 command; 16 KiB contiguous below 4 GiB), whether
    // ADMA2 carries data (setup()'s check passed and no ADMA error since) and writes (no write read back different), the
    // state of the last ADMA error (command()), SDMA's own read limit for the bounce path (kBounceSectors once setup()
    // has checked the boundary resume), and the read-back of the first ADMA2 writes (writeChecks left; into checkBuffer).
    switch_dma_buffer tableMemory = {};
    SwitchAdmaTable table = {};
    uint64_t tablePA = 0;
    bool tableUncached = false;  // the table reaches a non-cacheable page (dmaPage), which syncLines leaves alone
    bool adma = false, admaWrites = false;
    uint8_t admaErrorState = 0;
    uint64_t admaErrorAddress = 0;
    uint32_t sdmaReadSectors = kSafeSectors;
    uint32_t writeChecks = 0;
    // r12-sdio: the longest write chain one ADMA2 command carries (kDirectWriteSectors, or kSafeSectors once a longer one
    // read back different), and the read-backs of writes over kSafeSectors left this boot.
    uint32_t writeSectors = kSafeSectors, largeWriteChecks = 0;
    // r12-sdio: whether ADMA2 reaches pages above 4 GiB (SwitchAdma.h), and the SDMA copy the proofs (proveHigh,
    // proveWrite) compare against: kBounceBytes, freed once no proof is left (releaseChecks).
    SwitchAdmaHigh high = {};
    uint8_t *checkBuffer = nullptr;
    // r12-sdio: a read chain's segments that may not go straight to the card go through the bounce buffer alone
    // (SwitchAdmaStaging), once setup()'s check has run it on the hardware (stagingOn).
    Staging staging = {};
    Staging::Stage *stages = nullptr;
    bool stagingOn = false;
    // r14-sdcq: Auto CMD23 framing per direction (checkCmd23 passed; writes off again if a CMD23 write read back
    // different) and the CMD23 writes still to read back this boot.
    bool cmd23Reads = false, cmd23Writes = false;
    uint32_t cmd23WriteChecks = 0;
    // r14-sdcq: read chains with gaps (SwitchQueue.h) while gapsOn: the sink their gaps are read into (kGapSectors,
    // contiguous below 4 GiB, never read or written by the CPU after setup) and the proofs still to do this boot.
    switch_dma_buffer sinkMemory = {};
    uint64_t sinkPA = 0;
    bool gapsOn = false;
    uint32_t gapProofs = 0;

    bool inSink(uint64_t pa) const
    {
        return (sinkPA != 0u) && pa >= sinkPA && pa - sinkPA < static_cast<uint64_t>(kGapSectors) * kSector;
    }

    // r14-sdcq: the command queue (SwitchCq.h): the engine and its bus (this controller), the CQ control byte's place
    // (identify()), the DMA buffer CMD48/CMD49 move their block through, the two task arrays the worker alternates (a
    // group's last task may still wait for its completions while the next group fills the other one), and the tasks
    // still to compare with legacy reads this boot.
    // r18-sdcq: CMD13 for the recovery, and the last command any path sent the card (its index and the interrupt status
    // it ended with) for the error line.
    struct CqBus {
        NativeSwitchBlock *self;
        int command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1) const;
        int control(uint32_t byte, bool write, uint8_t *value, uint32_t *r1) const;
        int status(bool tasks, uint32_t *r1) const;

        static void settle()
        {
            IOSleep(1);
        }

        uint8_t lastCommand() const
        {
            return self->cardLast;
        }

        uint32_t lastIrq() const
        {
            return self->cardLastIrq;
        }
    };

    using CqEngine = SwitchCqEngine<CqBus>;
    CqBus cqBus{this};
    CqEngine cq;
    uint32_t cqControlAddress = 0;
    switch_dma_buffer regMemory = {};
    uint8_t *regBuffer = nullptr;
    uint64_t regPA = 0;
    Batch cqGroups[2][kCqMaxTasks] = {};
    unsigned cqTurn = 0;
    uint32_t cqProofs = 0;
    char cqWhy[320] = {};
    // r18-sdcq: the last command the card got (any path: noteCommand) and the interrupt status it ended with; the
    // recovered errors' lines (cqRecovered), and whether the stop's IOLog line went out.
    uint8_t cardLast = 0;
    uint32_t cardLastIrq = 0;
    bool cqStopLogged = false;
    char cqStopWhy[160] = {};

    void noteCommand(uint8_t index)
    {
        cardLast = index;
    }

    // r16-sdcq: from legacy mode a group needs this many chains: entering and leaving CQ mode cost what checkCq measured,
    // so a group must save at least that much at the per-task saving its test group showed (setup()).
    unsigned cqMinChains = 2;
    // r16-sdcq: commitWrite's one-shot lines: writes flushed because the cache would not go off; a flush that failed.
    bool flushingWrites = false, flushFailed = false;
    char admaWhy[128] = {};
    // SDMMC1 interrupt: the controller's line is (STATUS & SIGNAL_ENABLE). The worker enables the signals it
    // waits for and sleeps on irqEvent; the filter disables them again (lowering the line) and wakes it.
    IOWorkLoop *irqLoop = nullptr;
    IOFilterInterruptEventSource *irqSource = nullptr;
    int irqEvent = 0;
    SwitchSdBus bus;
    // Bus state: what the card runs at, its RCA after our re-initialization, the last STATUS a bring-up
    // command saw, and whether SDR104 tuning is in effect (a failed transfer is re-tuned before its retry)
    // at which kSpeeds step.
    const char *busMode = "inherited";
    uint32_t busKHz = 0;
    uint32_t rca = 0, initStatus = 0;
    bool tuned = false;
    unsigned speed = 0;
    // System sleep: the worker stops between commands while suspended; the card was in 1.8 V signalling at boot (so it
    // is taken back there after SC7); LBA 1 as read at boot, compared after re-initialization.
    bool suspended = false, working = false, lowVoltageBus = false;
    uint8_t gptHeader[kSector] = {};
    // r14-sdcq: the card's registers as identify() read them once at boot (CID and CSD during reinitialize(), the rest in
    // transfer state), for the identification lines and for what CMD23 and the command queue may use.
    uint8_t cid[16] = {}, csd[16] = {}, scrBytes[8] = {}, ssrBytes[64] = {};
    bool haveCid = false, haveCsd = false, haveScr = false, haveSsr = false, havePerf = false, havePower = false;
    SwitchSdScr scr = {};
    SwitchSdStatus sdStatus = {};
    SwitchSdGeneralInfo extInfo = {};
    SwitchSdPerf perf = {};
    SwitchSdPower power = {};
    const char *extWhy = nullptr;

    uint8_t r8(uint32_t o)
    {
        return switch_mmio_read8(regs, o);
    }

    uint16_t r16(uint32_t o)
    {
        return switch_mmio_read16(regs, o);
    }

    uint32_t r32(uint32_t o)
    {
        return switch_mmio_read32(regs, o);
    }

    void w8(uint32_t o, uint8_t v)
    {
        switch_mmio_write8(regs, o, v);
    }

    void w16(uint32_t o, uint16_t v)
    {
        switch_mmio_write16(regs, o, v);
    }

    void w32(uint32_t o, uint32_t v)
    {
        switch_mmio_write32(regs, o, v);
    }

    static bool stage(IOMemoryDescriptor *memory, uint32_t from, uint8_t *at, uint32_t length, bool toClient)
    {
        if (copyFlush)
            return (toClient ? memory->writeBytes(from, at, length) : memory->readBytes(from, at, length)) == length;
        return switchClientCopy(memory, at, length, toClient, copypv, from);
    }

    bool awaitTuningIdle(uint64_t timeoutNs);
    bool claimBus(SwitchSdUse use);
    void ended(uint32_t status);
    int awaitRelease();
    void startInterrupt();
    static bool interruptFilter(OSObject *, IOFilterInterruptEventSource *);
    bool waitSignal(uint32_t signals, uint64_t timeoutNs);
    uint32_t awaitStatus(uint32_t awaited, uint64_t limitNs);
    [[noreturn]] void interruptLost(uint32_t awaited);
    void resetLines();
    void recover(bool data);
    int command(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool cmd23 = false);
    // r12-sdio: command()'s halves, so the worker can do other work while the card moves the data. r14-sdcq: `cmd23`
    // frames a multi-block command with Auto CMD23 instead of Auto CMD12 (switchSdBlockFrame).
    // r14-sdcq: `task` (a command-queue task's CMD46 argument) issues CMD46 instead, with no auto command (complete()'s
    // `task`).
    int issue(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, bool cmd23 = false,
              uint32_t task = kNoTask);
    int complete(uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool *early, bool cmd23 = false,
                 bool task = false);
    bool failed(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write, unsigned descriptors,
                bool retry);
    int run(uint64_t lba, uint32_t sectors, bool write, unsigned descriptors);
    int retry(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write, unsigned descriptors);
    static void worker(void *, wait_result_t);
    Request *pickLocked(uint32_t *sectors, uint32_t *count);
    void takenLocked(Request *first, uint32_t count, uint64_t pickNs);
    void beginBatch(Batch &);
    void endBatch(Batch &);
    void account(Batch &);
    void completeBatch(Batch &);
    // r14-sdcq: the command queue: a group's chains (under queueLock), the group run, its tasks, the protocol's own
    // commands, leaving CQ mode, stopping it for the boot, and the power cycle for a card that will not leave it.
    unsigned groupLocked(Request *first, uint32_t sectors, uint32_t count, bool *queue);
    Batch *runGroup(Batch *group, unsigned n, Batch *pending, bool queue);
    void beginTask(Batch &, uint32_t argument);
    int endTask(Batch &, uint32_t *r1);
    int cqCommand(uint32_t index, uint32_t argument, uint32_t flags, uint32_t *response, int data,
                  uint64_t limitNs = 1500000000ULL);
    void cqLeave();
    bool stepUnmap(Unmap &job);
    int erase(uint64_t first, uint64_t last, uint32_t argument, uint64_t limitNs);
    bool trimRunnable() const;
    uint64_t trimQuietUntil() const;
    void trimStep();
    static void saveTrimCursor(uint64_t cursor);
    static void trimFired(thread_call_param_t owner, thread_call_param_t);
    // r18-sdcq: one line per recovered error (rate-limited), the power cycle of a card that stays in CQ mode, and the
    // stop's one IOLog line.
    void cqRecovered(const SwitchCqError &e) const;
    void cqPowerCycle();
    void cqStopped();
    void resetCard();
    // r16-sdcq: a write's last step (a flush, only while the card's cache would not go off); what a failed enable left
    // in the performance register, for checkCq's line.
    bool commitWrite();
    void describeCq(char *out, size_t size);
    const char *checkCq(uint64_t *queuedNs, uint64_t *serialNs, uint64_t *enterNs, uint64_t *leaveNs, unsigned *tasks);
    // r10-adma2: the ADMA2 path, the bounce path in SDMA-sized pieces, the write read-back, the setup check, the
    // power-state hooks.
    SwitchAdmaRefusal buildTable(Request *first, uint32_t sectors, bool write, bool task = false);
    void prepareDirect(bool write, ReadFill fill);
    void finishDirect(bool write);
    bool stageChain(Request *first, uint32_t from, uint32_t length, uint8_t *buffer, bool toClient);
    bool bounceChain(Request *first, uint32_t sectors);
#if DEVELOPMENT || DEBUG
    bool proveWrite(Request *first, uint32_t sectors, bool cmd23);
    bool proveHigh(Request *first, uint32_t sectors);
    bool proveGaps(Request *first, uint32_t sectors);

    // r14-sdcq: a read chain compared with the same blocks read by SDMA (proveGaps, the command queue's proofs).
    enum ChainProof : uint8_t {
        kChainMatches,
        kChainDiffers,
        kChainUnproven,
        kChainBroken
    };

    ChainProof compareChain(Request *first, uint32_t sectors, uint64_t *at);
#endif
    void releaseChecks();
    bool copyStaged();
    const char *checkAdma(const uint8_t *reference, uint32_t *descriptors, uint32_t *highPages, uint32_t *highOk,
                          uint32_t *staged);
    const char *checkCmd23(uint64_t *withNs, uint64_t *withoutNs);
    char cmd23Why[96] = {};
    char stagingReason[96] = {};
    void dmaSuspend();
    void dmaResume();
    void logStats();
    void logThroughput();
    // SDR104 bring-up (setup() and re-tuning only).
    int cardCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data = 0);
    int appCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data = 0);
    void readR2(uint8_t out[16]);
    // r14-sdcq: the card's identity and capabilities, read and logged once at boot.
    bool extRead(uint8_t fno, uint32_t address, uint32_t length, uint8_t *out);
    void identify();
    int switchFunction(bool set, unsigned group, uint32_t value);
    void setTiming(int uhs, bool highSpeed, int source, uint32_t carDivider, uint32_t divider);
    bool calibratePads();
    bool switchTo1V8(uint8_t ldo2);
    bool enablePllc4();
    void tuneStep();
    bool tune();
    bool clockAndTune(unsigned from);
    void retune(bool slower);
    bool raiseBusSpeed();
    bool reinitialize(uint32_t baseKHz, uint8_t ldo2, bool uhs, bool resume);
    void powerOffCard();
    void powerOnController();

public:
    void suspend();
    void resume();
    Stats stats = {};
    uint64_t firstSector = 0, sectorCount = 0;
    // Per request and per merged command, as advertised: reads kBounceSectors with ADMA2 or once setup() has checked
    // SDMA boundary resumes, else kSafeSectors; writes kDirectWriteSectors with ADMA2, else kSafeSectors.
    uint32_t maxSectors = kSafeSectors, maxWriteSectors = kSafeSectors;
    NativeSwitchBlockDevice *device = nullptr;

    bool setup();
    int transfer(uint64_t lba, uint32_t sectors, bool write, uint8_t *bytes);
    void submit(Request *);
    IOReturn unmap(const IOBlockStorageDeviceExtent *extents, uint32_t count, uint64_t blocks);

    void prepareTrim();

    void trimStats(uint64_t *erases, uint64_t *sectors, uint64_t *busyNs) const
    {
        *erases = __atomic_load_n(&stats.trimErases, __ATOMIC_RELAXED);
        *sectors = __atomic_load_n(&stats.trimSectors, __ATOMIC_RELAXED);
        *busyNs = __atomic_load_n(&stats.trimNs, __ATOMIC_RELAXED);
    }

    // SD 5.1 Discard (SD Status DISCARD_SUPPORT) frees blocks without defining what they then read, which is what an
    // unmap asks. A card without it erases the unmapped AUs later, in idle time (SwitchSdTrim.h).
    bool canUnmap() const
    {
        return haveSsr && (sdStatus.discard || (trimBits != nullptr));
    }

    void reportState(const char *why);
};

OSDefineMetaClassAndStructors(NativeSwitchBlockDevice, IOBlockStorageDevice)
    OSDefineMetaClassAndStructors(NativeSwitchBlock, IOService)

        namespace
{
    NativeSwitchBlock *gOwner = nullptr;
    int gPublishStatus = -1;
    // r8-sdboot: the boot prefetch thread (SwitchCryptex.cpp). Its reads, issued from that thread down to here
    // (cluster_io -> spec_strategy -> IOMediaBSDClient -> IOBlockStorageDriver -> doAsyncReadWrite), take the high class,
    // first come first served with the foreground's reads: in the background class they waited out the 100 ms aging
    // behind a saturated foreground (11-04 r7h: 965 MiB took 7.5 s to 68.3 s uptime, ~2.5 MB/s after the first 10 s),
    // while the foreground page-faulted the same blocks one 4 KiB read at a time.
    thread_t gPrefetchThread = THREAD_NULL;
}

extern "C" void switch_block_prefetch_thread(thread_t thread)
{
    __atomic_store_n(&gPrefetchThread, thread, __ATOMIC_RELEASE);
}

extern "C" bool switch_sd_trim_stats(uint64_t *erases, uint64_t *sectors, uint64_t *busy_ns)
{
    if (!gOwner)
        return false;
    gOwner->trimStats(erases, sectors, busy_ns);
    return true;
}

namespace {
void putString(OSDictionary *dictionary, const char *key, const char *value)
{
    if (const OSString *string = OSString::withCStringNoCopy(value)) {
        dictionary->setObject(key, string);
        string->release();
    }
}
}

bool NativeSwitchBlockDevice::configure(NativeSwitchBlock *provider, uint64_t sectors)
{
    owner = provider;
    blocks = sectors;
    setProperty(kIOMaximumBlockCountReadKey, provider->maxSectors, 64);
    setProperty(kIOMaximumBlockCountWriteKey, provider->maxWriteSectors, 64);
    setProperty(kIOMaximumByteCountReadKey, static_cast<unsigned long long>(provider->maxSectors) * kSector, 64);
    setProperty(kIOMaximumByteCountWriteKey, static_cast<unsigned long long>(provider->maxWriteSectors) * kSector, 64);
    if (OSDictionary *protocol = OSDictionary::withCapacity(2)) {
        putString(protocol, kIOPropertyPhysicalInterconnectTypeKey, "SD");
        putString(protocol, kIOPropertyPhysicalInterconnectLocationKey, kIOPropertyInternalKey);
        setProperty(kIOPropertyProtocolCharacteristicsKey, protocol);
        protocol->release();
    }
    if (OSDictionary *device = OSDictionary::withCapacity(1)) {
        putString(device, kIOPropertyMediumTypeKey, kIOPropertyMediumTypeSolidStateKey);
        setProperty(kIOPropertyDeviceCharacteristicsKey, device);
        device->release();
    }
    // Force Unit Access: every write completes only once it is on flash (doSynchronize). Unmap: SD discard.
    if (OSDictionary *features = OSDictionary::withCapacity(2)) {
        features->setObject(kIOStorageFeatureForceUnitAccess, kOSBooleanTrue);
        if (provider->canUnmap())
            features->setObject(kIOStorageFeatureUnmap, kOSBooleanTrue);
        setProperty(kIOStorageFeaturesKey, features);
        features->release();
    }
    setName("SwitchSD");
    return true;
}

IOReturn NativeSwitchBlockDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                                   IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !count || block >= blocks || count > blocks - block || count > owner->maxSectors ||
        buffer->getLength() < count * kSector)
        return kIOReturnBadArgument;
    const IODirection direction = buffer->getDirection();
    if (direction != kIODirectionIn && direction != kIODirectionOut)
        return kIOReturnBadArgument;
    if (direction == kIODirectionOut && count > owner->maxWriteSectors)
        return kIOReturnBadArgument;
    auto *request = static_cast<Request *>(IOMalloc(sizeof(Request)));
    if (!request)
        return kIOReturnNoMemory;
    buffer->retain();
    // No attributes: the default class, as IOStorage's own default priority. The boot prefetch: high.
    IOStoragePriority priority = kIOStoragePriorityDefault;
    if (current_thread() == __atomic_load_n(&gPrefetchThread, __ATOMIC_ACQUIRE)) {
        priority = kIOStoragePriorityHigh;
    } else if (attributes) {
        priority = attributes->priority;
    }
    *request = {nullptr,
                buffer,
                owner->firstSector + block,
                static_cast<uint32_t>(count),
                direction == kIODirectionOut,
                *completion,
                static_cast<uint8_t>(priority >> 6),
                0,
                false};
    owner->submit(request);
    return kIOReturnSuccess;
}

// SDR104 tuning only (setup and re-tunes, never per I/O): while Execute Tuning is set the controller raises no transfer
// complete (SD Host Controller Simplified Specification 4.20, 2.2.17 (d)), so nothing but PRESENT_STATE tells when the
// previous tuning block's read has ended.
bool NativeSwitchBlock::awaitTuningIdle(uint64_t timeoutNs)
{
    const uint64_t start = switch_now_ns();
    for (;;) {
        if (!(r32(PRESENT) & PRESENT_BUSY))
            return true;
        const uint64_t elapsed = switch_now_ns() - start;
        if (elapsed >= timeoutNs)
            return false;
        if (elapsed > 1000000)
            IOSleep(1);
    }
}

// One read of PRESENT_STATE before a command, never a wait: the previous command's interrupts were the wait for the bus
// (SwitchSdBus.h). A data command that finds the card still programming has its release awaited by interrupt first.
// Refused, the command is not issued and fails with its caller's step; the caller's recovery resets the lines.
bool NativeSwitchBlock::claimBus(SwitchSdUse use)
{
    SwitchSdBusState state = bus.claim(r32(PRESENT), use);
    if (state == SwitchSdBusState::kCardBusy && awaitRelease() == 0)
        state = bus.claim(r32(PRESENT), use);
    if (state == SwitchSdBusState::kFree)
        return true;
    SWITCH_SD_DEBUG_COUNT(busRefused);
    return false;
}

// The interrupt status a command ended with. PRESENT_STATE is read only when the card may still hold DAT0.
void NativeSwitchBlock::ended(uint32_t status)
{
    bus.ended(status, bus.card ? r32(PRESENT) : 0);
}

// The card still holds DAT0 (programming) after a command's transfer complete, or before a write after a reset: CMD13,
// which the card takes in every state, sent with the busy response type, so the controller raises transfer complete
// when DAT0 is released (2.2.9, DAT Line Active (c)). Per the spec a write's or an R1b's own transfer complete already
// waits for the release, so this runs only if the controller ends one early. 0, or 1 with the lines reset.
int NativeSwitchBlock::awaitRelease()
{
    SWITCH_SD_DEBUG_COUNT(releases);
    if (bus.claim(r32(PRESENT), SwitchSdUse::kBusy) != SwitchSdBusState::kFree) {
        SWITCH_SD_DEBUG_COUNT(busRefused);
        return 1;
    }
    w32(STATUS, 0xffffffff);
    w16(TRANSFER, 0);
    w32(ARGUMENT, rca << 16);
    w16(COMMAND, 13u << 8 | RSP_R1B);
    noteCommand(13);
    const uint32_t s = awaitStatus(INT_TRANSFER, kReleaseLimitNs);
    ended(s);
    w32(STATUS, INT_COMMAND | INT_TRANSFER);
    if (!(s & INT_ERRORS) && (s & INT_TRANSFER) && bus.idle())
        return 0;
    resetLines();
    w32(STATUS, 0xffffffff);
    return 1;
}

// Primary interrupt context. Masking the signals lowers the level line, so there is
// nothing left for a work loop: STATUS stays for the worker to read and clear.
bool NativeSwitchBlock::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<NativeSwitchBlock *>(owner);
    if (!(self->r32(STATUS) & self->r32(SIGNAL_ENABLE)))
        return false;
    self->w32(SIGNAL_ENABLE, 0);
    (void)self->r32(SIGNAL_ENABLE);  // the line is low before ArmCpu ends the interrupt
    __atomic_fetch_add(&self->stats.interrupts, 1, __ATOMIC_RELAXED);
    thread_wakeup(&self->irqEvent);
    return false;
}

// Sleeps until the controller raises one of `signals` or timeoutNs passes; false on
// timeout. The wait is asserted before the signals are enabled, so an interrupt that
// fires at once (a bit already latched) still wakes this thread.
bool NativeSwitchBlock::waitSignal(uint32_t signals, uint64_t timeoutNs)
{
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(timeoutNs, &interval);
    assert_wait_deadline(&irqEvent, THREAD_UNINT, mach_absolute_time() + interval);
    w32(SIGNAL_ENABLE, signals);
    const wait_result_t result = thread_block(THREAD_CONTINUE_NULL);
    w32(SIGNAL_ENABLE, 0);
    return result == THREAD_AWAKENED;
}

// A wait that ran out with an awaited bit latched in STATUS: the controller raised it, the CPU never took the interrupt.
void NativeSwitchBlock::interruptLost(uint32_t awaited)
{
    char gic[192];
    switch_interrupt_describe(kInterrupt, gic, sizeof(gic));
    panic("SwitchSD: SDMMC1 interrupt lost: STATUS %#x awaited %#x, %s", r32(STATUS), awaited, gic);
}

// Sleeps until STATUS shows one of `awaited` or an error, at most limitNs; returns the STATUS it read last. STATUS is
// read before each sleep, so a bit that latched meanwhile takes no interrupt.
uint32_t NativeSwitchBlock::awaitStatus(uint32_t awaited, uint64_t limitNs)
{
    const uint32_t signals = awaited | ERROR_SIGNALS;
    const uint64_t start = switch_now_ns();
    for (;;) {
        const uint32_t s = r32(STATUS);
        if (s & (awaited | INT_ERRORS))
            return s;
        const uint64_t elapsed = switch_now_ns() - start;
        if (elapsed >= limitNs)
            return s;
        const uint64_t left = limitNs - elapsed;
        if (!waitSignal(signals, left < kInterruptCheckNs ? left : kInterruptCheckNs) && (r32(STATUS) & signals))
            interruptLost(signals);
    }
}

void NativeSwitchBlock::startInterrupt()
{
    IOService *nub = switch_interrupt_nub(kInterrupt, false);
    irqLoop = IOWorkLoop::workLoop();
    // Filter only: it never schedules the (absent) work-loop action.
    irqSource = irqLoop ? IOFilterInterruptEventSource::filterInterruptEventSource(
                              this, nullptr, &NativeSwitchBlock::interruptFilter, nub, 0)
                        : nullptr;
    if (!irqSource || irqLoop->addEventSource(irqSource) != kIOReturnSuccess) {
        panic("SwitchSD: cannot attach the SDMMC1 interrupt (INTID %u)", kInterrupt);
    }
    irqSource->enable();
}

// Software-reset the CMD and DAT state machines (hekate _sdmmc_reset_cmd_data); STATUS is left alone.
void NativeSwitchBlock::resetLines()
{
    w8(RESET, 0x06);
    for (const uint64_t start = switch_now_ns(); (r8(RESET) & 0x06) && switch_now_ns() - start < 100000000;) {
    }
    bus.reset();
}

// Reset the CMD and DAT state machines; for a failed data command,
// also stop the card (CMD12, abort type, R1b) so it returns to transfer state.
void NativeSwitchBlock::recover(bool data)
{
    resetLines();
    w32(STATUS, 0xffffffff);
    if (!data || !claimBus(SwitchSdUse::kAbort))
        return;
    w16(TRANSFER, 0);
    w32(ARGUMENT, 0);
    switch_dcache_sync();
    w16(COMMAND, 0x0cdb);
    noteCommand(12);
    // CMD12 is an R1b: transfer complete marks the end of the card's busy.
    const uint32_t s = awaitStatus(INT_TRANSFER, 2000000000);
    ended(s);
    w32(STATUS, 0xffffffff);
    if (!(s & INT_TRANSFER) || (s & INT_ERRORS) || (!bus.idle() && awaitRelease())) {
        resetLines();
        w32(STATUS, 0xffffffff);
    }
}

// One data command: by ADMA2 through the descriptor table when `descriptors` is nonzero (built and synced by the
// caller), else by SDMA into/out of the bounce buffer. Returns 0 or a nonzero step code; *status receives the last
// interrupt status sample. r12-sdio: issue() then complete(); the worker's ADMA2 commands call the two apart.
int NativeSwitchBlock::command(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, uint32_t *status,
                               bool cmd23)
{
    *status = 0;
    if (const int step = issue(lba, sectors, write, descriptors, cmd23))
        return step;
    return complete(sectors, write, descriptors, status, nullptr, cmd23);
}

// The command's first half: the partition check, the registers and the command itself. 0, or step 1 when the bus was not
// free (claimBus; then nothing was issued).
int NativeSwitchBlock::issue(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, bool cmd23,
                             uint32_t task)
{
    // Every card write passes here, whichever path staged it: none may reach outside the APFS partition. (Until setup()
    // has found the partition, sectorCount is 0 and so no write can be issued at all.)
    if (write && (lba < firstSector || sectors > sectorCount || lba - firstSector > sectorCount - sectors)) {
        panic("SwitchSD: write lba=%u n=%u outside the APFS partition [%llu, +%llu)", lba, sectors, firstSector,
              sectorCount);
    }
    // r14-sdcq: the command queue carries reads only (a CQ write would need CMD47 and its own proofs).
    if (write && task != kNoTask)
        panic("SwitchSD: write lba=%u n=%u as a command-queue task", lba, sectors);
    if (!claimBus(write ? SwitchSdUse::kWrite : SwitchSdUse::kRead))
        return 1;
    w32(STATUS, 0xffffffff);
    // DMA select per command: the bring-up commands' SDMA (cardCommand) and the data path share the controller.
    const uint64_t address = descriptors ? tablePA : bouncePA;
    w8(HOST_CONTROL, static_cast<uint8_t>((r8(HOST_CONTROL) & ~DMA_SELECT) | (descriptors ? DMA_ADMA2 : 0)));
    w32(DMA_LOW, static_cast<uint32_t>(address));
    w32(DMA_HIGH, static_cast<uint32_t>(address >> 32));
    w16(BLOCK_SIZE, kSector | 0x7000);  // 512 KiB SDMA boundary (ADMA2 ignores it)
    w16(BLOCK_COUNT, static_cast<uint16_t>(sectors));
    // r14-sdcq: a task's CMD46 names the task (its block address went to the card in CMD45).
    w32(ARGUMENT, task != kNoTask ? task : lba);
    // DMA | block-count | Auto CMD12 (or r14-sdcq: Auto CMD23, its block count in Argument 2; a task: none) | read |
    // multi, as the qualified sequence.
    const SwitchSdFrame frame =
        task != kNoTask ? switchSdTaskFrame(sectors) : switchSdBlockFrame(sectors, write, cmd23);
    if (frame.argument2)
        w32(ARGUMENT2, frame.argument2);
    w16(TRANSFER, frame.transfer);
    switch_dcache_sync();
    w16(COMMAND, frame.command);
    noteCommand(frame.autoStop ? 12 : static_cast<uint8_t>(frame.command >> 8));  // r18-sdcq: an Auto CMD12 ends it
    return 0;
}

// The command's second half, for the command issue() started: 0 or a nonzero step code. Its time limit runs from here,
// not from issue(): the worker may have completed the previous chain's requests in between. `*early` (when asked): the
// command had already finished when this began, so it took no interrupt.
int NativeSwitchBlock::complete(uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool *early,
                                bool cmd23, bool task)
{
    *status = 0;
    const bool multi = sectors > 1;
    // One sleep per command: the worker waits for transfer complete, an error, or an SDMA boundary
    // (transfers over 512 KiB). Command complete latches in STATUS without raising the line and its R1 is
    // checked once the data has moved; a command the card refuses moves no data and so ends as a
    // data error or the time limit below. (Waiting for command complete first cost a second
    // interrupt and context switch per request, a real share of a ~4 KiB read.) ADMA2 has no boundary to resume: its
    // DMA interrupt only follows a descriptor with the Int attribute, which none of ours has.
    const uint32_t awaited = INT_TRANSFER | (descriptors ? 0 : INT_DMA) | ERROR_SIGNALS;
    const uint64_t start = switch_now_ns();
    const uint64_t limit = write ? 5000000000ULL : 2000000000ULL;
    uint32_t s = 0;
    uint32_t resume = 0;
    bool waited = false;
    for (;;) {
        s = r32(STATUS);
        *status = s;
        if (descriptors && (s & INT_ADMA_ERROR)) {
            // The engine's state and the descriptor it stopped at (ADMA Error Status; ADMA System Address), for failed().
            admaErrorState = r8(ADMA_ERROR);
            admaErrorAddress = r32(DMA_LOW) | static_cast<uint64_t>(r32(DMA_HIGH)) << 32;
            return kStepAdmaError;
        }
        if (s & INT_ERRORS)
            return (s & INT_COMMAND) ? 3 : 2;
        if (s & INT_TRANSFER)
            break;
        if (!descriptors && (s & INT_DMA)) {
            // Paused at the next 512 KiB boundary of the (boundary-aligned) bounce buffer: give it the address there.
            resume += kSdmaBoundary;
            if (resume > sectors * kSector)
                return 5;
            w32(STATUS, INT_DMA);
            w32(DMA_LOW, static_cast<uint32_t>(bouncePA + resume));
            w32(DMA_HIGH, static_cast<uint32_t>((bouncePA + resume) >> 32));
            continue;
        }
        const uint64_t elapsed = switch_now_ns() - start;
        if (elapsed >= limit)
            return (s & INT_COMMAND) ? 6 : 7;
        waited = true;
        // Sleep until the transfer completes or fails. A wait that times out with an awaited bit latched means the
        // interrupt does not reach the CPU.
        const uint64_t left = limit - elapsed;
        if (!waitSignal(awaited, left < kInterruptCheckNs ? left : kInterruptCheckNs) && (r32(STATUS) & awaited))
            interruptLost(awaited);
    }
    if (early)
        *early = !waited;
    ended(s);
    w32(STATUS, INT_COMMAND | INT_TRANSFER);
    if (!(s & INT_COMMAND) || (r32(RESPONSE) & R1_ERRORS))
        return 4;
    if (multi && !task) {
        // Auto CMD12's or (r14-sdcq) Auto CMD23's errors; only a CMD12 leaves a response in STOP_RESPONSE. A CQ task
        // has no auto command.
        if (r16(AUTO_CMD_ERROR) & 0x9f)
            return 8;
        if (!cmd23 && (r32(STOP_RESPONSE) & R1_STOP_ERRORS))
            return 9;
    }
    // A write's transfer complete follows the card's busy release, which DAT0 confirms (ended()).
    if (!bus.idle() && awaitRelease())
        return 10;
    return 0;
}

int NativeSwitchBlock::transfer(uint64_t lba, uint32_t sectors, bool write, uint8_t *bytes)
{
    const uint32_t length = sectors * kSector;
    if (lba > UINT32_MAX || sectors == 0 || sectors > (write ? kSafeSectors : kBounceSectors))
        return -1;
    if (write && bytes != bounce)
        memcpy(bounce, bytes, length);
    // Clean (write) or clean+invalidate (read) before the device owns the lines.
    switch_dcache_clean_invalidate_lines(bounce, length);
    switch_dcache_sync();
    if (const int step = run(lba, sectors, write, 0))
        return step;
    if (!write) {
        switch_dcache_invalidate_lines(bounce, length);
        switch_dcache_sync();
        if (bytes != bounce)
            memcpy(bytes, bounce, length);
    }
    return 0;
}

// Logs a failed data command and recovers the card. True when ADMA2 itself failed (the host's DMA, not the card or the
// bus): no retry and no re-tune then; the caller moves the data through the bounce buffer instead.
bool NativeSwitchBlock::failed(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write,
                               unsigned descriptors, bool retry)
{
    IOLog("SwitchSD: %s%s lba=%llu n=%u failed step=%d status=0x%08x present=0x%08x acmd=0x%04x\n",
          retry ? "retry " : "", write ? "write" : "read", lba, sectors, step, status, r32(PRESENT),
          r16(AUTO_CMD_ERROR));
    stats.errors++;
    stats.lastError = static_cast<uint32_t>(step);
    stats.lastStatus = status;
    const bool host = step == kStepAdmaError;
    if (host) {
        stats.admaErrors++;
        IOLog("SwitchSD: ADMA2 error: state 0x%02x, at 0x%llx (table 0x%llx, %u descriptors)\n", admaErrorState,
              admaErrorAddress, tablePA, descriptors);
    }
    recover(true);
    return host;
}

// One data command (command(); `descriptors` as there) with the error handling every data command gets: a failure is
// logged and the card recovered; at SDR104 a data error is most often a sampling point that drifted (temperature), so the
// bus is tuned again before the one retry, as the Linux MMC core re-tunes on CRC errors. Failing again right after a
// re-tune, later I/O runs one clock step slower (hekate's own answer to errors at SDR104 is its slower SDR82,
// sd.c:96-117).
int NativeSwitchBlock::run(uint64_t lba, uint32_t sectors, bool write, unsigned descriptors)
{
    uint32_t status = 0;
    const int step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status);
    return step ? retry(step, status, lba, sectors, write, descriptors) : 0;
}

// run()'s error handling for a command that failed with `step` (also for the worker's split ADMA2 commands): logged and
// recovered, re-tuned, issued once more. r14-sdcq: the retry stops by Auto CMD12 whatever the first try's framing was.
int NativeSwitchBlock::retry(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write,
                             unsigned descriptors)
{
    if (failed(step, status, lba, sectors, write, descriptors, false))
        return step;
    stats.retries++;
    if (tuned)
        retune(false);
    if (write && !descriptors) {
        switch_dcache_clean_invalidate_lines(bounce, static_cast<uintptr_t>(sectors) * kSector);
        switch_dcache_sync();
    }
    step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status);
    if (step && !failed(step, status, lba, sectors, write, descriptors, true) && tuned && speed + 1 < kSpeedCount)
        retune(true);
    return step;
}

void NativeSwitchBlock::submit(Request *request)
{
    request->queuedNs = switch_now_ns();
    IOLockLock(queueLock);
    lastIoNs = request->queuedNs;
    if (request->write)
        switchSdTrimClear(&trim, request->lba, request->sectors);
    if (tail) {
        tail->next = request;
    } else {
        head = request;
    }
    tail = request;
    if (++queued > stats.maxQueued)
        stats.maxQueued = queued;
    if (!request->write)
        ++queuedReads[request->klass];
    if (!stats.firstNs)
        stats.firstNs = statsLastNs = request->queuedNs;
    __atomic_fetch_add(&submitted, 1, __ATOMIC_RELAXED);
    IOLockWakeup(queueLock, &head, true);
    IOLockUnlock(queueLock);
}

IOReturn NativeSwitchBlockDevice::doUnmap(IOBlockStorageDeviceExtent *extents, UInt32 count, IOStorageUnmapOptions)
{
    return owner->unmap(extents, count, blocks);
}

// Thread context: queues the extents for the worker and sleeps until it has run them, so the controller keeps one
// thread and an unmap never lands between a chain's commands.
IOReturn NativeSwitchBlock::unmap(const IOBlockStorageDeviceExtent *extents, uint32_t count, uint64_t blocks)
{
    if (!canUnmap())
        return kIOReturnUnsupported;
    if (!extents || !count)
        return kIOReturnBadArgument;
    for (uint32_t i = 0; i < count; ++i) {
        if (!extents[i].blockCount || extents[i].blockStart >= blocks ||
            extents[i].blockCount > blocks - extents[i].blockStart)
            return kIOReturnBadArgument;
    }
    if (!sdStatus.discard) {
        // Marked now, erased in idle time: a write queued before then cancels its AUs (submit).
        IOLockLock(queueLock);
        for (uint32_t i = 0; i < count; ++i)
            switchSdTrimMark(&trim, firstSector + extents[i].blockStart, extents[i].blockCount);
        IOLockWakeup(queueLock, &head, true);
        IOLockUnlock(queueLock);
        return kIOReturnSuccess;
    }
    Unmap job = {nullptr, extents, count, 0, 0, 0, kIOReturnSuccess, false};
    IOLockLock(queueLock);
    Unmap **last = &unmaps;
    while (*last)
        last = &(*last)->next;
    *last = &job;
    IOLockWakeup(queueLock, &head, true);
    while (!job.finished)
        IOLockSleep(queueLock, &unmaps, THREAD_UNINT);
    IOLockUnlock(queueLock);
    return job.result;
}

// Worker thread, no chain in flight: the job's next discard. Extents are the APFS partition's blocks: firstSector maps
// them onto the card. True once the job is finished (result set).
bool NativeSwitchBlock::stepUnmap(Unmap &job)
{
    if (job.at == job.end) {
        const IOBlockStorageDeviceExtent &extent = job.extents[job.index++];
        job.at = firstSector + extent.blockStart;
        job.end = job.at + extent.blockCount;
    }
    const uint64_t length = job.end - job.at < kDiscardSectors ? job.end - job.at : kDiscardSectors;
    cqLeave();
    if (const int step = erase(job.at, job.at + length - 1, kSdDiscardArgument, kDiscardLimitNs)) {
        switch_refusal_k(switch_refusal_key_with(kUnmapRefusal, static_cast<uint64_t>(step)), "SwitchSD", "discard",
                         "the card failed to discard LBA %llu-%llu (step %d, irq %#x)", job.at, job.at + length - 1,
                         step, cardLastIrq);
        job.result = kIOReturnIOError;
        return true;
    }
    job.at += length;
    return job.at == job.end && job.index == job.count;
}

// CMD32/CMD33 bound the range (block addresses, SDHC/SDXC), CMD38 discards or erases it; nothing may come between them
// (ERASE_SEQ). Busy after CMD38 ends in transfer complete, an interrupt like any other command's.
int NativeSwitchBlock::erase(uint64_t first, uint64_t last, uint32_t argument, uint64_t limitNs)
{
    uint32_t r1 = 0;
    int step = cqCommand(32, static_cast<uint32_t>(first), RSP_R1, &r1, kRegNone);
    if (!step && !(r1 & R1_ERRORS))
        step = cqCommand(33, static_cast<uint32_t>(last), RSP_R1, &r1, kRegNone);
    if (!step && !(r1 & R1_ERRORS))
        step = cqCommand(38, argument, RSP_R1B, &r1, kRegNone, limitNs);
    if (!step && (r1 & R1_ERRORS))
        step = 20;
    if (step)
        recover(false);
    return step;
}

// After setup(), with the SD Status known: a card without discard keeps its unmapped AUs for idle-time erases.
void NativeSwitchBlock::prepareTrim()
{
    if (!haveSsr || sdStatus.discard || !sdStatus.auSectors)
        return;
    const uint64_t words = switchSdTrimWords(firstSector, sectorCount, sdStatus.auSectors);
    trimBytes = words * sizeof(uint64_t);
    trimBits = words ? static_cast<uint64_t *>(IOMallocZero(trimBytes)) : nullptr;
    trimCall = trimBits ? thread_call_allocate_with_options(&NativeSwitchBlock::trimFired, this,
                                                            THREAD_CALL_PRIORITY_LOW, THREAD_CALL_OPTIONS_ONCE)
                        : nullptr;
    if (!trimCall) {
        if (trimBits)
            IOFree(trimBits, trimBytes);
        trimBits = nullptr;
        return;
    }
    switchSdTrimInit(&trim, trimBits, firstSector, sectorCount, sdStatus.auSectors);
}

void NativeSwitchBlock::trimFired(thread_call_param_t owner, thread_call_param_t)
{
    auto *self = static_cast<NativeSwitchBlock *>(owner);
    IOLockLock(self->queueLock);
    IOLockWakeup(self->queueLock, &self->head, true);
    IOLockUnlock(self->queueLock);
}

// Under queueLock: there is something to erase and this boot may still erase it.
bool NativeSwitchBlock::trimRunnable() const
{
    return (trim.pending != 0u) && !trimFailed && trimBootSectors < kTrimBootSectors && switch_perf_boot_done();
}

// Under queueLock: when the queue will have been quiet long enough for an erase.
uint64_t NativeSwitchBlock::trimQuietUntil() const
{
    return lastIoNs + (switch_display_is_on() ? kTrimQuietLitNs : kTrimQuietNs);
}

void NativeSwitchBlock::saveTrimCursor(uint64_t cursor)
{
    if (!PEWriteNVRAMProperty(kTrimCursorVariable, &cursor, sizeof(cursor)))
        switch_refusal("SwitchSD", "trim cursor", "the erase cursor could not be kept in NVRAM");
}

// Worker thread, queueLock held on entry and on return, no chain in flight, the queue quiet: one erase of the next due
// run. The run's bits are cleared before the lock is dropped; a write queued while the card erases runs after it.
void NativeSwitchBlock::trimStep()
{
    if (!trimCursorLoaded) {
        // NVRAM's saved variables are back once the Data volume is mounted, long before System Boot Complete.
        IOLockUnlock(queueLock);
        uint64_t cursor = 0;
        unsigned int length = sizeof(cursor);
        const bool found =
            (PEReadNVRAMProperty(kTrimCursorVariable, &cursor, &length) != 0) && length == sizeof(cursor);
        IOLockLock(queueLock);
        trim.cursor = found ? cursor : 0;
        trimCursorLoaded = true;
        if (head || unmaps || suspended || switch_now_ns() < trimQuietUntil())
            return;  // I/O came while NVRAM was read
    }
    uint64_t lba = 0;
    uint32_t aus = 0;
    if (!switchSdTrimNext(&trim, switchSdEraseAus(sdStatus, kTrimRunMs), &lba, &aus))
        return;
    const uint64_t sectors = static_cast<uint64_t>(aus) * trim.auSectors;
    working = true;
    IOLockUnlock(queueLock);
    cqLeave();
    const uint64_t began = switch_now_ns();
    const int step = erase(lba, lba + sectors - 1, kSdEraseArgument,
                           switchSdEraseTimeoutMs(sdStatus, aus) * 1000000ULL + kTrimMarginNs);
    const uint64_t busy = switch_now_ns() - began;
    IOLockLock(queueLock);
    if (step) {
        trimFailed = true;
        switch_refusal_k(switch_refusal_key_with(kEraseRefusal, static_cast<uint64_t>(step)), "SwitchSD", "erase",
                         "the card failed to erase LBA %llu-%llu (step %d, irq %#x): no more erases this boot", lba,
                         lba + sectors - 1, step, cardLastIrq);
    } else {
        __atomic_fetch_add(&stats.trimErases, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&stats.trimSectors, sectors, __ATOMIC_RELAXED);
        __atomic_fetch_add(&stats.trimNs, busy, __ATOMIC_RELAXED);
        trimBootSectors += sectors;
        trimSinceSave += aus;
    }
    const bool save =
        trimSinceSave >= kTrimCursorEveryAus || trimBootSectors >= kTrimBootSectors || (trim.pending == 0u);
    const uint64_t cursor = trim.cursor;
    if (save)
        trimSinceSave = 0;
    working = false;
    if (suspended)
        IOLockWakeup(queueLock, &working, false);
    if (save) {
        IOLockUnlock(queueLock);
        saveTrimCursor(cursor);
        IOLockLock(queueLock);
    }
}

// How the ADMA2 path may reach a page of managed RAM. kCacheable: write-back cacheable (VM_WIMG_DEFAULT 0x2), maintained
// by line through the kernel's physical aperture. kUncached: not cacheable at all (VM_MEM_NOT_CACHEABLE 0x4: write-
// combined, device, real-time; osfmk/arm/memory_types.h). XNU gives every mapping of a page its attributes, the
// physical aperture's included (pmap_update_cache_attributes_locked), so such a page has no line to maintain, which is
// why dcache_incoherent_io_store64 skips it. Anything else (inner-only write-back, write-through, not managed RAM)
// takes the bounce buffer, where copypv reaches it with its own attributes.
namespace {
enum class DmaPage : uint8_t {
    kRefused,
    kCacheable,
    kUncached
};
}  // namespace

static DmaPage dmaPage(uint64_t page)
{
    constexpr unsigned kWimgDefault = 0x2;
    constexpr unsigned kWimgNotCacheable = 0x4;
    const ppnum_t pn = static_cast<ppnum_t>(atop_64(page));
    if (page < gPhysBase || page - gPhysBase >= gPhysSize || (pmap_valid_page(pn) == 0))
        return DmaPage::kRefused;
    const unsigned attributes = pmap_cache_attributes(pn);
    if (attributes == kWimgDefault)
        return DmaPage::kCacheable;
    return (attributes & kWimgNotCacheable) != 0 ? DmaPage::kUncached : DmaPage::kRefused;
}

static bool cacheableRam(uint64_t page)
{
    return dmaPage(page) == DmaPage::kCacheable;
}

// By line over every byte the table moves, through the kernel's physical aperture: clean (before a write: the card reads
// what the CPU wrote) or invalidate (before a read: no dirty line of the client's is evicted over what the card writes;
// after it: drops what the A57 prefetched meanwhile), or both for a read whose fill must reach DRAM first (ReadFill). The
// table only holds whole lines of managed RAM (SwitchAdma.h), so no other data shares a line, the card overwrites every
// line of a read, and phystokv covers every page. `uncached`: the table reaches a non-cacheable page somewhere, so each
// page is asked (dmaPage) and only cacheable ones are maintained.
// r14-sdcq: entries into [skip, skip + skipBytes) (the gap sink, which the CPU never touches) are left out.
static void syncLines(switch_dcache_op op, const SwitchAdmaTable &table, bool uncached, uint64_t skip = 0,
                      uint64_t skipBytes = 0)
{
    for (uint32_t i = 0; i < table.count; ++i) {
        uint64_t pa = switchAdmaAddress(table.entries[i]);
        if (pa - skip < skipBytes)
            continue;
        const uint64_t end = pa + table.entries[i].length;
        if (!uncached) {
            switch_dcache_phys_lines(op, pa, end - pa);
            continue;
        }
        while (pa < end) {
            const uint64_t page = (pa | (kAdmaPage - 1)) + 1;
            const uint64_t stop = page < end ? page : end;
            if (dmaPage(pa) == DmaPage::kCacheable)
                switch_dcache_phys_lines(op, pa, stop - pa);
            pa = stop;
        }
    }
}

// The descriptor table for a chain, from each request's physical segments; kAdmaOk, or why the chain takes the bounce
// buffer instead. How far up it may reach depends on the direction and on how far ADMA2 above 4 GiB is proven (high).
// r14-sdcq: a command-queue task (`task`) reaches above 4 GiB only once that is proven: the proof's SDMA read is a legacy
// command, which CQ mode does not allow; pages up there are staged through the bounce buffer meanwhile.
SwitchAdmaRefusal NativeSwitchBlock::buildTable(Request *first, uint32_t sectors, bool write, bool task)
{
    table.reset();
    tableUncached = false;
    staging.count = 0;
    const uint64_t limit = task && high.state == SwitchAdmaHigh::kProving ? kAdma4GiB : high.limit(write);
    table.limit = limit;
    const auto pageOk = [&](uint64_t page) {
        const DmaPage kind = dmaPage(page);
        if (kind == DmaPage::kUncached)
            tableUncached = true;
        return kind != DmaPage::kRefused && high.pageOk(page, write);
    };
    // r14-sdcq: a read chain's gaps go to the sink, one descriptor each (no gap is longer than the sink).
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): a chain always has its first request
    uint64_t lba = first->lba;
    const auto gap = [&](const Request *r) -> uint64_t {
        return (r->lba - lba) * kSector;
    };
    if (!write && stagingOn) {
        // r12-sdio: a read's refused segments alone through the bounce buffer (SwitchAdmaStaging).
        staging.reset(&table, bouncePA, stages, kStages);
        table.limit = limit;
        for (const Request *r = first; r; r = r->next) {
            SwitchAdmaRefusal why = kAdmaOk;
            if (gap(r)) {
                why = gap(r) > static_cast<uint64_t>(kGapSectors) * kSector || !sinkPA
                          ? kAdmaMissing
                          : staging.addSink(sinkPA, gap(r));
            }
            if (!why)
                why = staging.addMemory(r->buffer, static_cast<uint64_t>(r->sectors) * kSector, pageOk);
            if (why) {
                staging.count = 0;
                return why;
            }
            lba = r->lba + r->sectors;
        }
        if (!staging.finish()) {
            staging.count = 0;
            return kAdmaFull;
        }
        return table.bytes == static_cast<uint64_t>(sectors) * kSector ? kAdmaOk : kAdmaMissing;
    }
    for (const Request *r = first; r; r = r->next) {
        SwitchAdmaRefusal why = kAdmaOk;
        if (gap(r)) {
            why = gap(r) > static_cast<uint64_t>(kGapSectors) * kSector || !sinkPA ? kAdmaMissing
                                                                                   : table.addSealed(sinkPA, gap(r));
        }
        if (!why)
            why = table.addMemory(r->buffer, static_cast<uint64_t>(r->sectors) * kSector, pageOk);
        if (why)
            return why;
        lba = r->lba + r->sectors;
    }
    return table.bytes == static_cast<uint64_t>(sectors) * kSector && table.finish() ? kAdmaOk : kAdmaMissing;
}

// An ADMA2 command for the chain in `table` (buildTable) goes straight to or from the requests' pages. Before it
// (prepareDirect): the descriptors cleaned, and the pages cleaned (write) or invalidated (read). After it (finishDirect):
// a read's pages invalidated again. Linux's arm64 dma_map_sg cleans before any transfer and XNU's IODMACommand does too
// (then cleans and invalidates after); both must allow for a device that leaves part of a buffer unwritten. Here every
// line of a read's table is whole and the card overwrites it or the read fails, so a dirty line is dropped rather than
// written back. A read whose pages hold a fill to keep (ReadFill) is cleaned and invalidated instead.
void NativeSwitchBlock::prepareDirect(bool write, ReadFill fill)
{
    const uint64_t began = switch_now_ns();
    switch_dcache_clean_lines(table.entries, table.count * sizeof(SwitchAdmaDescriptor));
    for (uint32_t i = 0; fill == ReadFill::kPoisonHigh && !write && i < table.count; ++i) {
        uint64_t pa = switchAdmaAddress(table.entries[i]);
        for (const uint64_t end = pa + table.entries[i].length; pa < end;) {
            const uint64_t page = (pa | (kAdmaPage - 1)) + 1;
            const uint64_t stop = page < end ? page : end;
            if (pa >= kAdma4GiB)
                memset(reinterpret_cast<void *>(phystokv(pa)), kPoison, stop - pa);
            pa = stop;
        }
    }
    if (write) {
        syncLines(SWITCH_DCACHE_CLEAN, table, tableUncached);
    } else {
        syncLines(fill == ReadFill::kNone ? SWITCH_DCACHE_INVALIDATE : SWITCH_DCACHE_CLEAN_INVALIDATE, table,
                  tableUncached, sinkPA, sinkPA ? static_cast<uint64_t>(kGapSectors) * kSector : 0);
    }
    if (tableUncached)
        SWITCH_SD_DEBUG_COUNT(uncachedChains);
    switch_dcache_sync();
    stats.syncNs += switch_now_ns() - began;
}

// r12-sdio: a read's staged segments from the bounce buffer out to their requests (after finishDirect). False when a copy
// failed (the chain completes with an I/O error).
bool NativeSwitchBlock::copyStaged()
{
    const uint64_t began = switch_now_ns();
    bool ok = true;
    for (uint32_t i = 0; ok && i < staging.count; ++i) {
        const Staging::Stage &stageAt = staging.stages[i];
        ok = stage(stageAt.memory, static_cast<uint32_t>(stageAt.offset), bounce + stageAt.at, stageAt.length, true);
    }
    stats.copyNs += switch_now_ns() - began;
    return ok;
}

void NativeSwitchBlock::finishDirect(bool write)
{
    if (write)
        return;
    const uint64_t began = switch_now_ns();
    syncLines(SWITCH_DCACHE_INVALIDATE, table, tableUncached, sinkPA,
              sinkPA ? static_cast<uint64_t>(kGapSectors) * kSector : 0);
    switch_dcache_sync();
    stats.syncNs += switch_now_ns() - began;
}

// The chain's bytes [from, from + length) between the requests' memory and the start of `buffer` (the bounce buffer, or
// the proofs' SDMA copy). r14-sdcq: toward the requests a read chain's gaps are skipped; from the requests (a write, or a
// read-back's comparison copy) every byte must come from one (switchChainCopy).
bool NativeSwitchBlock::stageChain(Request *first, uint32_t from, uint32_t length, uint8_t *buffer, bool toClient)
{
    const uint64_t began = switch_now_ns();
    const bool ok = switchChainCopy(
        first, from, length, buffer,
        [&](Request *r, uint32_t offset, uint8_t *at, uint32_t n) {
            return stage(r->buffer, offset, at, n, toClient);
        },
        toClient);
    stats.copyNs += switch_now_ns() - began;
    return ok;
}

// The chain through the bounce buffer by SDMA, in as many commands as SDMA's own limits need (writes kSafeSectors, reads
// sdmaReadSectors: a chain merged for ADMA2 can be longer), each piece staged before or copied out after its command.
bool NativeSwitchBlock::bounceChain(Request *first, uint32_t sectors)
{
    const bool write = first->write;
    const uint32_t limit = write ? kSafeSectors : sdmaReadSectors;
    for (uint32_t done = 0; done < sectors;) {
        const uint32_t n = sectors - done < limit ? sectors - done : limit;
        if (write && !stageChain(first, done * kSector, n * kSector, bounce, false))
            return false;
        if (transfer(first->lba + done, n, write, bounce))
            return false;
        if (!write && !stageChain(first, done * kSector, n * kSector, bounce, true))
            return false;
        done += n;
    }
    return true;
}

#if DEVELOPMENT || DEBUG
// The first kWriteChecks ADMA2 writes of a boot, and the first writes from pages above 4 GiB (high), are read back by SDMA
// (the proven path) and compared with the requests' memory. False only for data that differs: the caller then writes the
// chain again through the bounce buffer and ADMA2 writes stay off until reboot. A read-back that fails proves nothing
// either way and is not counted.
bool NativeSwitchBlock::proveWrite(Request *first, uint32_t sectors, bool cmd23)
{
    const uint32_t length = sectors * kSector;
    if (!checkBuffer || sectors > kBounceSectors)
        return true;
    for (uint32_t done = 0; done < sectors;) {
        const uint32_t n = sectors - done < kSafeSectors ? sectors - done : kSafeSectors;
        if (transfer(first->lba + done, n, false, checkBuffer + static_cast<size_t>(done) * kSector))
            return true;
        done += n;
    }
    if (!stageChain(first, 0, length, bounce, false))
        return true;
    if (!memcmp(bounce, checkBuffer, length)) {
        stats.verified++;
        if (writeChecks)
            --writeChecks;
        if (table.high)
            high.writeChecked();
        if (sectors > kSafeSectors && largeWriteChecks)
            --largeWriteChecks;
        if (cmd23 && cmd23WriteChecks)
            --cmd23WriteChecks;
        releaseChecks();
        return true;
    }
    uint32_t at = 0;
    while (bounce[at] == checkBuffer[at])
        ++at;
    if (cmd23) {
        // r14-sdcq: Auto CMD23 is what is new about this write (ADMA2 writes have read back equal for many boots).
        IOLog("SwitchSD: CMD23 write lba=%llu n=%u reads back different from byte %u; written again through the bounce "
              "buffer, writes stop by CMD12 until reboot\n",
              first->lba, sectors, at);
        cmd23Writes = false;
    } else if (sectors > kSafeSectors) {
        // r12-sdio: only writes over 256 KiB are new: they take the bounce path's 256 KiB SDMA commands from now on.
        IOLog("SwitchSD: ADMA2 write lba=%llu n=%u (%llu bytes above 4 GiB) reads back different from byte %u; written "
              "again through the bounce buffer, writes over 256 KiB in 256 KiB commands until reboot\n",
              first->lba, sectors, table.high, at);
        writeSectors = kSafeSectors;
    } else {
        IOLog("SwitchSD: ADMA2 write lba=%llu n=%u (%llu bytes above 4 GiB) reads back different from byte %u; written "
              "again through the bounce buffer, ADMA2 writes off until reboot\n",
              first->lba, sectors, table.high, at);
        admaWrites = false;
    }
    releaseChecks();
    return false;
}

// r12-sdio: a read chain whose table reached above 4 GiB while that is being proven (high), right after its ADMA2 command
// (prepareDirect poisoned those pages first): the same blocks again by SDMA into checkBuffer, compared with every byte the
// table moved. Equal: one proof more (the last one turns ADMA2 above 4 GiB on, one line). Different: SDMA's copy goes into
// the requests' memory, so they complete with the proven path's bytes, and ADMA2 stays below 4 GiB until reboot (one
// line). If the SDMA read fails, nothing is proven and the chain is read again through the bounce buffer. False only when
// the requests' bytes could not be made right (they complete with an I/O error).
bool NativeSwitchBlock::proveHigh(Request *first, uint32_t sectors)
{
    const uint32_t length = sectors * kSector;
    for (uint32_t done = 0; checkBuffer && done < sectors;) {
        const uint32_t n = sectors - done < kSafeSectors ? sectors - done : kSafeSectors;
        if (transfer(first->lba + done, n, false, checkBuffer + static_cast<size_t>(done) * kSector))
            return bounceChain(first, sectors);
        done += n;
    }
    if (!checkBuffer)
        return bounceChain(first, sectors);
    // The table's descriptors are in chain order: compare the requests' own pages page by page through the kernel's
    // physical aperture (as syncLines reaches them), counting what still holds the poison. Runs staged in the bounce
    // buffer were copied out already (and the buffer has been read into since): their bytes are skipped here.
    uint64_t at = 0;
    uint64_t poisoned = 0;
    uint64_t differs = UINT64_MAX;
    uint64_t differsPA = 0;
    for (uint32_t i = 0; i < table.count; ++i) {
        uint64_t pa = switchAdmaAddress(table.entries[i]);
        if ((pa >= bouncePA && pa - bouncePA < kBounceBytes) || inSink(pa)) {
            at += table.entries[i].length;  // r14-sdcq: a gap's bytes in the sink are nobody's
            continue;
        }
        for (const uint64_t end = pa + table.entries[i].length; pa < end;) {
            const uint64_t page = (pa | (kAdmaPage - 1)) + 1;
            const uint64_t stop = page < end ? page : end;
            const auto *memory = reinterpret_cast<const uint8_t *>(phystokv(pa));
            if (memcmp(memory, checkBuffer + at, stop - pa)) {
                for (uint64_t k = 0; k < stop - pa; ++k) {
                    if (memory[k] == checkBuffer[at + k])
                        continue;
                    if (differs == UINT64_MAX) {
                        differs = at + k;
                        differsPA = pa + k;
                    }
                    if (memory[k] == kPoison)
                        ++poisoned;
                }
            }
            at += stop - pa;
            pa = stop;
        }
    }
    if (differs == UINT64_MAX) {
        const uint64_t bytes = table.high;
        if (high.proved(bytes)) {
            IOLog("SwitchSD: ADMA2 above 4 GiB proven: %u reads (%llu KiB up there) match SDMA; requests' pages go "
                  "straight up to %llu GiB, the first %u writes from up there read back\n",
                  SwitchAdmaHigh::kProofs, high.provenBytes >> 10, kAdmaDmaLimit >> 30, high.writeProofsLeft);
        }
        releaseChecks();
        return true;
    }
    high.refuse();
    IOLog("SwitchSD: ADMA2 read lba=%llu n=%u (%llu bytes above 4 GiB) differs from SDMA at byte %llu (PA 0x%llx, %llu "
          "bytes still poisoned); SDMA's copy used, ADMA2 stays below 4 GiB until reboot\n",
          first->lba, sectors, table.high, differs, differsPA, poisoned);
    const bool repaired = stageChain(first, 0, length, checkBuffer, true);
    releaseChecks();
    return repaired;
}

// r14-sdcq: a read chain's blocks again by SDMA (the proven path, stopped by CMD12) into checkBuffer, and each request's
// bytes compared with their place there (a gap's place is nobody's). Differs: SDMA's bytes are put into the requests, so
// they complete with the proven path's data (*at: the first byte that differed). Unproven: the SDMA read failed (or there
// is no buffer), so nothing is known; after a failed SDMA read the chain is read again through the bounce buffer. Broken:
// the requests' bytes could not be made right (they complete with an I/O error).
NativeSwitchBlock::ChainProof NativeSwitchBlock::compareChain(Request *first, uint32_t sectors, uint64_t *at)
{
    *at = 0;
    if (!checkBuffer || sectors > kBounceSectors)
        return kChainUnproven;
    for (uint32_t done = 0; done < sectors;) {
        const uint32_t n = sectors - done < kSafeSectors ? sectors - done : kSafeSectors;
        if (transfer(first->lba + done, n, false, checkBuffer + static_cast<size_t>(done) * kSector))
            return bounceChain(first, sectors) ? kChainUnproven : kChainBroken;
        done += n;
    }
    // Each request's bytes into the bounce buffer at its place in the command.
    for (const Request *r = first; r; r = r->next) {
        const uint32_t from = static_cast<uint32_t>(r->lba - first->lba) * kSector;
        const uint32_t length = r->sectors * kSector;
        const bool copied = stage(r->buffer, 0, bounce + from, length, false);
        if (copied && !memcmp(bounce + from, checkBuffer + from, length))
            continue;
        if (copied) {
            *at = from;
            while (bounce[*at] == checkBuffer[*at])
                ++*at;
        }
        const bool repaired = stageChain(first, 0, sectors * kSector, checkBuffer, true);
        if (!repaired)
            return kChainBroken;
        return copied ? kChainDiffers : kChainUnproven;
    }
    return kChainMatches;
}

// r14-sdcq: the first kGapProofs read chains with gaps of every boot, right after their command (compareChain). Equal: one
// proof more (the last one logs). Different: the requests get SDMA's bytes and gap reads stay off until reboot (one
// line). False only when the requests' bytes could not be made right.
bool NativeSwitchBlock::proveGaps(Request *first, uint32_t sectors)
{
    uint64_t at = 0;
    const ChainProof proof = compareChain(first, sectors, &at);
    if (proof == kChainMatches) {
        ++stats.gapProven;
        if (gapProofs) {
            --gapProofs;
            if (!gapProofs)
                IOLog("SwitchSD: gap reads proven: %u chains read through their gaps match SDMA\n", kGapProofs);
        }
    } else if (proof == kChainDiffers) {
        gapsOn = false;
        IOLog("SwitchSD: read chain lba=%llu n=%u with gaps differs from SDMA at byte %llu; SDMA's copy used, "
              "gap reads off until reboot\n",
              first->lba, sectors, at);
    }
    releaseChecks();
    return proof != kChainBroken;
}

#endif

// The proofs' SDMA buffer, once neither the first writes nor ADMA2 above 4 GiB need one.
void NativeSwitchBlock::releaseChecks()
{
    if (!checkBuffer || (admaWrites && (writeChecks || (largeWriteChecks && writeSectors > kSafeSectors))) ||
        (adma && high.checking(admaWrites)) || (admaWrites && cmd23Writes && cmd23WriteChecks) ||
        (gapsOn && gapProofs) || ((cq.usable() || cq.stuck) && cqProofs))  // r18-sdcq: back after a power cycle
        return;
    IOFreeData(checkBuffer, kBounceBytes);
    checkBuffer = nullptr;
}

// One card command for a chain of requests that continue each other on the card (same
// direction, each starting where the previous ended, worker() built it in FIFO order); then
// every request completes with its own length. A single request is a chain of one.
// r12-sdio: in three steps (Batch). beginBatch: the checks, the table and, for a chain that is one ADMA2 command, the
// cache maintenance and the command issued. Nothing in it waits for the card.
void NativeSwitchBlock::beginBatch(Batch &b)
{
    Request *const first = b.first;
    const uint32_t sectors = b.sectors;
    // Each request was checked against the partition window in doAsyncReadWrite and the
    // chain is their contiguous union; checking the command itself keeps any bug here from
    // ever reaching outside the APFS partition (issue() checks every write again).
    if (first->lba < firstSector || sectors > sectorCount || first->lba - firstSector > sectorCount - sectors) {
        panic("SwitchSD: command lba=%llu n=%u outside the APFS partition [%llu, +%llu)", first->lba, sectors,
              firstSector, sectorCount);
    }
    // r14-sdcq: the sectors no request of the chain covers (a read chain's gaps). A write chain has none: every byte it
    // puts on the card comes from its requests (pickLocked takes only the requests right behind a write).
    uint64_t payload = 0;
    for (const Request *r = first; r; r = r->next)
        payload += r->sectors;
    if (first->write && payload != sectors)
        panic("SwitchSD: write chain lba=%llu n=%u carries %llu sectors of requests", first->lba, sectors, payload);
    b.gapSectors = static_cast<uint32_t>(sectors - payload);
    b.write = first->write;
    b.beganNs = switch_now_ns();
    b.overlapNs = 0;
    b.issued = false;
    b.issueStep = 0;
    // r10-adma2: straight between the card and the requests' pages by ADMA2 when it is on (writes: unless proveWrite
    // turned them off) and the whole chain qualifies; through the bounce buffer otherwise, counted by why.
    b.why = kAdmaOk;
    b.direct = adma && (!b.write || (admaWrites && sectors <= writeSectors)) &&
               (b.why = buildTable(first, sectors, b.write)) == kAdmaOk;
    // r12-sdio: a read that reaches above 4 GiB while that is unproven is compared with SDMA before it completes.
    b.proving = SwitchAdmaHigh::kLiveProofs && b.direct && high.proofDue(b.write, table.high);
    b.staged = b.direct && (staging.count != 0u);
    b.cmd23 = b.direct && sectors > 1 && (b.write ? cmd23Writes : cmd23Reads);
    if (!b.direct)
        return;
    prepareDirect(b.write, b.proving ? ReadFill::kPoisonHigh : ReadFill::kNone);
    b.issueStep = issue(static_cast<uint32_t>(first->lba), sectors, b.write, table.count, b.cmd23);
    b.issued = true;
}

// endBatch: the issued command waited for (the interrupt, unless it finished meanwhile), retried and checked, then the
// proofs; or the whole bounce path. The stats.
void NativeSwitchBlock::endBatch(Batch &b)
{
    Request *const first = b.first;
    const uint32_t sectors = b.sectors;
    const bool write = b.write;
    IOReturn result = kIOReturnSuccess;
    if (b.direct) {
        uint32_t status = 0;
        bool early = false;
        int step = b.issueStep ? b.issueStep : complete(sectors, write, table.count, &status, &early, b.cmd23);
        if (b.cmd23)
            ++stats.cmd23;
        if (!b.issueStep && b.overlapNs) {
            ++stats.overlapped;
            if (early)
                ++stats.early;
        }
        if (step)
            step = retry(step, status, first->lba, sectors, write, table.count);
        finishDirect(write);
        // r12-sdio: staged segments out to their requests before any proof reuses the bounce buffer.
        if (!step && b.staged) {
            if (!copyStaged())
                result = kIOReturnIOError;
            ++stats.stagedChains;
            stats.stagedBytes += staging.staged;
        }
        if (step == kStepAdmaError && b.proving) {
            // Only the pages above 4 GiB are new to the engine: they stay out of it, and this chain goes by SDMA.
            high.refuse();
            b.direct = false;
            IOLog("SwitchSD: ADMA2 error on a read above 4 GiB (state 0x%02x at 0x%llx); ADMA2 stays below 4 GiB until "
                  "reboot\n",
                  admaErrorState, admaErrorAddress);
            releaseChecks();
        } else if (step == kStepAdmaError) {
            // The host's DMA failed, not the card: this chain and every later one through the bounce buffer.
            adma = false;
            b.direct = false;
            IOLog("SwitchSD: ADMA2 off until reboot (step %d); requests go through the bounce buffer\n", step);
        } else if (step) {
            result = kIOReturnIOError;
#if DEVELOPMENT || DEBUG
        } else if (b.proving) {
            if (!proveHigh(first, sectors))
                result = kIOReturnIOError;
        } else if (b.gapSectors && gapProofs) {
            if (!proveGaps(first, sectors))
                result = kIOReturnIOError;
        } else if (write &&
                   (writeChecks || high.writeCheckDue(write, table.high) ||
                    (sectors > kSafeSectors && largeWriteChecks) || (b.cmd23 && cmd23WriteChecks)) &&
                   !proveWrite(first, sectors, b.cmd23)) {
            b.direct = false;
#endif
        }
    }
    if (b.direct) {
        stats.direct += b.count;
        stats.descriptors += table.count;
    } else {
        if (!bounceChain(first, sectors))
            result = kIOReturnIOError;
        stats.bounced[b.why] += b.count;
    }
    // r16-sdcq: a write completes on flash: with the card's cache off (always, but for one that would not go off) the
    // card's busy said so already; otherwise a flush first (commitWrite), and the write fails if that does.
    if (write && !result && !commitWrite())
        result = kIOReturnIOError;
    b.result = result;
    account(b);
}

// The stats for a chain whose command (or r14-sdcq: task) has ended. Card and copies first, completions after: busy time
// covers the card, the copies and the cache maintenance, completeNs the callbacks (IOStorageFamily/APFS work that runs on
// this thread; r12-sdio: while the next command runs, their time inside this chain's span is not this chain's busy time).
void NativeSwitchBlock::account(Batch &b)
{
    const bool write = b.write;
    const IOReturn result = b.result;
    for (const Request *r = b.first; r; r = r->next) {
        const uint32_t length = r->sectors * kSector;
        if (write) {
            stats.writes++;
            if (!result)
                stats.writeBytes += length;
        } else {
            stats.reads++;
            if (!result)
                stats.readBytes += length;
        }
        if (r->sectors <= 8)
            stats.small++;
        if (r->lba == lastEnd)
            stats.sequential++;
        lastEnd = r->lba + r->sectors;
    }
    const uint64_t span = switch_now_ns() - b.beganNs;
    const uint64_t latency = span > b.overlapNs ? span - b.overlapNs : 0;
    stats.commands++;
    if (b.gapSectors) {
        ++stats.gapChains;
        stats.gapBytes += static_cast<uint64_t>(b.gapSectors) * kSector;
    }
    stats.busyNs += latency;
    (write ? stats.writeNs : stats.readNs) += latency;
    stats.merged += b.count - 1;
    if (latency > stats.maxLatencyNs)
        stats.maxLatencyNs = latency;
}

// completeBatch: every request of the chain completes with endBatch's result.
void NativeSwitchBlock::completeBatch(Batch &b)
{
    const uint64_t began = switch_now_ns();
    for (Request *r = b.first; r;) {
        Request *next = r->next;
        const uint32_t length = r->sectors * kSector;
        const IOMemoryDescriptor *buffer = r->buffer;
        const IOStorageCompletion completion = r->completion;
        IOFree(r, sizeof(*r));
        buffer->release();
        // IOStorage::complete, without binding another original IOStorageFamily symbol.
        if (completion.action)
            completion.action(completion.target, completion.parameter, b.result, b.result ? 0 : length);
        __atomic_fetch_add(&serviced, 1, __ATOMIC_RELAXED);
        r = next;
    }
    b.first = nullptr;
    stats.completeNs += switch_now_ns() - began;
}

// ---- r14-sdcq: the command queue (SwitchCq.h) ----
//
// One of the queue protocol's own commands, waited for by interrupt as the data commands are (complete()): no data
// (CMD44/CMD45, R1; CMD43, R1b, whose busy end raises transfer complete on this controller, the hardware busy detection
// L4T relies on with MMC_CAP_WAIT_WHILE_BUSY), or one 512-byte block by SDMA through regBuffer (CMD48 from the card,
// CMD49 to it; transfer complete follows CMD49's busy). 0 or a step code (1 bus not free (claimBus), 2/3 an error interrupt
// without/with command complete, 6/7 no end within 1.5 s: the extension registers' limit is 1 s of data access (CMD48) or
// busy (CMD49, a cache flush included: 5.7.2.1/5.7.2.2, 4.17) after the command, plus the block itself; 10 the card's
// DAT0 release after a CMD49's transfer complete failed (awaitRelease), as complete() checks a write); a failure resets
// the CMD and DAT lines. *response gets the R1. r18-sdcq: also on a failure, when the command's response came (command
// complete seen: steps 3, 6 and 10), so the error line has it; the release is confirmed after an R1b's transfer complete
// too (CMD43: "not assign another task until CMD43 is completed", 9.10 4.19.8); the interrupt status each command ended
// with is kept for the line (cardLastIrq).
int NativeSwitchBlock::cqCommand(uint32_t index, uint32_t argument, uint32_t flags, uint32_t *response, int data,
                                 uint64_t limitNs)
{
    *response = 0;
    SwitchSdUse use = SwitchSdUse::kCommand;
    if (data == kRegRead) {
        use = SwitchSdUse::kRead;
    } else if (data == kRegWrite) {
        use = SwitchSdUse::kWrite;
    } else if ((flags & 3) == 3) {
        use = SwitchSdUse::kBusy;
    }
    if (!claimBus(use))
        return 1;
    w32(STATUS, 0xffffffff);
    if (data != kRegNone) {
        switch_dcache_clean_invalidate_lines(regBuffer, kSector);
        switch_dcache_sync();
        w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);
        w32(DMA_LOW, static_cast<uint32_t>(regPA));
        w32(DMA_HIGH, static_cast<uint32_t>(regPA >> 32));
        w16(BLOCK_SIZE, kSector | 0x7000);
        w16(BLOCK_COUNT, 1);
    }
    w32(ARGUMENT, argument);
    uint16_t transfer = 0;
    if (data == kRegRead) {
        transfer = 0x11;
    } else if (data == kRegWrite) {
        transfer = 0x01;
    }
    w16(TRANSFER, transfer);
    switch_dcache_sync();
    w16(COMMAND, static_cast<uint16_t>(index << 8 | flags | (data != kRegNone ? CMD_DATA : 0)));
    noteCommand(static_cast<uint8_t>(index));
    const uint32_t done = data != kRegNone || (flags & 3) == 3 ? INT_TRANSFER : INT_COMMAND;
    const uint64_t limit = limitNs;
    const uint32_t s = awaitStatus(done, limit);
    ended(s);
    const uint32_t answer = r32(RESPONSE);
    int step = 0;
    if (s & INT_ERRORS) {
        step = (s & INT_COMMAND) ? 3 : 2;
    } else if (!(s & done)) {
        step = (s & INT_COMMAND) ? 6 : 7;
    }
    if (!step && !(s & INT_COMMAND))
        step = 4;
    // r16-sdcq: transfer complete follows a CMD49's busy (the card applying the byte, or flushing its cache); DAT0
    // confirms it before the read-back that checks it. r18-sdcq: and an R1b's (CMD43). RESPONSE is read first: CMD13
    // would replace it.
    if (!step && !bus.idle() && awaitRelease())
        step = 10;
    cardLastIrq = s;
    if (step) {
        // r18-sdcq (r8d): not after a command error (timeout, CRC, end bit, index): this controller sets command complete
        // with a timeout too (irq 0x00018001) and RESPONSE still holds the previous command's.
        if ((s & INT_COMMAND) && !(s & 0x000f0000u))
            *response = answer;
        stats.errors++;
        stats.lastError = static_cast<uint32_t>(step);
        stats.lastStatus = s;
        resetLines();
        w32(STATUS, 0xffffffff);
        return step;
    }
    *response = answer;
    w32(STATUS, INT_COMMAND | INT_TRANSFER);
    if (data == kRegRead) {
        switch_dcache_invalidate_lines(regBuffer, kSector);
        switch_dcache_sync();
    }
    return 0;
}

int NativeSwitchBlock::CqBus::command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1) const
{
    return self->cqCommand(index, argument, busy ? RSP_R1B : RSP_R1, r1, kRegNone);
}

// A host byte of the card's performance function (identify(): byte 262 at switchSdCqControlAddress, 260 and 261 right
// before it): CMD49 of `*value`, or CMD48 into it, one byte (LEN 0, no mask write; MIO 0, memory). r16-sdcq: the only
// extension-register bytes this driver ever writes, and only these values (switchSdPerfWriteAddress): 260 Cache Enable
// 00h/01h, 261 Flush Cache 01h, 262 the CQ enable and mode bits 00h/03h. Anything else is a bug here and panics before
// it reaches the card.
int NativeSwitchBlock::CqBus::control(uint32_t byte, bool write, uint8_t *value, uint32_t *r1) const
{
    const uint32_t cqAt = self->cqControlAddress;
    const bool known = byte >= kSdPerfCacheControl && byte <= kSdPerfCqControl;
    uint32_t address = 0;
    if (known)
        address = write ? switchSdPerfWriteAddress(self->extInfo.perf, byte, *value) : cqAt - (kSdPerfCqControl - byte);
    if (!cqAt || cqAt != switchSdCqControlAddress(self->extInfo.perf) || !self->regBuffer || !address ||
        address != cqAt - (kSdPerfCqControl - byte)) {
        panic("SwitchSD: extension register %s fno %u byte %u @0x%05x value 0x%02x refused", write ? "write" : "read",
              unsigned(self->extInfo.perf.fno), byte, address, unsigned(*value));
    }
    if (write) {
        memset(self->regBuffer, 0, kSector);
        self->regBuffer[0] = *value;
    }
    *r1 = 0;
    const int step = self->cqCommand(write ? 49 : 48, switchSdExtArgument(self->extInfo.perf.fno, address, 1), RSP_R1,
                                     r1, write ? kRegWrite : kRegRead);
    if (step)
        return step;
    if (*r1 & R1_ERRORS)
        return 30;
    if (!write)
        *value = self->regBuffer[0];
    return 0;
}

// r18-sdcq: CMD13 SEND_STATUS for the queue's recovery ([31:16] RCA; [15] the Task Status Register instead while CQ is
// enabled, a stuff bit otherwise: 4.7.4), waited for by interrupt like the queue's own commands. Legal in every state
// and in CQ mode (Table 4-74); never polled: one per recovered error.
int NativeSwitchBlock::CqBus::status(bool tasks, uint32_t *r1) const
{
    return self->cqCommand(13, self->rca << 16 | (tasks ? 1u << 15 : 0u), RSP_R1, r1, kRegNone);
}

// Out of CQ mode before a legacy command, and when the queue runs dry. r18-sdcq: a card that will not leave it is
// power-cycled (cqPowerCycle) and the queue stays on (SwitchCq.h powerCycled), kMaxStuck times a boot.
void NativeSwitchBlock::cqLeave()
{
    if (cq.state != CqEngine::kOn || cq.leave(cqBus))
        return;
    cq.error = {};
    cq.error.command = 48;  // its read-backs never showed 00h
    cq.error.previous = 49;
    cq.error.step = -1;
    cq.error.value = cq.lastRead[2];
    cq.error.attempt = 1;
    (void)cq.recover(cqBus);  // CMD13 for the line (stuck: nothing else)
    cqRecovered(cq.error);
    cqPowerCycle();
}

// r18-sdcq: a card that stays in CQ mode cannot take legacy commands (Table 4-74), and CQ mode does not survive power-off:
// resetCard, then the engine knows (the queue on again, or stopped once kMaxStuck power cycles were needed).
void NativeSwitchBlock::cqPowerCycle()
{
    resetCard();
    cq.powerCycled();
    if (cq.stopped)
        cqStopped();
}

// r18-sdcq: the one IOLog line of a queue stopped for the boot (a proof differed, or the card had to be power-cycled out
// of CQ mode kMaxStuck times), once; the proofs' buffer released.
void NativeSwitchBlock::cqStopped()
{
    if (cqStopLogged)
        return;
    cqStopLogged = true;
    IOLog("SwitchSD: command queue off until reboot (%s) after %llu tasks in %llu groups, %llu errors recovered, %llu "
          "groups read the legacy way%s\n",
          cqStopWhy[0] ? cqStopWhy : cq.why, cq.tasks, cq.groups, cq.recoveries, cq.legacyGroups,
          cq.cacheOn ? "; its cache would not go off: every write is flushed" : "");
    releaseChecks();
}

// r18-sdcq: one os_log line per recovered error of the queue, the first kLoggedErrors of the boot and then every 64th (a
// count, no clock): the failed command, its task, the bus's step and interrupt status, its R1 and the error bits set in
// it, CMD13's card status right after (B-type bits there blame the failed command: it got no answer) with its state, the
// Task Status Register, the command before the failed one, the CMD43 abort's answer, and what happens next.
void NativeSwitchBlock::cqRecovered(const SwitchCqError &e) const
{
    constexpr uint64_t kLoggedErrors = 8;
    if (cq.recoveries > kLoggedErrors && cq.recoveries % 64)
        return;
    char bits[96];
    char status[96];
    switchSdR1ErrorNames(e.r1, bits, sizeof(bits));
    switchSdR1ErrorNames(e.cardStatus, status, sizeof(status));
    const char *next = "assigned again after entering CQ mode";
    if (e.outcome == CqEngine::kStuck) {
        next = "the card stays in CQ mode: power-cycled";
    } else if (e.attempt >= 2) {
        next = "the group the legacy way";
    } else if (e.outcome == CqEngine::kInPlace) {
        next = "assigned again in CQ mode";
    }
    const char *cqByte = "";
    if (e.command == 48 && e.step < 0)
        cqByte = (e.value & 1) ? "; the CQ byte reads 01h" : "; the CQ byte reads 00h";
    char out[512];
    snprintf(out, sizeof(out),
             "SwitchSD: CQ error %llu recovered: CMD%u task %u step %d irq 0x%08x R1 0x%08x (%s)%s; CMD13 step %d "
             "status 0x%08x (%s) state %u, tasks step %d 0x%08x; previous CMD%u; CMD43 step %d R1 0x%08x; attempt %u, "
             "%u tasks: %s",
             cq.recoveries, static_cast<unsigned>(e.command), static_cast<unsigned>(e.task), e.step, e.irq, e.r1, bits,
             cqByte, e.statusStep, e.cardStatus, status, static_cast<unsigned>(e.cardStatus >> 9 & 0xf), e.tasksStep,
             e.taskStatus, static_cast<unsigned>(e.previous), e.abortStep, e.abortR1, static_cast<unsigned>(e.attempt),
             static_cast<unsigned>(e.tasks), next);
    os_log(OS_LOG_DEFAULT, "%s", out);
}

// A card that will not leave CQ mode cannot take legacy commands (Table 4-74), and CQ mode does not survive power-off:
// the card is powered off and initialized again as after system sleep (suspend/resume's own steps), then LBA 1 must read
// as at boot. Panics where resume() would (the card cannot be used at all then).
void NativeSwitchBlock::resetCard()
{
    // r16-sdcq: nothing to flush first: every completed write is on flash (the cache is off for writes, or each was
    // flushed: commitWrite); the power cycle clears Cache Enable.
    dmaSuspend();
    if (irqSource)
        irqSource->disable();
    powerOffCard();
    powerOnController();
    if (irqSource)
        irqSource->enable();
    const bool uhs = lowVoltageBus && enablePllc4();
    if (lowVoltageBus && !uhs) {
        panic("SwitchSD: PLLC4 does not lock after the card's power cycle (PLLC4_BASE 0x%08x)",
              switch_mmio_read32(car, CAR_PLLC4_BASE));
    }
    (void)reinitialize(PLLP_KHZ * 2 / (kPllpDivider48MHz + 2), 0xc0 | kLdo3V3, uhs, true);
    uint8_t again[kSector] = {};
    const int step = transfer(1, 1, false, again);
    if (step || memcmp(again, gptHeader, sizeof(again)))
        panic("SwitchSD: LBA 1 differs after the card's power cycle (read step %d, bus %s)", step, busMode);
    dmaResume();
}

// r16-sdcq: a write's last step, on the worker after its command (always out of CQ mode: a legacy command). Nothing while
// the card's cache is off, as it is for every write unless it would not go off at the end of a CQ session; then a flush
// (SwitchCq.h commitWrite), so the write completes on flash all the same. One line the first time a write needs a
// flush and the first time one fails; false: the write is not known to be on flash (it fails, nothing panics).
bool NativeSwitchBlock::commitWrite()
{
    if (!cq.cacheOn)
        return true;
    if (!flushingWrites) {
        flushingWrites = true;
        IOLog("SwitchSD: the card's cache would not go off (Cache Enable reads 0x%02x): every write is flushed before "
              "it completes\n",
              static_cast<unsigned>(cq.lastRead[0]));
    }
    const uint64_t began = switch_now_ns();
    const bool flushed = cq.commitWrite(cqBus);
    ++stats.flushes;
    stats.flushNs += switch_now_ns() - began;
    if (flushed)
        return true;
    ++stats.flushErrors;
    if (!flushFailed) {
        flushFailed = true;
        IOLog("SwitchSD: a write's cache flush failed (Flush Cache reads 0x%02x after %u read-backs): that write, and "
              "any later one whose flush fails, completes with an I/O error\n",
              static_cast<unsigned>(cq.lastRead[kSdPerfFlushControl - kSdPerfCacheControl]), cq.lastReads);
    }
    return false;
}

// r16-sdcq: what the bring-up left in the card's control bytes (the engine's last read-backs) and Task Error Status
// (bytes 8-15, one CMD48), for checkCq's one line.
void NativeSwitchBlock::describeCq(char *out, size_t size)
{
    const uint8_t cache = cq.cacheRead;
    const uint8_t control = cq.cqRead;
    uint32_t r1 = 0;
    uint64_t errors = 0;
    const bool read =
        (regBuffer != nullptr) &&
        (cqCommand(48, switchSdExtArgument(extInfo.perf.fno, extInfo.perf.address, 16), RSP_R1, &r1, kRegRead) == 0) &&
        ((r1 & R1_ERRORS) == 0u);
    for (unsigned i = 0; read && i < 8; ++i)
        errors |= static_cast<uint64_t>(regBuffer[8 + i]) << (8 * i);
    char tasks[24] = "unread";
    if (read)
        snprintf(tasks, sizeof(tasks), "0x%016llx", errors);
    char cacheState[64] = "not offered";
    if (perf.cache && cq.cacheReads) {
        snprintf(cacheState, sizeof(cacheState), "read 0x%02x after 01h, %s now", static_cast<unsigned>(cache),
                 cq.cacheOn ? "would not go off" : "off");
    } else if (perf.cache) {
        strlcpy(cacheState, "not tried", sizeof(cacheState));
    }
    snprintf(out, size, "CQ control reads 0x%02x (%s) after %u read-backs over ~%u ms; cache %s; task errors %s",
             static_cast<unsigned>(control), switchSdCqControlMeaning(control), cq.cqReads,
             cq.cqReads ? cq.cqReads - 1 : 0, cacheState, tasks);
}

// Whether the chain pickLocked just took starts a command-queue group (queueLock held): a read of at most
// kCqTaskSectors with no unproven gaps while the queue is on. Then more chains join, each the oldest queued read that
// overlaps no older queued request (switchNextIndependentRead: never past a write it would see) with the reads that
// continue it (as pickLocked gathers, up to kCqTaskSectors, gaps once proven), until the card's depth or kCqGroupSectors.
// From legacy mode a group needs two chains (one is cheaper as a legacy command than CQ mode's way in and out); in CQ
// mode one will do. The group goes in cqGroups[cqTurn], the picked chain first; returns its size, or 0 for the legacy way.
// r16-sdcq: from legacy mode, cqMinChains (setup(): enough tasks that the queue's measured saving pays for the way in and
// out, at least two).
// r18-sdcq: every chain unlinked here runs: with fewer than cqMinChains from legacy mode (*queue false) they go the
// legacy way one after another (runGroup), where until now the chains after the first were dropped, their requests never
// completed (r8c: no panic, its page-ins and the cryptex reads behind them asleep for good). While the queue is paused
// (SwitchCq.h admit, counted per read that could start a group) nothing is gathered.
unsigned NativeSwitchBlock::groupLocked(Request *first, uint32_t sectors, uint32_t count, bool *queue)
{
    *queue = false;
    if (!cq.usable() || suspended || first->write || sectors > kCqTaskSectors)
        return 0;
    uint64_t payload = 0;
    for (const Request *r = first; r; r = r->next)
        payload += r->sectors;
    if (payload != sectors && gapProofs)
        return 0;
    if (!cq.admit())
        return 0;
    Batch *const group = cqGroups[cqTurn];
    group[0] = {};
    group[0].first = first;
    group[0].sectors = sectors;
    group[0].count = count;
    unsigned n = 1;
    uint32_t total = sectors;
    const unsigned limit = cq.depth < kCqMaxTasks ? cq.depth : kCqMaxTasks;
    const uint32_t maxGap = gapsOn && !gapProofs ? kGapSectors : 0;
    const uint32_t gapBudget = gapsOn && !gapProofs ? kGapBudget : 0;
    const uint64_t pickNs = switch_now_ns();
    while (n < limit && total < kCqGroupSectors && merge && mergeAnywhere) {
        Request *const r = switchNextIndependentRead(head, kCqTaskSectors, kMergeScan);
        if (!r)
            break;
        uint64_t start = 0;
        uint32_t chainSectors = 0;
        const uint32_t chainCount =
            switchGatherReads(head, r, kCqTaskSectors, kMaxChain, kMergeScan, &start, &chainSectors, maxGap, gapBudget);
        if (!switchChainFits(head, start, chainSectors, chainCount, maxGap, gapBudget)) {
            switchClearChain(head);
            break;
        }
        Request *const chain = switchUnlinkChain(head, tail, chainCount);
        takenLocked(chain, chainCount, pickNs);
        group[n] = {};
        group[n].first = chain;
        group[n].sectors = chainSectors;
        group[n].count = chainCount;
        ++n;
        total += chainSectors;
    }
    *queue = n >= (cq.state == CqEngine::kOn ? 1u : cqMinChains);
    return *queue || n > 1 ? n : 0;
}

// A task's command (the engine's next CMD46): its table (beginBatch's builder, with nothing above 4 GiB while that is
// unproven), or the bounce buffer when the chain cannot go straight (a task is at most 256 KiB: one SDMA command there),
// the cache maintenance, the command issued. Nothing in it waits for the card.
void NativeSwitchBlock::beginTask(Batch &b, uint32_t argument)
{
    Request *const first = b.first;
    uint64_t payload = 0;
    for (const Request *r = first; r; r = r->next)
        payload += r->sectors;
    b.gapSectors = static_cast<uint32_t>(b.sectors - payload);
    b.write = false;
    b.beganNs = switch_now_ns();
    b.overlapNs = 0;
    b.issueStep = 0;
    b.cmd23 = false;
    b.proving = false;
    b.why = buildTable(first, b.sectors, false, true);
    b.direct = b.why == kAdmaOk;
    b.staged = b.direct && (staging.count != 0u);
    if (b.direct) {
        prepareDirect(false, ReadFill::kNone);
    } else {
        switch_dcache_clean_invalidate_lines(bounce, static_cast<uintptr_t>(b.sectors) * kSector);
        switch_dcache_sync();
    }
    b.issueStep =
        issue(static_cast<uint32_t>(first->lba), b.sectors, false, b.direct ? table.count : 0, false, argument);
    b.issued = true;
}

// A task's end: its transfer waited for (by interrupt, unless it finished meanwhile), its data out of the bounce buffer if
// it went there, the stats. 0, or the step that failed (the lines recovered and the card's transfer stopped by CMD12;
// the engine's run recovers the queue). r18-sdcq: *r1, the CMD46 response when one came; a failure is the engine's
// recovered error (one os_log line), not failed()'s IOLog, but for ADMA2's own (the host's DMA: that line stays).
int NativeSwitchBlock::endTask(Batch &b, uint32_t *r1)
{
    uint32_t status = 0;
    bool early = false;
    const unsigned descriptors = b.direct ? table.count : 0;
    const int step = b.issueStep ? b.issueStep : complete(b.sectors, false, descriptors, &status, &early, false, true);
    if (!b.issueStep && b.overlapNs) {
        ++stats.overlapped;
        if (early)
            ++stats.early;
    }
    if (b.direct)
        finishDirect(false);
    if (step) {
        *r1 = (status & INT_COMMAND) ? r32(RESPONSE) : 0;
        cardLastIrq = status;
        if (step == kStepAdmaError) {
            (void)failed(step, status, b.first->lba, b.sectors, false, descriptors, false);
        } else {
            stats.errors++;
            stats.lastError = static_cast<uint32_t>(step);
            stats.lastStatus = status;
            recover(true);
        }
        return step;
    }
    IOReturn result = kIOReturnSuccess;
    if (b.direct) {
        if (b.staged) {
            if (!copyStaged())
                result = kIOReturnIOError;
            ++stats.stagedChains;
            stats.stagedBytes += staging.staged;
        }
        stats.direct += b.count;
        stats.descriptors += table.count;
    } else {
        switch_dcache_invalidate_lines(bounce, static_cast<uintptr_t>(b.sectors) * kSector);
        switch_dcache_sync();
        if (!stageChain(b.first, 0, b.sectors * kSector, bounce, true))
            result = kIOReturnIOError;
        stats.bounced[b.why] += b.count;
    }
    b.result = result;
    ++stats.cqTasks;
    account(b);
    return 0;
}

// One command-queue group (groupLocked); `pending` is the batch whose completions have not run yet. r18-sdcq: the engine
// runs it (SwitchCq.h run): into CQ mode, every chain assigned as a task (CMD44 + CMD45), then each executed in order
// (CMD46) while the previous one's completions run; while proofs are due the group's requests are held until, out of CQ
// mode, each task's blocks read again the legacy way match (compareChain). An error is recovered (CMD13, CMD43; one
// os_log line) and the chains not done yet are assigned again once; if that fails too they go the legacy way and the
// queue stays on (a pause after kStrikes such groups in a row). Only a proof that differs stops it for the boot (one
// IOLog line), the whole group then read again the legacy way. `queue` false: fewer chains than a group from legacy mode
// needs, each the legacy way. Every chain's requests complete exactly once. Returns the batch left pending (the last).
Batch *NativeSwitchBlock::runGroup(Batch *group, unsigned n, Batch *pending, bool queue)
{
    const uint64_t began = switch_now_ns();

    // The engine's view of the group: chain i is group[i]; completions that run while a command moves data are that
    // command's overlap (account() takes them out of its latency).
    struct Host {
        NativeSwitchBlock *self;
        Batch *group, *earlier;
        unsigned current;

        void overlapped(uint64_t since) const
        {
            if (group[current].issued)
                group[current].overlapNs += switch_now_ns() - since;
        }

        void beginTask(unsigned i, uint32_t argument)
        {
            current = i;
            self->beginTask(group[i], argument);
        }

        int endTask(unsigned i, uint32_t *r1) const
        {
            return self->endTask(group[i], r1);
        }

        void beginLegacy(unsigned i)
        {
            current = i;
            self->beginBatch(group[i]);
        }

        void endLegacy(unsigned i) const
        {
            self->endBatch(group[i]);
        }

        void complete(unsigned i) const
        {
            const uint64_t since = switch_now_ns();
            self->completeBatch(group[i]);
            overlapped(since);
        }

        void completeEarlier()
        {
            if (!earlier)
                return;
            const uint64_t since = switch_now_ns();
            self->completeBatch(*earlier);
            earlier = nullptr;
            overlapped(since);
        }

        uint8_t prove(unsigned i) const
        {
#if DEVELOPMENT || DEBUG
            uint64_t at = 0;
            const ChainProof proof = self->compareChain(group[i].first, group[i].sectors, &at);
            if (proof == kChainMatches)
                return CqEngine::kProofMatches;
            if (proof == kChainUnproven)
                return CqEngine::kProofUnproven;
            if (proof == kChainBroken) {
                group[i].result = kIOReturnIOError;
                return CqEngine::kProofBroken;
            }
            // The queue stops and the whole group is read again the legacy way: no task of a group that read one
            // block wrong completes with the queue's bytes.
            snprintf(self->cqStopWhy, sizeof(self->cqStopWhy),
                     "task %u (lba %llu n %u) differs from the legacy read at byte %llu", i, group[i].first->lba,
                     group[i].sectors, at);
            return CqEngine::kProofDiffers;
#else
            (void)i;
            return CqEngine::kProofUnproven;
#endif
        }

        void recovered(const SwitchCqError &e) const
        {
            self->cqRecovered(e);
        }

        void powerCycle() const
        {
            self->resetCard();
        }
    } host{this, group, pending, 0};

    if (!queue) {
        cqLeave();  // never in CQ mode here (only legacy mode asks for more chains than one), but legacy needs it out
        CqEngine::runLegacy(host, n);
        return &group[n - 1];
    }
    SwitchCqTask tasks[kCqMaxTasks];
    for (unsigned i = 0; i < n; ++i) {
        // As beginBatch checks every command: no task's blocks reach outside the APFS partition.
        const Request *const first = group[i].first;
        const uint32_t sectors = group[i].sectors;
        if (first->lba < firstSector || sectors > sectorCount || first->lba - firstSector > sectorCount - sectors) {
            panic("SwitchSD: command lba=%llu n=%u outside the APFS partition [%llu, +%llu)", first->lba, sectors,
                  firstSector, sectorCount);
        }
        tasks[i] = {first->lba, sectors};
    }
    const bool proving = SwitchAdmaHigh::kLiveProofs && cqProofs != 0;
    unsigned proven = 0;
    const CqEngine::Outcome outcome = cq.run(cqBus, host, tasks, n, proving, &proven);
    if (proving) {
        const uint32_t before = cqProofs;
        cqProofs = cqProofs > proven ? cqProofs - proven : 0;
        if (before && !cqProofs)
            IOLog("SwitchSD: command queue proven: %u tasks read back the same the legacy way\n", kCqProofs);
        releaseChecks();
    }
    if (outcome == CqEngine::kGroupStopped)
        cqStopped();
    if (cq.pausedNow) {
        os_log(OS_LOG_DEFAULT,
               "SwitchSD: command queue paused for %u groups (%u groups in a row read the legacy way; %llu errors "
               "recovered, %llu groups read the legacy way, %llu pauses)",
               cq.pauseLeft, CqEngine::kStrikes, cq.recoveries, cq.legacyGroups, cq.pauses);
    }
    stats.cqNs += switch_now_ns() - began;
    return &group[n - 1];
}

// The next chain off the queue (queueLock held, queue not empty), unlinked: the head, or a passing read (r6-lows), and the
// queued requests that continue it on the card.
Request *NativeSwitchBlock::pickLocked(uint32_t *chainSectors, uint32_t *chainCount)
{
    // Take the head and every queued request that continues it on the card in the same
    // direction, up to one command's limit: one command, one card access latency, one
    // interrupt instead of several. No write passes another.
    Request *prev = nullptr;
    Request *first = head;
    const uint64_t pickNs = switch_now_ns();
    // r6-lows: a read of a higher IOStoragePriority class goes first (the UI's page-ins, I/O tier 0, behind up to ~100 queued
    // background requests at ~0.5 ms each otherwise): the oldest read of the best class queued, unless it overlaps an older
    // request or the head has waited kAgeNs.
    if (first->next && first->klass && pickNs - first->queuedNs < kAgeNs) {
        unsigned better = 0;
        while (better < first->klass && !queuedReads[better])
            ++better;
        if (better < first->klass) {
            for (Request *before = first, *candidate = first->next; candidate;
                 before = candidate, candidate = candidate->next) {
                if (candidate->write || candidate->klass != better)
                    continue;
                bool overlaps = false;
                for (const Request *older = head; older != candidate && !overlaps; older = older->next) {
                    overlaps = older->lba < candidate->lba + candidate->sectors &&
                               candidate->lba < older->lba + older->sectors;
                }
                if (overlaps)
                    continue;
                prev = before;
                first = candidate;
                ++passed;
                ++passedTotal;
                break;
            }
        }
    }
    uint32_t sectors = first->sectors;
    uint32_t count = 1;
    bool gathered = false;
    if (merge && mergeAnywhere && !first->write) {
        // r12-sdio: a read takes every queued read that continues it on the card, before or after it, wherever it is
        // queued, unless that read overlaps an older request still queued (SwitchQueue.h). Back in card order. The
        // chain must fit its command exactly (tile it, but for gaps within the bounds); one that does not (never seen;
        // host-tested) stays queued as it was, and this boot goes back to the old rule, with one line.
        // r14-sdcq: and, while gap reads are on, queued reads up to kGapSectors away (kGapBudget per command).
        uint64_t start = 0;
        const uint32_t maxGap = gapsOn ? kGapSectors : 0;
        const uint32_t gapBudget = gapsOn ? kGapBudget : 0;
        count = switchGatherReads(head, first, maxSectors, kMaxChain, kMergeScan, &start, &sectors, maxGap, gapBudget);
        if (switchChainFits(head, start, sectors, count, maxGap, gapBudget)) {
            first = switchUnlinkChain(head, tail, count);
            gathered = true;
        } else {
            switchClearChain(head);
            mergeAnywhere = false;
            IOLog("SwitchSD: read chain lba=%llu n=%u (%u requests) does not fit; reads merge only with the requests "
                  "right behind them until reboot\n",
                  start, sectors, count);
            sectors = first->sectors;
            count = 1;
        }
    }
    if (!gathered) {
        Request *last = first;
        const uint32_t limit = first->write ? writeSectors : maxSectors;
        // A passing read's chain passes the older requests too: each request it takes must not overlap any of them either.
        const auto overlapsOlder = [&](const Request *later) {
            if (!prev)
                return false;
            for (const Request *older = head; older != first; older = older->next) {
                if (older->lba < later->lba + later->sectors && later->lba < older->lba + older->sectors)
                    return true;
            }
            return false;
        };
        while (merge && count < kMaxChain && last->next && last->next->write == first->write &&
               last->next->lba == last->lba + last->sectors && sectors + last->next->sectors <= limit &&
               !overlapsOlder(last->next)) {
            last = last->next;
            sectors += last->sectors;
            ++count;
        }
        // Unlink first..last (the head's run, or a passing read's from the middle).
        if (prev) {
            prev->next = last->next;
        } else {
            head = last->next;
        }
        if (tail == last)
            tail = prev;
        last->next = nullptr;
    }
    takenLocked(first, count, pickNs);
    *chainSectors = sectors;
    *chainCount = count;
    return first;
}

// The queue counts and the per-class wait stats for a chain just unlinked (queueLock held).
void NativeSwitchBlock::takenLocked(Request *first, uint32_t count, uint64_t pickNs)
{
    queued -= count;
    for (const Request *r = first; r; r = r->next) {
        if (!r->write)
            --queuedReads[r->klass];
        ClassWait &wait = waits[r->klass];
        const uint64_t waited = pickNs > r->queuedNs ? pickNs - r->queuedNs : 0;
        ++wait.n;
        wait.sumNs += waited;
        if (waited > wait.maxNs)
            wait.maxNs = waited;
    }
}

void NativeSwitchBlock::worker(void *context, wait_result_t)
{
    auto *self = static_cast<NativeSwitchBlock *>(context);
    // r12-sdio: the chain whose command runs (next) and the one whose requests are still to complete (pending), in turns.
    Batch batches[2] = {};
    unsigned turn = 0;
    Batch *pending = nullptr;
    for (;;) {
        IOLockLock(self->queueLock);
        // r14-sdcq: the queue ran dry (or sleep is coming) with the card in CQ mode: out of it before sleeping, so a
        // lone read after a pause goes the legacy way at once and system sleep finds the card out of CQ mode.
        if (!pending && (!self->head || self->suspended) && self->cq.state == CqEngine::kOn) {
            self->working = true;
            IOLockUnlock(self->queueLock);
            self->cqLeave();
            IOLockLock(self->queueLock);
            self->working = false;
            if (self->suspended)
                IOLockWakeup(self->queueLock, &self->working, false);
            IOLockUnlock(self->queueLock);
            continue;
        }
        // Idle: an erase-based trim once the queue has been quiet long enough, else a timer for when it will have been.
        bool trimNow = false;
        while (!pending && ((!self->head && !self->unmaps) || self->suspended)) {
            if (!self->suspended && self->trimRunnable()) {
                const uint64_t now = switch_now_ns();
                const uint64_t until = self->trimQuietUntil();
                if (now >= until) {
                    trimNow = true;
                    break;
                }
                uint64_t wait = 0;
                uint64_t leeway = 0;
                nanoseconds_to_absolutetime(until - now, &wait);
                nanoseconds_to_absolutetime(kTrimMarginNs, &leeway);
                thread_call_enter_delayed_with_leeway(self->trimCall, nullptr, mach_absolute_time() + wait, leeway,
                                                      THREAD_CALL_DELAY_LEEWAY);
            }
            IOLockSleep(self->queueLock, &self->head, THREAD_UNINT);
        }
        if (trimNow) {
            self->trimStep();
            IOLockUnlock(self->queueLock);
            continue;
        }
        // A discard needs no chain in flight: on its turn no chain is picked, so the pending one completes first.
        const bool discardTurn = (self->unmaps != nullptr) && !self->suspended &&
                                 ((self->head == nullptr) || self->chainsSinceUnmap >= kUnmapEveryChains);
        if (discardTurn && !pending) {
            Unmap *job = self->unmaps;
            self->working = true;
            self->chainsSinceUnmap = 0;
            IOLockUnlock(self->queueLock);
            const bool finished = self->stepUnmap(*job);
            IOLockLock(self->queueLock);
            if (finished) {
                self->unmaps = job->next;
                job->finished = true;
                IOLockWakeup(self->queueLock, &self->unmaps, false);
            }
            self->working = false;
            if (self->suspended)
                IOLockWakeup(self->queueLock, &self->working, false);
            IOLockUnlock(self->queueLock);
            continue;
        }
        Batch *next = nullptr;
        Batch *group = nullptr;
        unsigned tasks = 0;
        bool queue = false;
        if (self->head && !self->suspended && !discardTurn) {
            if (self->unmaps)
                ++self->chainsSinceUnmap;
            next = &batches[turn];
            turn ^= 1;
            next->first = self->pickLocked(&next->sectors, &next->count);
            // r14-sdcq: the picked read and the independent reads queued with it as one command-queue group. r18-sdcq:
            // or, when they are too few for the queue from legacy mode, those chains one after another the legacy way.
            tasks = self->groupLocked(next->first, next->sectors, next->count, &queue);
            if (tasks) {
                group = self->cqGroups[self->cqTurn];
                self->cqTurn ^= 1;
                next = nullptr;
            }
            self->working = true;
        }
        IOLockUnlock(self->queueLock);
        if (group) {
            pending = self->runGroup(group, tasks, pending, queue);
        } else {
            // r14-sdcq: a legacy command needs the card out of CQ mode.
            if (next)
                self->cqLeave();
            // The next chain's command goes to the card first; the previous chain's completions run while it moves
            // data. A chain that is not one ADMA2 command (the bounce path) only has its table built here, so those
            // completions are not held behind its several commands.
            if (next)
                self->beginBatch(*next);
            if (pending) {
                const uint64_t began = switch_now_ns();
                self->completeBatch(*pending);
                if (next && next->issued)
                    next->overlapNs = switch_now_ns() - began;
                pending = nullptr;
            }
            if (!next)
                continue;
            self->endBatch(*next);
            pending = next;
        }
        IOLockLock(self->queueLock);
        self->working = false;
        if (self->suspended)
            IOLockWakeup(self->queueLock, &self->working, false);
        IOLockUnlock(self->queueLock);
#if DEVELOPMENT || DEBUG
        // Throughput line every statsEveryNs while I/O is happening (one log line).
        {
            const uint64_t now = switch_now_ns();
            if (now - self->statsLastNs >= NativeSwitchBlock::statsEveryNs) {
                self->statsLastNs = now;
                self->logThroughput();
            }
        }
#endif
    }
}

void NativeSwitchBlock::logStats()
{
    IOLog("SwitchSD: reads=%llu (%llu KiB) writes=%llu (%llu KiB) errors=%llu max=%lluus last=%u/0x%x irq=%llu "
          "cmds=%llu callbacks=%llums\n",
          stats.reads, stats.readBytes >> 10, stats.writes, stats.writeBytes >> 10, stats.errors,
          stats.maxLatencyNs / 1000, stats.lastError, stats.lastStatus, stats.interrupts, stats.commands,
          stats.completeNs / 1000000);
    logThroughput();
}

// One line to compare boots from a photo. busy is worker time per card command (bus, card and
// bounce copies, not completions), copy the part of it spent copying between the bounce buffer and the
// requests' memory; "of" is wall time since the first request, so the percentage
// is how much of that the SD card was the thing being waited on. MB/s is over busy time: the
// card's effective rate at the request sizes APFS actually issues.
void NativeSwitchBlock::logThroughput()
{
    const uint64_t requests = stats.reads + stats.writes;
    const uint64_t bytes = stats.readBytes + stats.writeBytes;
    const uint64_t wall = stats.firstNs ? switch_now_ns() - stats.firstNs : 0;
    const uint64_t busy = stats.busyNs;
    const uint64_t rate = busy ? bytes * 10000 / busy : 0;  // bytes/ns * 1000 = MB/s, in tenths
    // r6-lows: queue time per IOStoragePriority class since the previous line (requests, mean and max in ms; hi = I/O tier 0,
    // the UI's own page-ins), and the reads taken before older requests (this line / since boot). A string argument: os_log
    // drops a message whose scalar arguments pass 256 bytes.
    char classes[200];
    {
        static const char *const names[kClasses] = {"hi", "def", "lo", "bg"};
        size_t at = 0;
        IOLockLock(queueLock);
        for (unsigned c = 0; c < kClasses; ++c) {
            const ClassWait w = waits[c];
            if (at < sizeof(classes)) {
                at += static_cast<size_t>(snprintf(classes + at, sizeof(classes) - at, " %s %llu/%llu.%llu/%llu",
                                                   names[c], w.n, w.n ? w.sumNs / w.n / 1000000 : 0,
                                                   w.n ? w.sumNs / w.n / 100000 % 10 : 0, w.maxNs / 1000000));
            }
            waits[c] = {};
        }
        if (at < sizeof(classes))
            snprintf(classes + at, sizeof(classes) - at, " passed %llu/%llu", passed, passedTotal);
        passed = 0;
        IOLockUnlock(queueLock);
    }
    // r14-sdcq: read chains with gaps and the KiB read through those gaps (gap chains/KiB).
    // r12-sdio: chains (cmds; the bounce path's extra SDMA pieces not counted; r14-sdcq: c23 of them framed with Auto
    // CMD23), SDMMC1 interrupts taken (irq), and of the
    // ADMA2 commands issued before the previous chain's completions ran, those that had already finished when the worker
    // came back to them (ovl finished/issued; they took no interrupt).
    // r10-adma2: requests by ADMA2 (dma) and through the bounce buffer, the latter by why when any did (SwitchAdmaRefusal:
    // off = ADMA2 off), ADMA2 descriptors per ADMA2 request, ADMA2 writes read back equal (vfy), cache maintenance time
    // (sync), and MB/s per direction over each direction's own busy time. r12-sdio: of the ADMA2 read chains, those with
    // segments staged through the bounce buffer, and those bytes (part chains/KiB).
    char bounces[96] = "";
    {
        static const char *const names[kAdmaRefusals] = {"off", "align", "mem", "high", "full", "gone"};
        size_t at = 0;
        for (unsigned i = 0; i < kAdmaRefusals; ++i) {
            if (stats.bounced[i] && at < sizeof(bounces)) {
                at += static_cast<size_t>(snprintf(bounces + at, sizeof(bounces) - at, "%s%s %llu", at ? " " : " (",
                                                   names[i], stats.bounced[i]));
            }
        }
        if (at && at < sizeof(bounces))
            snprintf(bounces + at, sizeof(bounces) - at, ")");
    }
    uint64_t bounced = 0;
    for (const uint64_t count : stats.bounced)
        bounced += count;
    const uint64_t perRequest = stats.direct ? stats.descriptors * 10 / stats.direct : 0;
    // r14-sdcq: the command queue: tasks/groups, times into CQ mode, the worker's ms in groups (all of it: assignment,
    // tasks, proofs), "proving" while the first tasks are read again; or off (never on, or stopped: then "off" after
    // the counts).
    // r16-sdcq: then, only if the card's cache would not go off, the writes flushed for it / failed, and their ms.
    // r18-sdcq: then, once anything went wrong, "rec" errors recovered / groups read the legacy way after errors / groups
    // skipped by pauses, and "paused" while one lasts.
    char cqState[160];
    if (cq.state == CqEngine::kOff && !cq.tasks) {
        strlcpy(cqState, "off", sizeof(cqState));
    } else {
        const char *phase = "";
        if (cq.state == CqEngine::kOff) {
            phase = " off";
        } else if (cqProofs) {
            phase = " proving";
        } else if (cq.pauseLeft) {
            phase = " paused";
        }
        snprintf(cqState, sizeof(cqState), "%llu/%llu in %llu %llums%s", cq.tasks, cq.groups, cq.enters,
                 stats.cqNs / 1000000, phase);
    }
    if (cq.recoveries || cq.skipped) {
        const size_t at = strlen(cqState);
        snprintf(cqState + at, sizeof(cqState) - at, " rec %llu/%llu/%llu", cq.recoveries, cq.legacyGroups, cq.skipped);
    }
    if (stats.flushes) {
        const size_t at = strlen(cqState);
        snprintf(cqState + at, sizeof(cqState) - at, " wflush %llu/%llu %llums", stats.flushes, stats.flushErrors,
                 stats.flushNs / 1000000);
    }
#if DEVELOPMENT || DEBUG
    // SwitchSdBus.h: commands refused at their PRESENT_STATE read and DAT0 releases awaited by CMD13 (both 0 per the
    // spec).
    if (stats.busRefused || stats.releases) {
        const size_t at = strlen(cqState);
        snprintf(cqState + at, sizeof(cqState) - at, " bus refused %llu dat0 %llu", stats.busRefused, stats.releases);
    }
    if (stats.uncachedChains) {
        const size_t at = strlen(cqState);
        snprintf(cqState + at, sizeof(cqState) - at, " uncached %llu", stats.uncachedChains);
    }
#endif
    // Erase-based trim: erases, MiB freed and the card's busy ms for them (since boot), and AUs still due.
    if (stats.trimErases || trim.pending) {
        const size_t at = strlen(cqState);
        snprintf(cqState + at, sizeof(cqState) - at, " trim %llu/%lluM/%llums due %llu", stats.trimErases,
                 stats.trimSectors >> 11, stats.trimNs / 1000000, trim.pending);
    }
    // r12-sdio: ADMA2 above 4 GiB: on, off, or proofs done/needed.
    char highState[16];
    if (!adma || high.state == SwitchAdmaHigh::kOff) {
        strlcpy(highState, "off", sizeof(highState));
    } else if (high.state == SwitchAdmaHigh::kOn) {
        strlcpy(highState, "on", sizeof(highState));
    } else {
        snprintf(highState, sizeof(highState), "%u/%u", SwitchAdmaHigh::kProofs - high.proofsLeft,
                 SwitchAdmaHigh::kProofs);
    }
    const uint64_t readRate = stats.readNs ? stats.readBytes * 10000 / stats.readNs : 0;
    const uint64_t writeRate = stats.writeNs ? stats.writeBytes * 10000 / stats.writeNs : 0;
    // Periodic: to the log only (os_log), never the boot console.
    char out[680];
    snprintf(
        out, sizeof(out),
        "SwitchSD: %llu rq %llu MiB avg %lluK | busy %llu.%llus copy %llums sync %llums %llu.%llu MB/s "
        "(rd %llu.%llu wr %llu.%llu) %llu%% of %llus | dma %llu bounce %llu%s seg %llu.%llu part %llu/%lluK vfy %llu "
        "wmax %uK 4G+ %s | "
        "<=4K %llu%% seq %llu%% merged %llu gap %llu/%lluK q%u | cmds %llu c23 %llu irq %llu ovl %llu/%llu | cq %s | "
        "err %llu adma %llu retune %llu | "
        "%s %u.%02u MHz | wait ms%s",
        requests, bytes >> 20, requests ? (bytes / requests) >> 10 : 0, busy / 1000000000, busy / 100000000 % 10,
        stats.copyNs / 1000000, stats.syncNs / 1000000, rate / 10, rate % 10, readRate / 10, readRate % 10,
        writeRate / 10, writeRate % 10, wall ? busy * 100 / wall : 0, wall / 1000000000, stats.direct, bounced, bounces,
        perRequest / 10, perRequest % 10, stats.stagedChains, stats.stagedBytes >> 10, stats.verified,
        writeSectors * kSector / 1024, highState, requests ? stats.small * 100 / requests : 0,
        requests ? stats.sequential * 100 / requests : 0, stats.merged, stats.gapChains, stats.gapBytes >> 10,
        stats.maxQueued, stats.commands, stats.cmd23, __atomic_load_n(&stats.interrupts, __ATOMIC_RELAXED), stats.early,
        stats.overlapped, cqState, stats.errors, stats.admaErrors, stats.retunes, busMode, busKHz / 1000,
        busKHz % 1000 / 10, classes);
    os_log(OS_LOG_DEFAULT, "%s", out);
}

void NativeSwitchBlock::reportState(const char *why)
{
    logStats();
    IOLog("SwitchSD: %s present=0x%08x status=0x%08x hc=0x%02x hc2=0x%04x clock=0x%04x submitted=%llu serviced=%llu "
          "queued=%d\n",
          why, r32(PRESENT), r32(STATUS), r8(HOST_CONTROL), r16(HOST_CONTROL2), r16(CLOCK), submitted, serviced,
          static_cast<int>(head != nullptr));
}

// ---- Bus speed: take the inherited card to UHS-I SDR104 ----
//
// A card changes its signalling voltage only during initialization (CMD11 after ACMD41 with
// S18R), so the card goes back to idle with CMD0 and through SD initialization again, in the
// order hekate's bdk uses on this SoC (bdk/storage/sdmmc.c sdmmc_storage_init_sd :1994,
// _sd_storage_get_op_cond :1090, _sd_storage_set_uhs_bus_speed :1523; sdmmc_driver.c). hekate
// powers the card off before chainloading L4T (bootloader/l4t/l4t.c:1028 sd_end ->
// sdmmc1_disable_power), so U-Boot's 3.3 V init was the first since power-on and the card can
// still accept 1.8 V. Nothing here writes the card's data area: CMD6 (switch), ACMD6 (bus
// width) and ACMD42 (DAT3 pull-up) change volatile card state only. Commands sleep on the
// interrupt as the data path's do; the bounded spins left (tuning blocks, clock stable, software
// resets) have no interrupt and run only here, in recovery and on resume.

// One card command; flags is the COMMAND low byte (RSP_*). data > 0 reads one block of that many
// bytes into the bounce buffer by SDMA. 0 or a nonzero code; *response gets RESPONSE[31:0] and
// initStatus the last STATUS sample.
int NativeSwitchBlock::cardCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data)
{
    SwitchSdUse use = SwitchSdUse::kCommand;
    if (data) {
        use = SwitchSdUse::kRead;
    } else if ((flags & 3) == 3) {
        use = SwitchSdUse::kBusy;
    }
    if (!claimBus(use)) {
        initStatus = r32(PRESENT);
        return 1;
    }
    w32(STATUS, 0xffffffff);
    if (data) {
        switch_dcache_clean_invalidate_lines(bounce, data);
        switch_dcache_sync();
        w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);  // SDMA (the data path may have left ADMA2 selected)
        w32(DMA_LOW, static_cast<uint32_t>(bouncePA));
        w32(DMA_HIGH, static_cast<uint32_t>(bouncePA >> 32));
        w16(BLOCK_SIZE, static_cast<uint16_t>(data | 0x7000));
        w16(BLOCK_COUNT, 1);
    }
    w16(TRANSFER, data ? 0x11 : 0);  // DMA | read, one block; nothing for a command without data
    w32(ARGUMENT, arg);
    switch_dcache_sync();
    w16(COMMAND, static_cast<uint16_t>(index << 8 | flags | (data ? CMD_DATA : 0)));
    noteCommand(static_cast<uint8_t>(index));
    // Data and R1b commands end in transfer complete (an R1b's when the card's busy ends), which follows command complete.
    const bool transfers = data != 0 || (flags & 3) == 3;
    const uint32_t done = INT_COMMAND | (transfers ? INT_TRANSFER : 0);
    const uint32_t s = awaitStatus(transfers ? INT_TRANSFER : INT_COMMAND, transfers ? 1000000000ULL : 100000000ULL);
    ended(s);
    initStatus = s;
    if ((s & INT_ERRORS) || (s & done) != done) {
        recover(false);
        return (s & INT_ERRORS) ? 2 : 3;
    }
    if (response)
        *response = r32(RESPONSE);
    w32(STATUS, 0xffffffff);
    if (!bus.idle() && awaitRelease())
        return 4;  // R1b: the card's DAT0 release failed
    if (data) {
        switch_dcache_invalidate_lines(bounce, data);
        switch_dcache_sync();
    }
    return 0;
}

// CMD55 (with the current RCA, 0 before CMD3) then the application command (`data` as cardCommand's).
int NativeSwitchBlock::appCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data)
{
    uint32_t r55 = 0;
    if (const int error = cardCommand(55, rca << 16, RSP_R1, &r55))
        return error;
    if (!(r55 & R1_APP_CMD) || (r55 & R1_ERRORS))
        return 5;
    return cardCommand(index, arg, flags, response, data);
}

// r14-sdcq: the R2 response cardCommand() just received (CMD2's CID, CMD9's CSD), most significant byte first.
void NativeSwitchBlock::readR2(uint8_t out[16])
{
    const uint32_t rep[4] = {r32(RESPONSE), r32(RESPONSE + 4), r32(RESPONSE + 8), r32(RESPONSE + 12)};
    switchSdR2(rep, out);
}

// CMD6, check (set=false) or switch, of `value` in function group `group` (0-based: 0 access
// mode, 3 power limit), the other groups left as they are (0xF). The 64-byte status lands at
// bounce[0..63]: bytes 0-1 maximum current (mA), 6-7 group 4 and 12-13 group 1 support bits,
// byte 15 high nibble group 4 and byte 16 low nibble group 1 result (hekate sdmmc.c:1305/1497).
int NativeSwitchBlock::switchFunction(bool set, unsigned group, uint32_t value)
{
    uint32_t arg = (set ? 0x80000000u : 0u) | 0x00ffffffu;
    uint32_t r = 0;
    arg = (arg & ~(0xfu << (group * 4))) | value << (group * 4);
    const int error = cardCommand(6, arg, RSP_R1, &r, 64);
    if (error)
        return error;
    return (r & R1_ERRORS) ? 6 : 0;
}

// r14-sdcq: CMD48, `length` bytes of the extension register space at `address` (page * 512 + offset, one page) of
// function `fno`, into `out`. The data block is always 512 bytes, the register's bytes first (5.7.2.1). Setup only.
bool NativeSwitchBlock::extRead(uint8_t fno, uint32_t address, uint32_t length, uint8_t *out)
{
    uint32_t r = 0;
    if (!switchSdExtRange(address, length))
        return false;
    if (cardCommand(48, switchSdExtArgument(fno, address, length), RSP_R1, &r, kSector) || (r & R1_ERRORS))
        return false;
    memcpy(out, bounce, length);
    return true;
}

// r14-sdcq: who the card is and what it can do, read once at boot in transfer state and logged as two lines (IOLog,
// one-shot): the identity and the verdict first (is it A2, does it offer the command queue), then the raw registers and
// the extension functions. SCR (ACMD51), SD Status (ACMD13) and, when the SCR lists CMD48/49, the General Information
// and the Power Management and Performance Enhancement register sets (CMD48). Read-only: nothing here writes the card.
// A command that fails only leaves its part out. Needs the RCA, so the re-initialized bus (raiseBusSpeed).
void NativeSwitchBlock::identify()
{
    if (!rca) {
        IOLog("SwitchSD: card identification skipped (inherited bus, RCA unknown)\n");
        return;
    }
    uint32_t r = 0;
    if (!appCommand(51, 0, RSP_R1, &r, sizeof(scrBytes)) && !(r & R1_ERRORS)) {
        memcpy(scrBytes, bounce, sizeof(scrBytes));
        scr = switchSdParseScr(scrBytes);
        haveScr = true;
    }
    if (!appCommand(13, 0, RSP_R1, &r, sizeof(ssrBytes)) && !(r & R1_ERRORS)) {
        memcpy(ssrBytes, bounce, sizeof(ssrBytes));
        sdStatus = switchSdParseStatus(ssrBytes);
        haveSsr = true;
    }
    static uint8_t regs[1024];  // setup() runs once; too big for its stack
    if (!haveScr) {
        extWhy = "no SCR";
    } else if (!(scr.cmdSupport & kSdCmd48)) {
        extWhy = "SCR lists no CMD48/49";
    } else if (!extRead(0, 0, kSector, regs)) {
        extWhy = "general information read failed";
    } else {
        extInfo = switchSdParseGeneralInfo(regs, kSector);
        if (extInfo.why && extInfo.length > kSector && extInfo.length <= sizeof(regs) &&
            extRead(0, kSector, kSector, regs + kSector))
            extInfo = switchSdParseGeneralInfo(regs, sizeof(regs));
        extWhy = extInfo.why;
        if (!extWhy && extInfo.power.found && switchSdExtRange(extInfo.power.address, 2) &&
            extRead(extInfo.power.fno, extInfo.power.address, 2, regs)) {
            power = switchSdParsePower(regs);
            havePower = true;
        }
        // The performance register set is used only when its first 263 bytes (up to the CQ control byte) sit in one page.
        if (!extWhy && extInfo.perf.found) {
            if (!switchSdCqControlAddress(extInfo.perf)) {
                extWhy = "performance register set crosses a page";
            } else if (!extRead(extInfo.perf.fno, extInfo.perf.address, kSdPerfCqControl + 1, regs)) {
                extWhy = "performance register read failed";
            } else {
                perf = switchSdParsePerf(regs);
                havePerf = true;
            }
        }
    }
    const SwitchSdCid id = switchSdParseCid(cid);
    const SwitchSdCsd size = switchSdParseCsd(csd);
    const char *maker = switchSdMaker(id.mid);
    char makerHex[8];
    snprintf(makerHex, sizeof(makerHex), "0x%02x", id.mid);
    static const char *const kinds[] = {"SDSC", "SDHC/SDXC", "SDUC", "?"};
    uint8_t queue = 0;
    if (havePerf) {
        queue = perf.cqDepth;
    } else if (haveSsr) {
        queue = sdStatus.cqDepth;
    }
    const bool a2 = haveSsr && sdStatus.appClass >= 2;
    const char *verdict = "SD Status unread: A2 and command queue unknown";
    if (a2 && queue) {
        verdict = "A2 with a command queue: queued reads can overlap on the card";
    } else if (queue) {
        verdict = "not A2, but it offers a command queue";
    } else if (a2) {
        verdict = "A2 by its SD Status, but no command queue in its registers";
    } else if (haveSsr) {
        verdict = "not A2 and no command queue: reads stay one command at a time";
    }
    const char *cmd23Support = "?";
    const char *cmd48Support = "?";
    if (haveScr) {
        cmd23Support = (scr.cmdSupport & kSdCmd23) ? "yes" : "no";
        cmd48Support = (scr.cmdSupport & kSdCmd48) ? "yes" : "no";
    }
    const char *cardCache = "?";
    if (havePerf) {
        cardCache = perf.cache ? "yes" : "no";
    } else if (haveSsr) {
        cardCache = sdStatus.cache ? "yes" : "no";
    }
    char classes[48] = "SD Status unread";
    if (haveSsr) {
        snprintf(classes, sizeof(classes), "class %u U%u V%u A%u", static_cast<unsigned>(sdStatus.speedClass),
                 static_cast<unsigned>(sdStatus.uhsGrade), static_cast<unsigned>(sdStatus.videoClass),
                 static_cast<unsigned>(sdStatus.appClass));
    }
    char line[512];
    snprintf(line, sizeof(line),
             "SwitchSD: card %s%s%s%s \"%s\" OEM \"%s\" rev %u.%u #%08x %04u-%02u, %llu GiB %s | SD %u.x, CMD23 %s, "
             "CMD48/49 %s | %s | queue depth %u, cache %s -> %s",
             maker ? maker : makerHex, maker ? " (" : "", maker ? makerHex : "", maker ? ")" : "",
             haveCid ? id.pnm : "?", haveCid ? id.oid : "?", static_cast<unsigned>(id.prv >> 4),
             static_cast<unsigned>(id.prv & 0xf), id.psn, static_cast<unsigned>(id.year),
             static_cast<unsigned>(id.month), size.bytes >> 30, haveCsd ? kinds[size.structure & 3] : "(no CSD)",
             haveScr ? static_cast<unsigned>(scr.major) : 0u, cmd23Support, cmd48Support, classes,
             static_cast<unsigned>(queue), cardCache, verdict);
    IOLog("%s\n", line);
#if DEVELOPMENT || DEBUG
    // The raw registers (the CID's serial included) and the extension functions as read.
    char hex[4][132];
    const uint8_t *const raw[4] = {cid, csd, scrBytes, ssrBytes};
    static const unsigned lengths[4] = {16, 16, 8, 25};  // SD Status: bits 511..312, every field defined
    for (unsigned k = 0; k < 4; ++k) {
        hex[k][0] = 0;
        for (unsigned i = 0; i < lengths[k]; ++i) {
            snprintf(hex[k] + 2 * static_cast<size_t>(i), sizeof(hex[k]) - 2 * static_cast<size_t>(i), "%02x",
                     raw[k][i]);
        }
    }
    char ext[256];
    if (extWhy || !extInfo.extensions) {
        snprintf(ext, sizeof(ext), "ext registers: %s", extWhy ? extWhy : "no functions");
    } else {
        snprintf(
            ext, sizeof(ext),
            "ext rev %u len %u, %u functions; power %sfno %u @0x%05x (rev %u, off-notify %u, sustain %u, down %u); "
            "perf %sfno %u @0x%05x (rev %u, FX %u, maint card %u host %u, cache %u on %u, queue %u control 0x%02x, "
            "task errors 0x%02x)",
            static_cast<unsigned>(extInfo.revision), static_cast<unsigned>(extInfo.length),
            static_cast<unsigned>(extInfo.extensions), havePower ? "" : "unread ",
            static_cast<unsigned>(extInfo.power.fno), extInfo.power.address, static_cast<unsigned>(power.revision),
            static_cast<unsigned>(power.offNotification), static_cast<unsigned>(power.sustenance),
            static_cast<unsigned>(power.powerDown), havePerf ? "" : "unread ", static_cast<unsigned>(extInfo.perf.fno),
            extInfo.perf.address, static_cast<unsigned>(perf.revision), static_cast<unsigned>(perf.fxEvent),
            static_cast<unsigned>(perf.cardMaintenance), static_cast<unsigned>(perf.hostMaintenance),
            static_cast<unsigned>(perf.cache), static_cast<unsigned>(perf.cacheEnable & 1),
            static_cast<unsigned>(perf.cqDepth), static_cast<unsigned>(perf.cqControl),
            static_cast<unsigned>(perf.taskErrors));
    }
    IOLog("SwitchSD: card registers CID %s CSD %s SCR %s SSR %s | %s\n", haveCid ? hex[0] : "-", haveCsd ? hex[1] : "-",
          haveScr ? hex[2] : "-", haveSsr ? hex[3] : "-", ext);
#endif
}

// hekate sdmmc_setup_clock (sdmmc_driver.c:307): card clock gated, default tap (TAP_HW_UPDATED
// off, :120/:133), CMD/DAT reset, timing bits (uhs < 0: 3.3 V legacy timing, HISPD as asked;
// else that UHS mode with 1.8 V signalling), CAR source and its U7.1 divider, always as a pair (source
// < 0: both unchanged; enable cleared around the change, the SET/CLR registers per hekate
// clock.c:72-79), the SDCLK divider (base / 2n, 0 passes base through), then the card clock back on
// if it was on. Only kSpeeds names CAR settings: PLLP with divider 0 would be 408 MHz.
void NativeSwitchBlock::setTiming(int uhs, bool highSpeed, int source, uint32_t carDivider, uint32_t divider)
{
    const uint16_t clock = r16(CLOCK);
    w16(CLOCK, clock & ~4u);
    w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) & ~(1u << 17));
    w32(VENDOR_CLOCK, (r32(VENDOR_CLOCK) & ~0xff0000u) | kTapB01 << 16);
    resetLines();
    w8(HOST_CONTROL, static_cast<uint8_t>(highSpeed ? r8(HOST_CONTROL) | 4 : r8(HOST_CONTROL) & ~4u));
    if (uhs < 0) {
        w16(HOST_CONTROL2, r16(HOST_CONTROL2) & ~(7 | CONTROL2_1V8));
    } else {
        w16(HOST_CONTROL2,
            static_cast<uint16_t>((r16(HOST_CONTROL2) & ~7u) | static_cast<uint32_t>(uhs) | CONTROL2_1V8));
    }
    (void)r16(CLOCK);
    if (source >= 0) {
        switch_mmio_write32(car, CAR_CLK_ENB_L_CLR, CAR_SDMMC1);
        switch_mmio_write32(car, CAR_CLK_SOURCE_SDMMC1, static_cast<uint32_t>(source) << 29 | (carDivider & 0xff));
        switch_mmio_write32(car, CAR_CLK_ENB_L_SET, CAR_SDMMC1);
        (void)switch_mmio_read32(car, CAR_RST_DEVICES_L);
    }
    const uint16_t next = static_cast<uint16_t>((clock & ~0xffc4u) | (divider & 0xff) << 8 | (divider >> 8 & 3) << 6);
    w16(CLOCK, next);
    for (const uint64_t start = switch_now_ns(); !(r16(CLOCK) & 2) && switch_now_ns() - start < 10000000;) {
    }
    if (clock & 4)
        w16(CLOCK, next | 4);
}

// hekate _sdmmc_autocal_config_offset + _sdmmc_autocal_execute (sdmmc_driver.c:845/178) for SDMMC1
// at 1.8 V on T210B01: pull-up/down offsets 6/6 (the L4T prod value for every UHS mode,
// tegra210b01-nx-prods.dtsi), pad comparator powered for the run, and hekate's fixed B01 drive
// codes (PU 8, PD 8, :151-153) if calibration does not finish in 10 ms. B01 needs no periodic
// re-calibration (hekate sets periodic_calibration only on T210, :1362-1365).
bool NativeSwitchBlock::calibratePads()
{
    const uint16_t clock = r16(CLOCK);
    w16(CLOCK, clock & ~4u);
    w32(AUTOCAL_CONFIG, (r32(AUTOCAL_CONFIG) & 0xffff8080u) | 6u << 8 | 6u);
    if (!(r32(PAD_CONTROL) & 0x80000000u)) {
        w32(PAD_CONTROL, r32(PAD_CONTROL) | 0x80000000u);
        (void)r16(CLOCK);
        IODelay(1);
    }
    w32(AUTOCAL_CONFIG, r32(AUTOCAL_CONFIG) | 1u << 29 | 1u << 31);
    (void)r16(CLOCK);
    IODelay(2);
    bool done = false;
    for (const uint64_t start = switch_now_ns(); !done && switch_now_ns() - start < 10000000;)
        done = ((r32(AUTOCAL_STATUS) & 0x80000000u) == 0u);
    if (!done) {
        w32(AUTOCAL_CONFIG, r32(AUTOCAL_CONFIG) & ~(1u << 29));
        switch_mmio_write32(apb, APB_SDMMC1_PAD_CONFIG,
                            (switch_mmio_read32(apb, APB_SDMMC1_PAD_CONFIG) & 0xf8080fffu) | 0x808u << 12);
        (void)switch_mmio_read32(apb, APB_SDMMC1_PAD_CONFIG);
    }
    w32(PAD_CONTROL, r32(PAD_CONTROL) & ~0x80000000u);
    if (clock & 4)
        w16(CLOCK, r16(CLOCK) | 4);
    return done;
}

// After CMD11: hekate sdmmc_setup_clock(SDR12) + sdmmc_enable_low_voltage (sdmmc_driver.c:1511):
// LDO2 to 1.8 V and its ramp, PMC told the SDMMC1 pads run at 1.8 V, Schmitt triggers on the six
// pads, pad calibration, 1.8 V in the SDHCI power register, 5 ms, then SDCLK and DAT[3:0] must
// read high within 1 ms. Unlike hekate, SDCLK stays stopped from the CMD11 response until after
// the 5 ms, as SD spec 4.2.4.2 and Linux (mmc_set_uhs_voltage, on this same controller) do.
bool NativeSwitchBlock::switchTo1V8(uint8_t ldo2)
{
    w16(CLOCK, r16(CLOCK) & ~4u);
    setTiming(UHS_SDR12, false, -1, 0, 1);  // 48 MHz CAR / 2: 24 MHz SDR12 once the clock runs
    const uint8_t value = static_cast<uint8_t>((ldo2 & ~static_cast<uint32_t>(kLdoVoltage)) | kLdo1V8);
    int error = TEGRA_I2C_EIO;
    // hekate _max7762x_set_reg retries a PMIC write 100 times 50 us apart (max7762x.c).
    for (unsigned attempt = 0; attempt < 100 && error; ++attempt) {
        error = tegra_i2c_write(kPmicBus, kPmicAddress, kPmicLdo2, &value, 1);
        if (error)
            IODelay(50);
    }
    uint8_t check = 0;
    if (error || tegra_i2c_read(kPmicBus, kPmicAddress, kPmicLdo2, &check, 1) || (check & kLdoVoltage) != kLdo1V8) {
        panic("SwitchSD: LDO2 1.8 V write failed (i2c %d, LDO2_CFG 0x%02x) with the card waiting in CMD11; "
              "power off, then boot with switch-no-sd-uhs",
              error, check);
    }
    IODelay(1000 + 150);  // regulator ramp (hekate set_voltage) + settle (enable_low_voltage)
    switch_mmio_write32(pmc, PMC_PWR_DET_VAL, switch_mmio_read32(pmc, PMC_PWR_DET_VAL) & ~PMC_SDMMC1_3V3);
    (void)switch_mmio_read32(pmc, PMC_PWR_DET_VAL);
    for (uint32_t pad = 0; pad < 6; ++pad) {
        switch_mmio_write32(apb, PINMUX_SDMMC1 + pad * 4,
                            switch_mmio_read32(apb, PINMUX_SDMMC1 + pad * 4) | PINMUX_SCHMITT);
    }
    if (!calibratePads())
        IOLog("SwitchSD: SDMMC1 pad calibration timed out; fixed B01 drive codes used\n");
    w8(POWER, 0x0a);  // hekate _sdmmc_set_io_power(1.8 V): voltage select, then bus power on
    w8(POWER, 0x0b);
    (void)r16(CLOCK);
    IOSleep(5);
    w16(CLOCK, r16(CLOCK) | 4);
    (void)r16(CLOCK);
    IODelay(1000);
    return (r32(PRESENT) & PRESENT_DAT) == PRESENT_DAT;
}

// hekate _clock_enable_pllc4 (clock.c:586). PLLC4 only feeds SDMMC on this platform; one already
// running with other dividers belongs to someone else and is left alone (no SDR104 then).
bool NativeSwitchBlock::enablePllc4()
{
    const uint32_t base = switch_mmio_read32(car, CAR_PLLC4_BASE);
    if (base & PLL_ENABLE)
        return (base & PLLC4_DIVIDERS) == PLLC4_CONFIG && ((base & PLL_LOCK) != 0u);
    switch_mmio_write32(car, CAR_PLLC4_MISC, PLLC4_LOCK_DETECT);
    switch_mmio_write32(car, CAR_PLLC4_BASE, switch_mmio_read32(car, CAR_PLLC4_BASE) & ~PLLC4_IDDQ);
    IODelay(10);
    switch_mmio_write32(car, CAR_PLLC4_BASE, PLLC4_CONFIG);
    switch_mmio_write32(car, CAR_PLLC4_BASE, PLLC4_CONFIG | PLL_ENABLE);
    (void)switch_mmio_wait32(car, CAR_PLLC4_BASE, PLL_LOCK, PLL_LOCK, 1000);
    IODelay(2);
    if (switch_mmio_read32(car, CAR_PLLC4_BASE) & PLL_LOCK)
        return true;
    switch_mmio_write32(car, CAR_PLLC4_BASE,
                        (switch_mmio_read32(car, CAR_PLLC4_BASE) & ~PLL_ENABLE) | PLLC4_IDDQ);  // back as it was
    return false;
}

// hekate _sdmmc_tuning_execute_once (sdmmc_driver.c:604) verbatim, including the CMD/DAT reset
// right after issuing CMD19 with the card clock gated: one 64-byte tuning block (4-bit bus), the
// controller compares it itself and moves to the next tap; buffer-read-ready says the step ended.
void NativeSwitchBlock::tuneStep()
{
    if (!awaitTuningIdle(2000000000)) {
        resetLines();
        return;
    }
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);  // as hekate tunes: SDMA selected, never the data path's ADMA2
    w16(BLOCK_SIZE, 64);
    w16(BLOCK_COUNT, 1);
    w16(TRANSFER, 0x10);  // read, no DMA
    w32(STATUS_ENABLE, r32(STATUS_ENABLE) | INT_READ_READY);
    w16(STATUS, r16(STATUS));
    w16(CLOCK, r16(CLOCK) & ~4u);
    w32(ARGUMENT, 0);
    w16(COMMAND, 19u << 8 | RSP_R1 | CMD_DATA);
    (void)r16(CLOCK);
    IODelay(1);
    resetLines();
    w16(CLOCK, r16(CLOCK) | 4);
    (void)r16(CLOCK);
    if (awaitStatus(INT_READ_READY, 5000000) & INT_READ_READY) {
        w16(STATUS, INT_READ_READY);
        w32(STATUS_ENABLE, r32(STATUS_ENABLE) & ~INT_READ_READY);
        (void)r16(CLOCK);
        IODelay(1);  // 8 card clocks
        return;
    }
    resetLines();
    w32(STATUS_ENABLE, r32(STATUS_ENABLE) & ~INT_READ_READY);
    (void)r16(CLOCK);
    IODelay(1);
}

// hekate sdmmc_tuning_execute (sdmmc_driver.c:760) for SDR104: step size 1, 128 tries, 1x
// multiplier, hardware tap update; the controller walks the taps one CMD19 at a time, clears
// EXEC_TUNING when it is done and sets TUNED_CLK when it found a window (stricter than hekate,
// which looks at TUNED_CLK only: a controller still in tuning mode is a failure too).
bool NativeSwitchBlock::tune()
{
    w32(VENDOR_TUNING1, 0);
    w32(VENDOR_TUNING0, (r32(VENDOR_TUNING0) & 0xffff1fffu) | 2u << 13);
    w32(VENDOR_TUNING0, (r32(VENDOR_TUNING0) & 0xffffe03fu) | 1u << 6);
    w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) | 1u << 17);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | CONTROL2_EXEC_TUNING);
    for (unsigned i = 0; i < 128 && (r16(HOST_CONTROL2) & CONTROL2_EXEC_TUNING); ++i)
        tuneStep();
    w32(STATUS, 0xffffffff);
    // The last tuning block raised no transfer complete either: the next command claims the bus by one read.
    if ((r16(HOST_CONTROL2) & (CONTROL2_EXEC_TUNING | CONTROL2_TUNED)) == CONTROL2_TUNED && awaitTuningIdle(2000000000))
        return true;
    // No window: leave tuning mode and fixed sampling, as Linux sdhci_reset_tuning does.
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) & ~(CONTROL2_EXEC_TUNING | CONTROL2_TUNED));
    resetLines();
    return false;
}

// Runs the card at kSpeeds[from], or the first slower step whose tuning finds a window; false
// when none does. The card stays in SDR104 throughout; only the clock and sampling point move.
bool NativeSwitchBlock::clockAndTune(unsigned from)
{
    for (unsigned i = from; i < kSpeedCount; ++i) {
        setTiming(UHS_SDR104, false, static_cast<int>(kSpeeds[i].source), kSpeeds[i].carDivider, kSpeeds[i].divider);
        if (tune()) {
            speed = i;
            tuned = true;
            busMode = "SDR104";
            busKHz = kSpeeds[i].kHz;
            return true;
        }
        IOLog("SwitchSD: no SDR104 sampling window at %u kHz\n", kSpeeds[i].kHz);
    }
    return false;
}

// After a failed SDR104 transfer (the card is back in transfer state after recover()): tune again
// at this clock, or with `slower` (the retry after a re-tune failed too) go one clock step down;
// either way, steps down while tuning finds no window. Tuning moves no data through the bounce
// buffer (CMD19 blocks stay in the controller), so a write waiting there is untouched. Panics only
// when not even 49.92 MHz tunes: then no transfer can succeed and every I/O would fail anyway.
void NativeSwitchBlock::retune(bool slower)
{
    stats.retunes++;
    if (slower ? !clockAndTune(speed + 1) : !tune() && !clockAndTune(speed + 1)) {
        panic("SwitchSD: SDR104 tuning finds no sampling window down to %u kHz (hc2 0x%04x, %llu errors); "
              "boot with switch-no-sd-uhs",
              kSpeeds[kSpeedCount - 1].kHz, r16(HOST_CONTROL2), stats.errors);
    }
    IOLog("SwitchSD: SDR104 re-tuned at %u kHz, tap %u\n", busKHz, (r32(VENDOR_CLOCK) >> 16) & 0xff);
}

// Returns true when the card was re-initialized (re-verify reads), false when the inherited mode
// was kept without touching the card. Every precondition is checked before CMD0; after CMD0 the
// inherited state is gone and no I/O is possible without finishing, so any failure panics naming
// the step (power off clears the card; with switch-no-sd-uhs none of this runs).
bool NativeSwitchBlock::raiseBusSpeed()
{
    if (!car || !apb || !pmc) {
        IOLog("SwitchSD: SDR104 skipped (CAR/APB/PMC not mapped); inherited bus kept\n");
        return false;
    }
    const uint32_t source = switch_mmio_read32(car, CAR_CLK_SOURCE_SDMMC1);
    const uint32_t clock = r16(CLOCK);
    const uint32_t divider = (clock >> 8 & 0xff) | (clock >> 6 & 3) << 8;
    const uint32_t baseKHz = source >> 29 == SOURCE_PLLP ? PLLP_KHZ * 2 / ((source & 0xff) + 2) : 0;
    busKHz = divider ? baseKHz / (2 * divider) : baseKHz;
    busMode = "inherited";
    // With switch-no-sd-uhs nothing below runs (no CMD0, no PLLC4, no PMIC access).
    unsigned off = 0;
    const char *skip = nullptr;
    uint8_t ldo2 = 0;
    if (PE_parse_boot_argn("switch-no-sd-uhs", &off, sizeof(off)) && off) {
        skip = "boot-arg switch-no-sd-uhs";
    } else if (((switch_mmio_read32(apb, APB_GP_HIDREV) >> 4) & 0xf) < 2) {
        skip = "not T210B01 (the pad/tap/trim values are B01's)";
    } else if (!(r32(CAPABILITIES_HI) & 2)) {
        skip = "controller reports no SDR104";
    } else if (!baseKHz || baseKHz > 50000 || baseKHz < 25000) {
        skip = "SDMMC1 CAR clock not PLLP 25-50 MHz";
    } else if (r16(HOST_CONTROL2) & CONTROL2_1V8) {
        skip = "bus already at 1.8 V";
    } else if (tegra_i2c_init(kPmicBus) || tegra_i2c_read(kPmicBus, kPmicAddress, kPmicLdo2, &ldo2, 1)) {
        skip = "PMIC (I2C5 0x3c) not reachable";
    } else if ((ldo2 & kLdoVoltage) != kLdo3V3) {
        skip = "LDO2 not at 3.3 V";
    } else if (!enablePllc4()) {
        skip = "PLLC4 busy with other dividers or no lock";
    }
    if (skip) {
        IOLog("SwitchSD: SDR104 skipped (%s; LDO2_CFG 0x%02x PLLC4 0x%08x); bus stays at %u kHz\n", skip, ldo2,
              switch_mmio_read32(car, CAR_PLLC4_BASE), busKHz);
        return false;
    }
    return reinitialize(baseKHz, ldo2, true, false);
}

// SD initialization from the idle state into the fastest mode the card and host share: at boot from U-Boot's card
// (raiseBusSpeed), and after system sleep from a freshly powered card (resume, `uhs` = it ran at 1.8 V before).
bool NativeSwitchBlock::reinitialize(uint32_t baseKHz, uint8_t ldo2, bool uhs, bool resume)
{
    const uint64_t start = switch_now_ns();
    const auto fail = [&](const char *step, int error) {
        if (resume) {
            panic("SwitchSD: SD re-init after sleep failed at %s (error %d, STATUS 0x%08x PRESENT 0x%08x hc2 0x%04x)",
                  step, error, initStatus, r32(PRESENT), r16(HOST_CONTROL2));
        }
        panic("SwitchSD: SD re-init failed at %s (error %d, STATUS 0x%08x PRESENT 0x%08x hc2 0x%04x); "
              "power off, then boot with switch-no-sd-uhs",
              step, error, initStatus, r32(PRESENT), r16(HOST_CONTROL2));
    };
    rca = 0;
    tuned = false;
    speed = 0;

    // Host defaults hekate's sdmmc_init sets and U-Boot does not (sdmmc_driver.c:1390-1391, 1257):
    // command output enable one cycle late, the band-gap regulator supplying the DLL, and the
    // SDMMC1 CLK pad's deep loopback that the read sampling path relies on.
    w32(IO_SPARE, r32(IO_SPARE) | 1u << 19);
    w32(VENDOR_IO_TRIM, r32(VENDOR_IO_TRIM) & ~4u);
    switch_mmio_write32(apb, APB_SDMMC1_CLK_LOOPBACK, 1);
    // Identification: 1-bit, legacy timing, <= 400 kHz, 1 ms + 74 clocks before the first command.
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~0x26u);
    setTiming(-1, false, -1, 0, (baseKHz + 799) / 800);
    IODelay(1000 + 200);
    uint32_t r = 0;
    int error = 0;
    error = cardCommand(0, 0, RSP_NONE, nullptr);
    if (error)
        fail("CMD0", error);
    error = cardCommand(8, 0x1aa, RSP_R1, &r);
    if (error || (r & 0xfff) != 0x1aa)
        fail("CMD8", error ? error : static_cast<int>(r & 0xfff));
    // ACMD41 until the card leaves busy: block addressing, max performance, ask for 1.8 V.
    for (const uint64_t ready = switch_now_ns();;) {
        error = appCommand(41, OCR_CCS | OCR_XPC | (uhs ? OCR_S18 : 0) | OCR_VDD_32_33, RSP_R3, &r);
        if (error)
            fail("ACMD41", error);
        if (r & OCR_READY)
            break;
        if (switch_now_ns() - ready > 1500000000ULL)
            fail("ACMD41 busy", static_cast<int>(r >> 24));
        IOSleep(10);
    }
    if (!(r & OCR_CCS))
        fail("ACMD41 byte-addressed card", static_cast<int>(r >> 24));
    const bool lowVoltage = uhs && ((r & OCR_S18) != 0u);
    if (lowVoltage) {
        uint32_t r11 = 0;
        error = cardCommand(11, 0, RSP_R1, &r11);
        if (error || (r11 & R1_ERRORS))
            fail("CMD11", error ? error : 6);
        if (!switchTo1V8(ldo2))
            fail("1.8 V switch (DAT[3:0] not released)", static_cast<int>((r32(PRESENT) >> 20) & 0xf));
    }
    error = cardCommand(2, 0, RSP_R2, nullptr);
    if (error)
        fail("CMD2", error);
    if (!resume) {
        readR2(cid);
        haveCid = true;
    }
    for (const uint64_t ready = switch_now_ns(); !rca;) {
        error = cardCommand(3, 0, RSP_R1, &r);
        if (error)
            fail("CMD3", error);
        rca = r >> 16;
        if (rca)
            break;
        if (switch_now_ns() - ready > 1500000000ULL)
            fail("CMD3 RCA 0", 0);
        IOSleep(1);  // hekate _sd_storage_get_rca: ask again 1 ms later
    }
    // r14-sdcq: the CSD while the card is in stand-by (CMD9 is a class 0 command of that state only). Logged by identify();
    // a failure only leaves it out.
    if (!resume && !cardCommand(9, rca << 16, RSP_R2, nullptr)) {
        readR2(csd);
        haveCsd = true;
    }
    // Out of identification: default speed (24 MHz) at 3.3 V as hekate (sdmmc.c:2054); the 1.8 V path
    // already runs SDR12 at that clock since switchTo1V8.
    if (!lowVoltage)
        setTiming(-1, false, -1, 0, 1);
    error = cardCommand(7, rca << 16, RSP_R1B, &r);
    if (error || (r & R1_ERRORS))
        fail("CMD7", error ? error : 6);
    error = appCommand(42, 0, RSP_R1, &r);
    if (error || (r & R1_ERRORS))
        fail("ACMD42", error ? error : 6);
    error = appCommand(6, 2, RSP_R1, &r);
    if (error || (r & R1_ERRORS))
        fail("ACMD6", error ? error : 6);
    w8(HOST_CONTROL, r8(HOST_CONTROL) | 2);
    error = switchFunction(false, 0, 0xf);
    if (error)
        fail("CMD6 query", error);
    const uint32_t access = bounce[13] | static_cast<uint32_t>(bounce[12]) << 8;
    const uint32_t power = bounce[7] | static_cast<uint32_t>(bounce[6]) << 8;
    const char *limit = "";
    if (lowVoltage && (access & 1u << UHS_SDR104)) {
        // hekate: 1.44 W power limit when offered (its result is not checked there either: the card
        // keeps 0.72 W), then check and switch to SDR104 if the card draws at most 800 mA there (the
        // board's max-current-800ma), else SDR25.
        if (switchFunction(true, 3, power & 2 ? 1 : 0)) {
            limit = ", power limit unchanged";
        } else {
            limit = ((static_cast<uint32_t>(bounce[15]) >> 4) & 0xf) == 1 ? ", 1.44 W" : ", 0.72 W";
        }
        error = switchFunction(false, 0, UHS_SDR104);
        if (error)
            fail("CMD6 check SDR104", error);
        if ((bounce[16] & 0xf) == UHS_SDR104 && (static_cast<uint32_t>(bounce[0]) << 8 | bounce[1]) <= 800) {
            error = switchFunction(true, 0, UHS_SDR104);
            if (error || (bounce[16] & 0xf) != UHS_SDR104)
                fail("CMD6 SDR104", error);
            // 199.68 MHz, or the first slower clock that tunes (the card stays in SDR104).
            if (!clockAndTune(0))
                fail("SDR104 tuning (no window down to 49.92 MHz)", r16(HOST_CONTROL2));
        }
    }
    if (!tuned) {
        // No SDR104 (card, or its current at SDR104 over budget): SDR25 at 1.8 V or high speed at
        // 3.3 V, both the inherited 48 MHz (group 1 function 1 is the same CMD6 for either); a card
        // without even that stays at default speed, 24 MHz.
        const bool fast = (access & 2) != 0u;
        if (fast) {
            error = switchFunction(true, 0, 1);
            if (error || (bounce[16] & 0xf) != 1)
                fail("CMD6 high speed", error);
        }
        int mode = -1;
        if (lowVoltage)
            mode = fast ? static_cast<int>(UHS_SDR25) : static_cast<int>(UHS_SDR12);
        setTiming(mode, fast && !lowVoltage, -1, 0, fast ? 0 : 1);
        if (fast) {
            busMode = lowVoltage ? "SDR25" : "HS";
        } else {
            busMode = lowVoltage ? "SDR12" : "DS";
        }
        busKHz = fast ? baseKHz : baseKHz / 2;
    }
    error = cardCommand(13, rca << 16, RSP_R1, &r);
    if (error || (r & R1_ERRORS) || ((r >> 9) & 0xf) != 4)
        fail("CMD13 transfer state", error ? error : static_cast<int>(r >> 9 & 0xf));
    lowVoltageBus = lowVoltage;
    if (!resume) {
        IOLog("SwitchSD: bus now %s %u kHz %s V 4-bit%s, tap %u%s, RCA 0x%04x, %llu ms\n", busMode, busKHz,
              lowVoltage ? "1.8" : "3.3", limit, (r32(VENDOR_CLOCK) >> 16) & 0xff, tuned ? " (tuned)" : "", rca,
              (switch_now_ns() - start) / 1000000);
    }
    return true;
}

// Card and SDMMC1 pads unpowered, as hekate sdmmc1_disable_power (the card must restart from power-off after SC7: the
// controller is reset and the card cannot leave 1.8 V signalling otherwise). Thread context, I2C up.
void NativeSwitchBlock::powerOffCard()
{
    w16(CLOCK, r16(CLOCK) & ~4u);
    w8(POWER, 0);
    uint8_t ldo2 = 0;
    int error = tegra_i2c_read(kPmicBus, kPmicAddress, kPmicLdo2, &ldo2, 1);
    if (!error) {
        const uint8_t off = static_cast<uint8_t>(ldo2 & kLdoVoltage);  // power mode 7:6 = 0: off
        error = tegra_i2c_write(kPmicBus, kPmicAddress, kPmicLdo2, &off, 1);
    }
    if (error)
        panic("SwitchSD: LDO2 off before sleep failed (I2C5 %d)", error);
    IOSleep(4);
    if (tegra_gpio_output(kCardPowerGpio, 0))
        panic("SwitchSD: cannot switch the card supply (GPIO PE4) off");
    IOSleep(10);
    switch_mmio_write32(pmc, PMC_NO_IOPOWER, switch_mmio_read32(pmc, PMC_NO_IOPOWER) | PMC_SDMMC1_NO_POWER);
    switch_mmio_write32(pmc, PMC_PWR_DET_VAL, switch_mmio_read32(pmc, PMC_PWR_DET_VAL) | PMC_SDMMC1_3V3);
    (void)switch_mmio_read32(pmc, PMC_PWR_DET_VAL);
}

// After SC7: SDMMC1 clocked from PLLP at U-Boot's 48 MHz and out of reset, pads and card powered at 3.3 V (hekate
// _sdmmc_config_sdmmc1), the host reset and set up as hekate sdmmc_init and setup() leave it, identification clock on.
void NativeSwitchBlock::powerOnController()
{
    switch_mmio_write32(car, CAR_RST_DEV_L_SET, CAR_SDMMC1);
    switch_mmio_write32(car, CAR_CLK_ENB_L_CLR, CAR_SDMMC1);
    switch_mmio_write32(car, CAR_CLK_SOURCE_SDMMC1, SOURCE_PLLP << 29 | kPllpDivider48MHz);
    switch_mmio_write32(car, CAR_CLK_ENB_L_SET, CAR_SDMMC1);
    (void)switch_mmio_read32(car, CAR_RST_DEVICES_L);
    IODelay(2);
    switch_mmio_write32(car, CAR_RST_DEV_L_CLR, CAR_SDMMC1);
    (void)switch_mmio_read32(car, CAR_RST_DEVICES_L);
    switch_mmio_write32(pmc, PMC_NO_IOPOWER, switch_mmio_read32(pmc, PMC_NO_IOPOWER) & ~PMC_SDMMC1_NO_POWER);
    (void)switch_mmio_read32(pmc, PMC_NO_IOPOWER);
    if (tegra_gpio_output(kCardPowerGpio, 1))
        panic("SwitchSD: cannot switch the card supply (GPIO PE4) on");
    IOSleep(10);
    switch_mmio_write32(pmc, PMC_PWR_DET_VAL, switch_mmio_read32(pmc, PMC_PWR_DET_VAL) | PMC_SDMMC1_3V3);
    const uint8_t on = static_cast<uint8_t>(0xc0 | kLdo3V3);  // power mode normal, 3.3 V
    const int error = tegra_i2c_write(kPmicBus, kPmicAddress, kPmicLdo2, &on, 1);
    if (error)
        panic("SwitchSD: LDO2 3.3 V after sleep failed (I2C5 %d)", error);
    IOSleep(1);
    w8(RESET, 1);
    for (const uint64_t begin = switch_now_ns(); r8(RESET) & 1;) {
        if (switch_now_ns() - begin > 100000000ULL)
            panic("SwitchSD: SDMMC1 reset did not complete after sleep");
    }
    bus.reset();
    w32(IO_SPARE, r32(IO_SPARE) | 1u << 19);
    w32(VENDOR_IO_TRIM, r32(VENDOR_IO_TRIM) & ~4u);
    w32(VENDOR_CLOCK, (r32(VENDOR_CLOCK) & 0xe0fffffbu) | kTrimB01 << 24);
    w16(CLOCK, 1u | 0x80u << 8);  // internal clock on, slowest divider until setTiming
    for (const uint64_t begin = switch_now_ns(); !(r16(CLOCK) & 2);) {
        if (switch_now_ns() - begin > 20000000ULL)
            panic("SwitchSD: SDMMC1 internal clock not stable after sleep");
    }
    w8(POWER, 0x0e);  // 3.3 V, then bus power on (hekate _sdmmc_set_io_power)
    w8(POWER, 0x0f);
    w16(CLOCK, r16(CLOCK) | 4);
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~0x18u);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | CONTROL2_V4_64BIT);
    w8(TIMEOUT, 0x0e);
    w32(SIGNAL_ENABLE, 0);
    w32(STATUS_ENABLE, 0x03ff800bu);
    w32(STATUS, 0xffffffff);
    switch_dcache_sync();
}

// System sleep (SwitchSuspend's sleep and wake actions, thread context, I2C up). Requests keep queueing while
// suspended; the worker finishes the command it is on and waits. LBA 1 must read back as at boot after resume.
void NativeSwitchBlock::suspend()
{
    IOLockLock(queueLock);
    suspended = true;
    while (working)
        IOLockSleep(queueLock, &working, THREAD_UNINT);
    IOLockUnlock(queueLock);
    // r14-sdcq: the worker leaves CQ mode before it sleeps; this only covers a card that is somehow still in it (the
    // card is about to lose power either way, and CQ mode does not survive that).
    if (cq.state == CqEngine::kOn)
        (void)cq.leave(cqBus);
    // r16-sdcq: leaving CQ mode turned the card's cache off; nothing to flush either way (every completed write is on
    // flash: commitWrite), and the power-off clears Cache Enable.
    dmaSuspend();
    if (irqSource)
        irqSource->disable();
    powerOffCard();
}

// r10-adma2 power-state hooks. Before sleep (the worker is between commands, so no DMA is in flight): SDMA selected and
// the DMA address cleared, so nothing points the engine at the descriptor table while the controller loses power. After
// wake (powerOnController() reset the controller and set v4 + 64-bit addressing again; reinitialize() and the LBA 1 check
// ran by SDMA): ADMA2 carries data again only if that addressing mode took and the SMMU still leaves SDMMC1 untranslated.
// The table needs nothing more: every ADMA2 command builds it afresh, and command() selects ADMA2 or SDMA per command.
void NativeSwitchBlock::dmaSuspend()
{
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);
    w32(DMA_LOW, 0);
    w32(DMA_HIGH, 0);
    table.reset();
}

void NativeSwitchBlock::dmaResume()
{
    if (!adma)
        return;
    const uint16_t control2 = r16(HOST_CONTROL2);
    const bool translated = (switch_mmio_read32(mc, 0xa94) & 0x80000000u) != 0u;
    if ((control2 & CONTROL2_V4_64BIT) == CONTROL2_V4_64BIT && !translated)
        return;
    adma = false;
    IOLog("SwitchSD: ADMA2 off after sleep (HOST_CONTROL2 0x%04x, SMMU %s for SDMMC1); requests go through the bounce "
          "buffer\n",
          control2, translated ? "on" : "off");
}

void NativeSwitchBlock::resume()
{
    powerOnController();
    if (irqSource)
        irqSource->enable();
    const bool uhs = lowVoltageBus && enablePllc4();
    if (lowVoltageBus && !uhs)
        panic("SwitchSD: PLLC4 does not lock after sleep (PLLC4_BASE 0x%08x)", switch_mmio_read32(car, CAR_PLLC4_BASE));
    (void)reinitialize(PLLP_KHZ * 2 / (kPllpDivider48MHz + 2), 0xc0 | kLdo3V3, uhs, true);
    uint8_t again[kSector] = {};
    const int step = transfer(1, 1, false, again);
    if (step || memcmp(again, gptHeader, sizeof(again))) {
        panic("SwitchSD: LBA 1 differs after sleep (read step %d, bus %s): not the card this boot started from", step,
              busMode);
    }
    dmaResume();
    cq.powerCycled();
    if (cq.stopped)
        cqStopped();  // r18-sdcq: a card stuck at sleep may have been the kMaxStuck-th
    IOLockLock(queueLock);
    suspended = false;
    IOLockWakeup(queueLock, &head, false);
    IOLockUnlock(queueLock);
}

// setup(): nullptr when ADMA2 may carry the data path, else why not. The controller must offer ADMA2 and keep v4 64-bit
// addressing, and a 1 MiB read through ADMA2 must match `reference` (the same MiB read by SDMA) byte for byte. Its table
// comes from the data path's own builder, over runs chosen to meet what the data path will: full-length descriptors in
// the contiguous bounce buffer, a short one that ends inside a sector and one that completes it, and single kernel pages
// out of physical order, which on this board land above 4 GiB about half the time (RAM is 0x80000000-0x17fffffff). Every
// destination byte starts in DRAM as the complement of what the card should deliver, so a byte the engine skipped or
// misplaced shows. Test pages above 4 GiB that kept their fill while everything else arrived mean the engine drops the
// high address bits: ADMA2 then stays below 4 GiB, as it does when no test page was above 4 GiB. (Such a write cannot
// have reached RAM: 4 GiB lower lies below DRAM at 0x80000000, outside the memory controller's EMEM aperture; pages
// whose alias would fall in IRAM at 0x40000000 are not used.)
const char *NativeSwitchBlock::checkAdma(const uint8_t *reference, uint32_t *descriptors, uint32_t *highPages,
                                         uint32_t *highOk, uint32_t *staged)
{
    *descriptors = *highPages = *highOk = *staged = 0;
    if (copyFlush)
        return "copyFlush";
    if (!(r32(CAPABILITIES) & (1u << 19)))
        return "the controller reports no ADMA2";
    if ((r16(HOST_CONTROL2) & CONTROL2_V4_64BIT) != CONTROL2_V4_64BIT)
        return "HOST_CONTROL2 does not keep v4 64-bit addressing";
    if (!switch_dma_alloc(&tableMemory, kAdmaTableBytes, 0x00000000ffffffffULL & ~(kAdmaPage - 1)))
        return "no memory for the descriptor table";
    table.entries = reinterpret_cast<SwitchAdmaDescriptor *>(tableMemory.va);
    tablePA = tableMemory.pa;
    if (tablePA & (kAdmaPage - 1))
        return "the descriptor table is not page-aligned below 4 GiB";
    table.capacity = kAdmaDescriptors;
    table.limit = kAdmaDmaLimit;

    constexpr uint32_t kTestPages = 128;
    IOBufferMemoryDescriptor *pages =
        IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, kTestPages * kAdmaPage, kAdmaPage);
    if (!pages)
        return "no memory for the test pages";
    if (pages->prepare() != kIOReturnSuccess) {
        pages->release();
        return "the test pages cannot be wired";
    }
    auto *const pageBytes = static_cast<uint8_t *>(pages->getBytesNoCopy());

    struct Run {
        uint64_t pa;
        uint8_t *at;
        uint32_t length;
    };

    static Run runs[kTestPages + 3];  // setup() runs once; ~3 KiB is too much for its stack
    unsigned count = 0;
    uint32_t streamed = 0;
    const auto take = [&](uint64_t pa, uint8_t *at, uint32_t length) {
        runs[count++] = {pa, at, length};
        streamed += length;
    };
    const char *why = nullptr;
    const uint64_t page0 = pages->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (pageBytes && page0) {
        take(bouncePA, bounce, 0x2f000);                    // three full-length descriptors and a short one
        take(page0 + 0x40, pageBytes + 0x40, 0x40);         // ends inside a sector...
        take(bouncePA + 0x30000, bounce + 0x30000, 0x1c0);  // ...which this one completes
        for (uint32_t i = kTestPages - 1; i; --i) {
            const uint64_t pa = pages->getPhysicalSegment(i * kAdmaPage, nullptr, kIOMemoryMapperNone);
            if (pa && pa - kAdmaIramAlias >= kAdmaIramBytes)
                take(pa, pageBytes + i * kAdmaPage, kAdmaPage);
        }
        take(bouncePA + 0x40000, bounce + 0x40000, kBounceBytes - streamed);  // the rest, in the bounce buffer
        table.reset();
        uint32_t at = 0;
        for (unsigned i = 0; i < count && !why; ++i) {
            for (uint32_t k = 0; k < runs[i].length; ++k)
                runs[i].at[k] = static_cast<uint8_t>(~reference[at + k]);
            if (runs[i].pa >= (1ULL << 32))
                ++*highPages;
            if (table.add(runs[i].pa, runs[i].length, cacheableRam))
                why = "the builder refused the test memory";
            at += runs[i].length;
        }
        if (!why && !table.finish())
            why = "no descriptors";
    } else {
        why = "the test pages have no physical address";
    }
    if (!why) {
        // As the data path's ADMA2 commands, without run()'s retry and re-tune: a failure here only keeps the bounce
        // buffer.
        *descriptors = table.count;
        switch_dcache_clean_lines(table.entries, table.count * sizeof(SwitchAdmaDescriptor));
        syncLines(SWITCH_DCACHE_CLEAN_INVALIDATE, table, false);
        switch_dcache_sync();
        uint32_t status = 0;
        const int step = command(static_cast<uint32_t>(firstSector), kBounceSectors, false, table.count, &status);
        syncLines(SWITCH_DCACHE_INVALIDATE, table, false);
        switch_dcache_sync();
        if (step) {
            snprintf(admaWhy, sizeof(admaWhy), "the read failed: step %d, STATUS 0x%08x, ADMA state 0x%02x at 0x%llx",
                     step, status, admaErrorState, admaErrorAddress);
            why = admaWhy;
            recover(true);
        }
    }
    unsigned highMissing = 0;
    for (uint32_t i = 0, at = 0; !why && i < count; at += runs[i++].length) {
        if (!memcmp(runs[i].at, reference + at, runs[i].length)) {
            if (runs[i].pa >= (1ULL << 32))
                ++*highOk;
            continue;
        }
        bool untouched = runs[i].pa >= (1ULL << 32);
        for (uint32_t k = 0; untouched && k < runs[i].length; ++k)
            untouched = runs[i].at[k] == static_cast<uint8_t>(~reference[at + k]);
        if (untouched) {
            ++highMissing;
        } else {
            why = "the 1 MiB read differs from SDMA's";
        }
    }
    // r12-sdio: the read path's per-segment staging (SwitchAdmaStaging) once on the hardware, before any request uses it:
    // the partition's first half MiB into the test pages, every page whose number is a multiple of 3 refused and so staged
    // through the bounce buffer, then copied out as the data path does; all of it must match SDMA's read. A failure leaves
    // staging off (a read chain with a refused segment then bounces whole, as before), with the reason on the ADMA2 line.
    *staged = 0;
    stages = why ? nullptr : static_cast<Staging::Stage *>(IOMalloc(kStages * sizeof(Staging::Stage)));
    const char *stagingWhy = nullptr;
    if (why) {
        stagingWhy = "";
    } else if (!stages) {
        stagingWhy = "no memory for the stages";
    }
    if (!stagingWhy) {
        constexpr uint32_t kTestBytes = kTestPages * kAdmaPage;
        for (uint32_t k = 0; k < kTestBytes; ++k)
            pageBytes[k] = static_cast<uint8_t>(~reference[k]);
        staging.reset(&table, bouncePA, stages, kStages);
        table.limit = kAdma4GiB;
        const auto refuseEveryThird = [](uint64_t page) {
            return cacheableRam(page) && atop_64(page) % 3;
        };
        if (staging.addMemory(pages, kTestBytes, refuseEveryThird) || !staging.finish() || table.bytes != kTestBytes ||
            !staging.count || staging.staged == kTestBytes) {
            stagingWhy = "the builder refused the test pages";
        } else {
            *staged = static_cast<uint32_t>(staging.staged / kAdmaPage);
            prepareDirect(false, ReadFill::kCpuFill);
            uint32_t status = 0;
            const int step =
                command(static_cast<uint32_t>(firstSector), kTestBytes / kSector, false, table.count, &status);
            finishDirect(false);
            if (step) {
                recover(true);
                stagingWhy = "its read failed";
            } else if (!copyStaged()) {
                stagingWhy = "a staged copy failed";
            } else if (memcmp(pageBytes, reference, kTestBytes)) {
                stagingWhy = "its read differs from SDMA's";
            }
        }
        staging.count = 0;
        table.reset();
    }
    stagingOn = (stagingWhy == nullptr);
    if (stagingWhy && *stagingWhy)
        snprintf(stagingReason, sizeof(stagingReason), ", per-segment staging off (%s)", stagingWhy);
    pages->complete();
    pages->release();
    // r12-sdio: test pages above 4 GiB that matched prove the high address word; ones that kept their fill disprove it;
    // with none up there (the usual case at boot) the data path's first read chains that reach there prove it (high).
    if (highMissing) {
        high.state = SwitchAdmaHigh::kOff;
    } else {
        high.state = *highPages || !SwitchAdmaHigh::kLiveProofs ? SwitchAdmaHigh::kOn : SwitchAdmaHigh::kProving;
    }
    return why;
}

// r14-sdcq: setup(): nullptr when multi-block ADMA2 commands may announce their length with Auto CMD23 (SET_BLOCK_COUNT)
// instead of stopping with Auto CMD12, else why not; and what one 4 KiB read took each way (the card's access time is
// most of it; the difference is the stop command's cost, or CMD23's). kCmd23Pairs 4 KiB reads (the size two-thirds of
// requests have) at blocks spread over the APFS partition, each read once with CMD12 and once with CMD23 by ADMA2 into
// the bounce buffer's first two pages, which start as 0xff and 0x00 so a read that delivered nothing differs. The order
// alternates so the card's own read cache favours neither. Every pair must match byte for byte; an error or a difference
// keeps CMD12, with the reason. Runs once in setup() after checkAdma (it uses the same table and syncs).
const char *NativeSwitchBlock::checkCmd23(uint64_t *withNs, uint64_t *withoutNs)
{
    constexpr uint32_t kSectors = 8;
    constexpr uint32_t kBytes = kSectors * kSector;
    *withNs = *withoutNs = 0;
    const uint64_t blocks = sectorCount / kSectors;
    if (blocks < kCmd23Pairs)
        return "the partition is too small";
    const char *why = nullptr;
    for (uint32_t i = 0; i < kCmd23Pairs && !why; ++i) {
        // A multiplicative hash spreads the reads over the whole partition, 4 KiB-aligned.
        const uint64_t lba = firstSector + (static_cast<uint64_t>(i + 1) * 2654435761ULL % blocks) * kSectors;
        for (unsigned pass = 0; pass < 2 && !why; ++pass) {
            const bool with = (pass == 0) == ((i & 1) != 0);
            const uint32_t at = with ? kBytes : 0;
            memset(bounce + at, with ? 0x00 : 0xff, kBytes);
            table.reset();
            if (table.add(bouncePA + at, kBytes, cacheableRam) || !table.finish()) {
                why = "the builder refused the bounce buffer";
                break;
            }
            prepareDirect(false, ReadFill::kCpuFill);
            uint32_t status = 0;
            const uint64_t began = switch_now_ns();
            const int step = command(static_cast<uint32_t>(lba), kSectors, false, table.count, &status, with);
            (with ? *withNs : *withoutNs) += switch_now_ns() - began;
            finishDirect(false);
            if (step) {
                recover(true);
                snprintf(cmd23Why, sizeof(cmd23Why), "read %u with %s failed: step %d, STATUS 0x%08x", i,
                         with ? "CMD23" : "CMD12", step, status);
                why = cmd23Why;
            }
        }
        if (!why && memcmp(bounce, bounce + kBytes, kBytes)) {
            uint32_t at = 0;
            while (bounce[at] == bounce[kBytes + at])
                ++at;
            snprintf(cmd23Why, sizeof(cmd23Why), "read %u (lba %llu) differs from CMD12's at byte %u", i, lba, at);
            why = cmd23Why;
        }
    }
    table.reset();
    return why;
}

// r14-sdcq: setup(): nullptr when read chains may go through the card's command queue (SwitchCq.h), else why not. The
// card must offer one (Performance Enhancement byte 6) with its control byte inside one page, reading 00h (nobody left it
// on); the data path must run ADMA2 with interrupts and have its proofs' buffer. Then one test group must work: *tasks
// 4 KiB reads at blocks spread over the APFS partition assigned at once and executed in order by ADMA2 into the bounce
// buffer (filled with 0xa5 first), out of CQ mode, the same blocks read the legacy way (SDMA, CMD12) into checkBuffer
// must match byte for byte. The same check times the group (*queuedNs, assignment to the last task's end), as many
// legacy 4 KiB reads of other blocks one at a time (*serialNs), and entering and leaving CQ mode. Any failure stops the
// queue for the boot (the card out of CQ mode, power-cycled if it would not leave). r18-sdcq: but a protocol error
// (entering, assigning, a task's data) is recovered as the data path recovers one (SwitchCq.h recover, one os_log line)
// and the test group runs once more. The queue's depth is one below what byte 6 offers (31 of a SanDisk A2 card's 32), as
// the Raspberry Pi kernel uses it (sd.c: cmdq_depth = reg_buf[6] & 0x1f, "31 tags"), the only Linux that queues SD reads:
// Task ID 31 has no Linux mileage on these cards.
// r16-sdcq: each CQ session turns the card's cache on first (4.19.1; SwitchCq.h enter) and off when it ends, each
// byte read back lazily; a failure's line says what the control bytes read back (describeCq). A cache some earlier stage
// left on goes off before any write. kTests 16: a steadier measurement for setup()'s decision whether the queue is
// faster at all.
const char *NativeSwitchBlock::checkCq(uint64_t *queuedNs, uint64_t *serialNs, uint64_t *enterNs, uint64_t *leaveNs,
                                       unsigned *tasks)
{
    *queuedNs = *serialNs = *enterNs = *leaveNs = 0;
    *tasks = 0;
    if (!havePerf)
        return extWhy ? extWhy : "no performance enhancement function";
    cqControlAddress = switchSdCqControlAddress(extInfo.perf);
    if (!cqControlAddress)
        return "its control byte crosses a page";
    if (!switch_dma_alloc(&regMemory, kAdmaPage, 0x00000000ffffffffULL & ~(kAdmaPage - 1)) ||
        (regMemory.pa & (kAdmaPage - 1))) {
        regBuffer = nullptr;
        return "no DMA buffer for its register";
    }
    regBuffer = regMemory.va;
    regPA = regMemory.pa;
    // r16-sdcq: a cache some earlier stage left on goes off before any write (one that will not has every write flushed:
    // commitWrite).
    if (perf.cacheEnable & kSdCacheEnable) {
        cq.cacheOn = true;
        (void)cq.disableCache(cqBus);
    }
    if (perf.cqDepth < 2)
        return "the card offers no queue";
    if (perf.cqControl & kSdCqEnable)
        return "its queue is enabled already";
    if (!checkBuffer)
        return "no buffer for its proofs";
    constexpr uint32_t kSectors = 8;
    constexpr uint32_t kBytes = kSectors * kSector;
    constexpr uint32_t kTests = 16;
    const uint8_t depth = static_cast<uint8_t>(perf.cqDepth > 2 ? perf.cqDepth - 1 : perf.cqDepth);
    const unsigned n = depth < kTests ? depth : kTests;
    const uint64_t blocks = sectorCount / kSectors;
    if (blocks < 2ULL * kTests)
        return "the partition is too small";
    // Blocks spread over the partition (another hash than checkCmd23's): [0, n) the group's, [n, 2n) the serial reads'.
    const auto lbaOf = [&](unsigned i) {
        return firstSector + (static_cast<uint64_t>(i + 101) * 2246822519ULL % blocks) * kSectors;
    };
    const auto oneRun = [&](uint32_t at) {
        table.reset();
        return !table.add(bouncePA + at, kBytes, cacheableRam) && table.finish();
    };
    uint32_t status = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (!oneRun(i * kBytes))
            return "the builder refused the bounce buffer";
        prepareDirect(false, ReadFill::kCpuFill);
        const uint64_t t0 = switch_now_ns();
        const int step =
            command(static_cast<uint32_t>(lbaOf(n + i)), kSectors, false, table.count, &status, cmd23Reads);
        *serialNs += switch_now_ns() - t0;
        finishDirect(false);
        if (step) {
            recover(true);
            snprintf(cqWhy, sizeof(cqWhy), "a legacy test read failed: step %d, STATUS 0x%08x", step, status);
            return cqWhy;
        }
    }
    // The engine is usable from here on; every failure below stops it again (the end of this function). r16-sdcq: with
    // the card's cache, which enter() turns on first and leaving turns off again (both inside enterNs/leaveNs).
    cq.start(depth, perf.cache);
    SwitchCqTask test[kTests];
    for (unsigned i = 0; i < n; ++i)
        test[i] = {lbaOf(i), kSectors};
    const char *why = nullptr;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        why = nullptr;
        bool protocol = false;  // r18-sdcq: the queue protocol's failure, which the recovery covers
        uint64_t t0 = switch_now_ns();
        if (!cq.enter(cqBus)) {
            why = cq.why ? cq.why : "the card would not enter CQ mode";
            protocol = cq.usable() || cq.stuck;
        }
        *enterNs = switch_now_ns() - t0;
        memset(bounce, 0xa5, static_cast<size_t>(n) * kBytes);
        t0 = switch_now_ns();
        if (!why && !cq.submit(cqBus, test, n)) {
            why = cq.why ? cq.why : "the test group could not be assigned";
            protocol = !cq.stopped;
        }
        for (unsigned i = 0; !why && i < n; ++i) {
            if (!oneRun(i * kBytes)) {
                why = "the builder refused the bounce buffer";
                break;
            }
            prepareDirect(false, ReadFill::kCpuFill);
            const uint8_t before = cardLast;
            const uint8_t task = static_cast<uint8_t>(cq.next);
            int step = issue(static_cast<uint32_t>(test[i].lba), kSectors, false, table.count, false, cq.argument());
            if (!step)
                step = complete(kSectors, false, table.count, &status, nullptr, false, true);
            finishDirect(false);
            if (step) {
                const uint32_t r1 = (status & INT_COMMAND) ? r32(RESPONSE) : 0;
                cardLastIrq = status;
                recover(true);
                cq.note(46, task, before, step, r1, cqBus);
                snprintf(cqWhy, sizeof(cqWhy), "test task %u failed: step %d, STATUS 0x%08x", i, step, status);
                why = cqWhy;
                protocol = true;
                break;
            }
            cq.executed();
        }
        *queuedNs = switch_now_ns() - t0;
        if (why && protocol && !attempt) {
            cq.error.attempt = 1;
            cq.error.tasks = static_cast<uint8_t>(n);
            const CqEngine::Recovery r = cq.recover(cqBus);
            cqRecovered(cq.error);
            if (r == CqEngine::kStuck || (r == CqEngine::kInPlace && !cq.leave(cqBus))) {
                resetCard();
                cq.powerCycled();
            }
            if (cq.usable())
                continue;
        }
        if (why)
            break;
        t0 = switch_now_ns();
        if (!cq.leave(cqBus))
            why = "the card stays in CQ mode";
        *leaveNs = switch_now_ns() - t0;
        // The same blocks the legacy way (out of CQ mode now), compared.
        for (unsigned i = 0; !why && i < n; ++i) {
            if (transfer(test[i].lba, kSectors, false, checkBuffer + static_cast<size_t>(i) * kBytes)) {
                why = "a legacy re-read failed";
            } else if (memcmp(bounce + static_cast<size_t>(i) * kBytes, checkBuffer + static_cast<size_t>(i) * kBytes,
                              kBytes)) {
                uint32_t at = 0;
                while (bounce[i * kBytes + at] == checkBuffer[i * kBytes + at])
                    ++at;
                snprintf(cqWhy, sizeof(cqWhy), "test task %u (lba %llu) differs from the legacy read at byte %u", i,
                         test[i].lba, at);
                why = cqWhy;
            }
        }
        break;
    }
    table.reset();
    if (why) {
        // Off for the boot; the card out of CQ mode first (power-cycled if it would not leave), its cache off again.
        // r16-sdcq: the reason with what the control bytes read back (why may be cqWhy itself: copied first).
        const bool stuck = !cq.fail(cqBus, why) || cq.stuck;
        char reason[128];
        strlcpy(reason, why, sizeof(reason));
        char detail[192];
        describeCq(detail, sizeof(detail));
        snprintf(cqWhy, sizeof(cqWhy), "%s; %s", reason, detail);
        if (stuck) {
            IOLog("SwitchSD: the card stayed in CQ mode: power-cycled\n");
            resetCard();
            cq.powerCycled();
        }
        return cqWhy;
    }
    *tasks = n;
    cq.resetCounts();  // r18-sdcq: the data path's lines count its own groups (r8c's "16 tasks in 1 groups" were these)
    return nullptr;
}

bool NativeSwitchBlock::setup()
{
    regs = switch_mmio_map(kControllerPA, 0x1000);
    mc = switch_mmio_map(kMemoryControllerPA, 0x1000);
    // CAR, APB_MISC/pinmux and PMC serve the SDR104 bring-up only; without them it is skipped.
    car = switch_mmio_map(kCarPA, 0x1000);
    apb = switch_mmio_map(kApbMiscPA, 0x4000);
    pmc = switch_mmio_map(kPmcPagePA, 0x1000);
    queueLock = IOLockAlloc();
    if (!regs || !mc || !queueLock) {
        IOLog("SwitchSD: map/alloc failed\n");
        return false;
    }

    if ((r32(PRESENT) & PRESENT_CARD) != PRESENT_CARD || !(r16(CLOCK) & 4)) {
        IOLog("SwitchSD: no card or SD clock off\n");
        return false;
    }
    // SDMMC1 group translation must be off for physical DMA (MC_SMMU_SDMMC1A_ASID).
    if (switch_mmio_read32(mc, 0xa94) & 0x80000000u) {
        IOLog("SwitchSD: SMMU translation enabled for SDMMC1\n");
        return false;
    }

    if (!switch_dma_alloc(&bounceMemory, kBounceBytes,
                          0x00000000ffffffffULL & ~static_cast<uint64_t>(kBounceAlign - 1))) {
        IOLog("SwitchSD: bounce allocation failed\n");
        return false;
    }
    bounce = bounceMemory.va;
    bouncePA = bounceMemory.pa;
    if (bouncePA & (kBounceAlign - 1)) {
        IOLog("SwitchSD: bad bounce PA 0x%llx\n", bouncePA);
        return false;
    }

    // SDHCI v4 64-bit SDMA (Tegra210 HOST v4). Interrupt signals stay off except
    // while the worker waits for one (waitSignal).
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~0x18u);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | CONTROL2_V4_64BIT);
    w8(TIMEOUT, 0x0e);
    w32(SIGNAL_ENABLE, 0);
    w32(STATUS_ENABLE, 0x03ff800bu);
    w32(STATUS, 0xffffffff);
    switch_dcache_sync();
    startInterrupt();
    // What a scatter-gather (ADMA2) path straight into the requests' pages would rest on: CAPABILITIES bit 19 (ADMA2),
    // 28 (64-bit addressing, v3) and 27 (64-bit addressing, v4). The log only, once.
    const uint32_t caps = r32(CAPABILITIES);
    const uint32_t capsHi = r32(CAPABILITIES_HI);
    os_log(OS_LOG_DEFAULT, "SwitchSD: host version %#06x caps %#010x %#010x (ADMA2 %u, 64-bit v3 %u v4 %u), copy %s",
           r16(VERSION), caps, capsHi, (caps >> 19) & 1, (caps >> 28) & 1, (caps >> 27) & 1,
           copyFlush ? "readBytes/writeBytes (flushing)" : "copypv (no flush)");

    // GPT: header at LBA 1, then entries. Exactly one APFS partition. The header is read in the
    // inherited bus mode first; after a re-initialization it must read back byte for byte, which
    // checks the new mode end to end (sampling, tuning) beyond the bus CRC.
    uint8_t header[kSector] = {};
    if (transfer(1, 1, false, header)) {
        IOLog("SwitchSD: GPT header read failed\n");
        return false;
    }
    memcpy(gptHeader, header, sizeof(gptHeader));
    if (raiseBusSpeed()) {
        uint8_t again[kSector] = {};
        const int step = transfer(1, 1, false, again);
        if (step || memcmp(header, again, sizeof(header))) {
            panic("SwitchSD: LBA 1 differs after switching to %s (read step %d); boot with switch-no-sd-uhs", busMode,
                  step);
        }
    }
    identify();
    const uint64_t entriesLBA = le64(header + 72);
    const uint32_t entryCount = le32(header + 80);
    const uint32_t entrySize = le32(header + 84);
    if (memcmp(header, "EFI PART", 8) || entrySize < 128 || entrySize > 512 || (entrySize & 7) || !entryCount ||
        entryCount * entrySize > 64 * 1024 || entriesLBA < 2) {
        IOLog("SwitchSD: invalid GPT header\n");
        return false;
    }
    const uint32_t entrySectors = (entryCount * entrySize + kSector - 1) / kSector;
    static uint8_t entries[64 * 1024];
    for (uint32_t done = 0; done < entrySectors;) {
        const uint32_t n = entrySectors - done > 64 ? 64 : entrySectors - done;
        if (transfer(entriesLBA + done, n, false, entries + static_cast<size_t>(done) * kSector)) {
            IOLog("SwitchSD: GPT entry read failed\n");
            return false;
        }
        done += n;
    }
    unsigned found = 0;
    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t *e = entries + static_cast<size_t>(i) * entrySize;
        if (memcmp(e, kApfsType, 16) && !partitionNamed(e, kPartitionName))
            continue;
        const uint64_t first = le64(e + 32);
        const uint64_t last = le64(e + 40);
        if (!first || last < first)
            continue;
        firstSector = first;
        sectorCount = last - first + 1;
        ++found;
    }
    if (found != 1 || firstSector + sectorCount > UINT32_MAX) {
        IOLog("SwitchSD: expected one APFS or \"%s\" partition, found %u\n", kPartitionName, found);
        return false;
    }

    // DMA for the data path. Both checks read the partition's first MiB and compare it with the same MiB read as four
    // 256 KiB SDMA commands (which never reach an SDMA boundary). ADMA2 straight to the requests' pages once checkAdma()
    // passes; then reads go up to 1 MiB and writes up to kDirectWriteSectors per command, and the bounce fallback splits
    // longer chains into 256 KiB SDMA commands. Otherwise SDMA through the bounce buffer as before: commands over 512 KiB
    // cross the SDMA boundary (command()), so one 1 MiB SDMA read must match too before reads may be that long. It goes
    // straight to command() so that a failure is not retried and re-tuned (transfer() would step the clock down); a
    // failure keeps 256 KiB, with one line.
    if (sectorCount >= kBounceSectors) {
        uint8_t *reference = static_cast<uint8_t *>(IOMallocData(kBounceBytes));
        int step = reference ? 0 : -2;
        for (uint32_t done = 0; !step && done < kBounceSectors; done += kSafeSectors)
            step = transfer(firstSector + done, kSafeSectors, false, reference + static_cast<size_t>(done) * kSector);
        uint32_t descriptors = 0;
        uint32_t highPages = 0;
        uint32_t highOk = 0;
        uint32_t staged = 0;
        const char *why =
            step ? "the reference read failed" : checkAdma(reference, &descriptors, &highPages, &highOk, &staged);
        if (!why) {
            adma = true;
            maxSectors = kBounceSectors;
            maxWriteSectors = kDirectWriteSectors;
            checkBuffer = static_cast<uint8_t *>(IOMallocData(kBounceBytes));
            writeChecks = SwitchAdmaHigh::kLiveProofs && checkBuffer ? kWriteChecks : 0;
            largeWriteChecks = SwitchAdmaHigh::kLiveProofs && checkBuffer ? kLargeWriteChecks : 0;
            admaWrites = !SwitchAdmaHigh::kLiveProofs || checkBuffer != nullptr;
            // Writes over 256 KiB only by ADMA2 with their read-backs; without the buffer for those, 256 KiB commands.
            writeSectors = admaWrites ? kDirectWriteSectors : kSafeSectors;
            // Without the buffer nothing above 4 GiB can be proven: it stays below.
            if (!checkBuffer && high.state == SwitchAdmaHigh::kProving)
                high.refuse();
            static const char *const states[] = {" (above 4 GiB: proven by the first reads that reach there)", "",
                                                 " (ADMA2 stays below 4 GiB)"};
            if (stagingOn)
                snprintf(stagingReason, sizeof(stagingReason), ", per-segment staging on (%u pages)", staged);
            IOLog("SwitchSD: ADMA2 on: 1 MiB read through %u descriptors matches SDMA, %u of %u test pages above "
                  "4 GiB right%s; commands up to %u KiB read, %u KiB write%s%s\n",
                  descriptors, highOk, highPages, states[high.state], maxSectors * kSector / 1024,
                  maxWriteSectors * kSector / 1024,
                  admaWrites ? (SwitchAdmaHigh::kLiveProofs ? ", the first writes read back" : "")
                             : ", writes through the bounce buffer",
                  stagingReason);
            // r14-sdcq: Auto CMD23 when the card's SCR lists CMD23 and both framings read the same bytes.
            if (!haveScr || !(scr.cmdSupport & kSdCmd23)) {
                IOLog("SwitchSD: CMD23 not used (%s); multi-block commands stop by CMD12\n",
                      haveScr ? "the card's SCR does not list it" : "no SCR");
            } else {
                uint64_t withNs = 0;
                uint64_t withoutNs = 0;
                if (const char *cmd23Off = checkCmd23(&withNs, &withoutNs)) {
                    IOLog("SwitchSD: CMD23 off (%s); multi-block commands stop by CMD12\n", cmd23Off);
                } else {
                    cmd23Reads = true;
                    cmd23Writes = admaWrites;
                    cmd23WriteChecks = SwitchAdmaHigh::kLiveProofs && cmd23Writes ? kCmd23WriteChecks : 0;
                    const uint64_t with = withNs / kCmd23Pairs;
                    const uint64_t without = withoutNs / kCmd23Pairs;
                    IOLog("SwitchSD: CMD23 on: %u 4 KiB reads match CMD12's; %llu.%llu us per read with CMD23, "
                          "%llu.%llu us with CMD12 (%lld ns saved per command); multi-block reads%s announce their "
                          "length%s\n",
                          kCmd23Pairs, with / 1000, with / 100 % 10, without / 1000, without / 100 % 10,
                          static_cast<long long>(without) - static_cast<long long>(with),
                          cmd23Writes ? " and writes" : "",
                          cmd23WriteChecks ? ", the first CMD23 writes read back" : "");
                }
            }
            // r14-sdcq: gap reads need the sink and, for their proofs, checkBuffer; queue merging must be on.
            // switch_dma_alloc leaves no line of the sink dirty in the cache: the engine's writes there are never
            // overwritten by an eviction, and nobody reads them.
            if ((!SwitchAdmaHigh::kLiveProofs || checkBuffer) && merge &&
                switch_dma_alloc(&sinkMemory, static_cast<size_t>(kGapSectors) * kSector,
                                 0x00000000ffffffffULL & ~(kAdmaPage - 1)) &&
                !(sinkMemory.pa & (kAdmaPage - 1)) && cacheableRam(sinkMemory.pa)) {
                sinkPA = sinkMemory.pa;
            }
            gapsOn = sinkPA != 0;
            gapProofs = SwitchAdmaHigh::kLiveProofs && gapsOn ? kGapProofs : 0;
            if (gapsOn) {
                IOLog("SwitchSD: gap reads on: a read chain takes queued reads up to %u KiB beyond it (%u KiB per "
                      "command), the card reading through the gap; the first %u such chains are compared with SDMA\n",
                      kGapSectors * kSector / 1024, kGapBudget * kSector / 1024, gapProofs);
            } else {
                const char *gapsOff = "no sink page below 4 GiB";
                if (!checkBuffer) {
                    gapsOff = "no buffer for their proofs";
                } else if (!merge) {
                    gapsOff = "merge off";
                }
                IOLog("SwitchSD: gap reads off (%s)\n", gapsOff);
            }
            // r14-sdcq: the command queue, when the card offers one and its test group reads what the legacy path reads.
            uint64_t queuedNs = 0;
            uint64_t serialNs = 0;
            uint64_t enterNs = 0;
            uint64_t leaveNs = 0;
            unsigned tested = 0;
            const char *cqOff = checkCq(&queuedNs, &serialNs, &enterNs, &leaveNs, &tested);
            // r16-sdcq: the queue only where it is faster. Its test group's saving per task must pay for CQ mode's way
            // in and out (CMD49 + CMD48 each) within a group the card can take: from legacy mode a group then needs
            // cqMinChains chains (at least two). A queue that saves nothing, or too little for its depth, stays off
            // (with the cache: SwitchCq.h fail).
            const uint64_t saving = !cqOff && tested && serialNs > queuedNs ? (serialNs - queuedNs) / tested : 0;
            const uint64_t toggle = enterNs + leaveNs;
            const uint64_t need = saving ? (toggle + saving - 1) / saving : 0;
            if (!cqOff && (!saving || need > cq.depth)) {
                snprintf(cqWhy, sizeof(cqWhy),
                         "its test group was no faster: %u queued 4 KiB reads took %llu us against %llu us one at a "
                         "time, into and out of CQ mode %llu us",
                         tested, queuedNs / 1000, serialNs / 1000, toggle / 1000);
                (void)cq.fail(cqBus, "no faster");
                cqOff = cqWhy;
            }
            if (cqOff) {
                IOLog("SwitchSD: command queue not used (%s); reads stay one command at a time\n", cqOff);
            } else {
                cqProofs = SwitchAdmaHigh::kLiveProofs ? kCqProofs : 0;
                cqMinChains = need < 2 ? 2 : static_cast<unsigned>(need);
                IOLog("SwitchSD: command queue on (sequential mode, depth %u, cache %s): %u queued 4 KiB reads match "
                      "the legacy path's; they took %llu us queued against %llu us one at a time (%llu us saved per "
                      "task); into CQ mode %llu us, out %llu us; %u or more independent queued reads go as tasks, the "
                      "first %u tasks read again the legacy way\n",
                      static_cast<unsigned>(cq.depth), cq.useCache ? "on in CQ mode only, off for every write" : "off",
                      tested, queuedNs / 1000, serialNs / 1000, saving / 1000, enterNs / 1000, leaveNs / 1000,
                      cqMinChains, cqProofs);
            }
        } else {
            IOLog("SwitchSD: ADMA2 off (%s); every request through the bounce buffer\n", why);
        }
        if (!step && !adma) {
            switch_dcache_clean_invalidate_lines(bounce, kBounceBytes);
            switch_dcache_sync();
            uint32_t status = 0;
            step = command(static_cast<uint32_t>(firstSector), kBounceSectors, false, 0, &status);
            if (step) {
                recover(true);
            } else {
                switch_dcache_invalidate_lines(bounce, kBounceBytes);
                switch_dcache_sync();
            }
            const bool same = (step == 0) && (memcmp(reference, bounce, kBounceBytes) == 0);
            if (same) {
                maxSectors = sdmaReadSectors = kBounceSectors;
            } else {
                IOLog("SwitchSD: 1 MiB commands failed their check (step %d, %s); keeping 256 KiB\n", step,
                      step ? "error" : "data differs");
            }
        }
        if (reference)
            IOFreeData(reference, kBounceBytes);
    }

    releaseChecks();
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&NativeSwitchBlock::worker, this, &thread) != KERN_SUCCESS) {
        IOLog("SwitchSD: worker start failed\n");
        return false;
    }
    thread_deallocate(thread);
    return true;
}

extern "C" void switch_block_suspend(void)
{
    if (gOwner)
        gOwner->suspend();
}

extern "C" void switch_block_resume(void)
{
    if (gOwner)
        gOwner->resume();
}

extern "C" int switch_block_publish(void)
{
    if (gPublishStatus >= 0)
        return gPublishStatus;
    gPublishStatus = SWITCH_BLOCK_NOT_READY;
    IOService *resources = IOService::getResourceService();
    auto *owner = OSTypeAlloc(NativeSwitchBlock);
    if (!owner || !owner->init() || !owner->attach(resources) || !owner->start(resources) || !owner->setup()) {
        // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): a half-published service is kept, never freed
        IOLog("SwitchSD: publish failed\n");
        return gPublishStatus;
    }
    owner->prepareTrim();
    auto *device = OSTypeAlloc(NativeSwitchBlockDevice);
    if (!device || !device->init(static_cast<OSDictionary *>(nullptr)) ||
        !device->configure(owner, owner->sectorCount) || !device->attach(owner) || !device->start(owner)) {
        // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): a half-published device is kept, never freed
        IOLog("SwitchSD: device start failed\n");
        return gPublishStatus;
    }
    owner->device = device;
    gOwner = owner;
    device->registerService();
    device->release();
    gPublishStatus = SWITCH_BLOCK_OK;
    return gPublishStatus;
}

// r17-launch: the SD card's block device for SwitchCryptex's direct read map (the cryptex images' extents are read
// with this device's doAsyncReadWrite, below the APFS container, not through the image files): the controller service the
// cryptex devices attach to, the window's size and largest read in sectors, and the BSD device of the window's
// whole-disk IOMedia (what DKIOCGETPHYSICALEXTENT names for an APFS file on it). nullptr until published.
extern "C" LIBKERN_RETURNS_NOT_RETAINED IOBlockStorageDevice *
switch_block_device(IOService **controller, dev_t *whole, uint64_t *sectors, uint32_t *maxReadSectors)
{
    *controller = nullptr;
    *whole = NODEV;
    *sectors = 0;
    *maxReadSectors = 0;
    if (!gOwner || !gOwner->device)
        return nullptr;
    // device -> IOBlockStorageDriver -> IOMedia (the whole window)
    if (OSIterator *drivers = gOwner->device->getChildIterator(gIOServicePlane)) {
        while (const IORegistryEntry *driver = OSDynamicCast(IORegistryEntry, drivers->getNextObject())) {
            OSIterator *medias = driver->getChildIterator(gIOServicePlane);
            if (!medias)
                continue;
            while (const IORegistryEntry *media = OSDynamicCast(IORegistryEntry, medias->getNextObject())) {
                const auto *whole_ = OSDynamicCast(OSBoolean, media->getProperty(kIOMediaWholeKey));
                const auto *majorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
                const auto *minorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
                if (whole_ == kOSBooleanTrue && majorNumber && minorNumber && *whole == NODEV)
                    *whole = makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue());
            }
            medias->release();
        }
        drivers->release();
    }
    *controller = gOwner;
    *sectors = gOwner->sectorCount;
    *maxReadSectors = gOwner->maxSectors;
    return gOwner->device;
}

// ---- Volume lookup for the raw root/Preboot/cryptex boot path ----

namespace {
// Returns the unique IOMedia with this UUID if it sits below our block device.
// On success fills device/path; NOT_READY until IOMediaBSDClient has named it.
int findVolume(const char *uuid, dev_t *device, char *path, size_t capacity)
{
    *device = NODEV;
    if (path && capacity)
        path[0] = 0;
    if (!gOwner || !gOwner->device)
        return SWITCH_BLOCK_NOT_READY;
    OSDictionary *match = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    const OSString *wanted = OSString::withCString(uuid);
    if (!match || !properties || !wanted) {
        OSSafeReleaseNULL(match);
        OSSafeReleaseNULL(properties);
        OSSafeReleaseNULL(wanted);
        return SWITCH_BLOCK_NOMEM;
    }
    properties->setObject("UUID", wanted);
    match->setObject(gIOPropertyMatchKey, properties);
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    properties->release();
    wanted->release();
    if (!it)
        return SWITCH_BLOCK_NOT_READY;
    int status = SWITCH_BLOCK_NOT_READY;
    while (const auto *media = OSDynamicCast(IOService, it->getNextObject())) {
        bool ours = false;
        const IORegistryEntry *entry = media->copyParentEntry(gIOServicePlane);
        for (unsigned depth = 0; entry && depth < 32 && !ours; ++depth) {
            ours = entry == gOwner->device;
            const IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
            entry->release();
            entry = parent;
        }
        OSSafeReleaseNULL(entry);
        if (!ours)
            continue;
        const auto *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
        const auto *majorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
        const auto *minorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
        if (!name || !majorNumber || !minorNumber)
            continue;  // BSD client not attached yet
        *device = makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue());
        if (path && capacity)
            snprintf(path, capacity, "/dev/%s", name->getCStringNoCopy());
        status = SWITCH_BLOCK_OK;
        break;
    }
    it->release();
    return status;
}

// Whether a BSD device is media below our block device with the given UUID (or any).
bool ownsDevice(dev_t device, const char *uuid)
{
    if (device == NODEV || !gOwner || !gOwner->device)
        return false;
    OSDictionary *match = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(2);
    const OSNumber *majorNumber = OSNumber::withNumber(major(device), 32);
    const OSNumber *minorNumber = OSNumber::withNumber(minor(device), 32);
    const bool ok =
        (match != nullptr) && (properties != nullptr) && (majorNumber != nullptr) && (minorNumber != nullptr);
    if (ok) {
        properties->setObject(kIOBSDMajorKey, majorNumber);
        properties->setObject(kIOBSDMinorKey, minorNumber);
        match->setObject(gIOPropertyMatchKey, properties);
    }
    OSIterator *it = ok ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(majorNumber);
    OSSafeReleaseNULL(minorNumber);
    if (!it)
        return false;
    bool result = false;
    if (const auto *media = OSDynamicCast(IOService, it->getNextObject())) {
        const auto *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
        if (identity && (!uuid || !strcmp(identity->getCStringNoCopy(), uuid))) {
            const IORegistryEntry *entry = media->copyParentEntry(gIOServicePlane);
            for (unsigned depth = 0; entry && depth < 32 && !result; ++depth) {
                result = entry == gOwner->device;
                const IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
                entry->release();
                entry = parent;
            }
            OSSafeReleaseNULL(entry);
        }
    }
    it->release();
    return result;
}
}

extern "C" int switch_raw_volume_status(const char *uuid, dev_t *device, char *path, size_t capacity, uint64_t *bytes)
{
    if (!uuid || !device || !path || !bytes || capacity < 6)
        return SWITCH_BLOCK_INVALID;
    *bytes = 0;
    const int status = findVolume(uuid, device, path, capacity);
    if (!status)
        *bytes = gOwner->sectorCount * kSector;
    return status;
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t capacity)
{
    if (!uuid || !device || !path || capacity < 6)
        return 0;
    // Only the OS root is reported here; other volumes use the raw query.
    if (strcmp(uuid, kRootUUID)) {
        *device = NODEV;
        path[0] = 0;
        return 0;
    }
    return static_cast<int>(findVolume(uuid, device, path, capacity) == SWITCH_BLOCK_OK);
}

extern "C" int native_verified_block_root_authorized(dev_t device)
{
    return static_cast<int>(ownsDevice(device, kRootUUID));
}

extern "C" int native_verified_block_os_root_authorized(dev_t actual, dev_t selected)
{
    return static_cast<int>(actual != NODEV && actual == selected && ownsDevice(actual, kRootUUID));
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    if (!digest)
        return 0;
    bzero(digest, 48);
    if (!uuid || strcmp(uuid, kRootUUID) || !ownsDevice(device, kRootUUID))
        return 0;
    memcpy(digest, kRootIdentifier, sizeof(kRootIdentifier));
    return 1;
}

extern "C" volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
extern "C" void switch_print_blocked_threads(unsigned max_threads);
extern "C" void switch_print_processors(const char *who);
extern "C" void switch_iokit_jobs(int *outstanding, int *threads, int *waiting, int *max_threads);

namespace {
// The service-plane subtree under the block device: class, name, busy count and
// IOService state bits (2 registered, 4 matched, 8 first published, 0x10 first matched).
// NOLINTNEXTLINE(misc-no-recursion): the service plane below the device is a few levels deep; *budget bounds it
void printServiceTree(const IORegistryEntry *entry, unsigned depth, unsigned *budget)
{
    if (!*budget)
        return;
    --*budget;
    auto *service = OSDynamicCast(IOService, entry);
    IOLog("SwitchSD: %*s%s %s busy=%u state=0x%x\n", static_cast<int>(depth * 2), "",
          entry->getMetaClass()->getClassName(), entry->getName(), service ? service->getBusyState() : 0,
          service ? static_cast<unsigned>(service->getState()) : 0);
    if (OSIterator *children = entry->getChildIterator(gIOServicePlane)) {
        while (const auto *child = OSDynamicCast(IORegistryEntry, children->getNextObject()))
            printServiceTree(child, depth + 1, budget);
        children->release();
    }
}
}

extern "C" void switch_raw_root_wait_diagnostic(void)
{
    IOLog("SwitchSD: per-core timer/IPI interrupts %u/%u %u/%u %u/%u %u/%u\n", armcpu_core_timer[0], armcpu_core_sgi[0],
          armcpu_core_timer[1], armcpu_core_sgi[1], armcpu_core_timer[2], armcpu_core_sgi[2], armcpu_core_timer[3],
          armcpu_core_sgi[3]);
    if (!gOwner) {
        IOLog("SwitchSD: not published (status %d)\n", gPublishStatus);
        return;
    }
    gOwner->reportState("root wait timed out");
    if (gOwner->device) {
        unsigned budget = 14;
        printServiceTree(gOwner->device, 0, &budget);
    }
    int outstanding = 0;
    int threads = 0;
    int waiting = 0;
    int maxThreads = 0;
    switch_iokit_jobs(&outstanding, &threads, &waiting, &maxThreads);
    IOLog("SwitchSD: IOKit matching jobs outstanding=%d config threads=%d waiting=%d max=%d\n", outstanding, threads,
          waiting, maxThreads);
    switch_print_processors("SwitchSD");
    switch_print_blocked_threads(40);
}

// FAT-profile entry points: the FAT image path is gone.
extern "C" int switch_data_volume_status(const char *, dev_t *device, char *path, size_t capacity, uint64_t *bytes)
{
    if (device)
        *device = NODEV;
    if (path && capacity)
        path[0] = 0;
    if (bytes)
        *bytes = 0;
    return SWITCH_BLOCK_INVALID;
}

extern "C" int switch_data_volume_path(const char *uuid, dev_t *device, char *path, size_t capacity, uint64_t *bytes)
{
    return static_cast<int>(switch_data_volume_status(uuid, device, path, capacity, bytes) == SWITCH_BLOCK_OK);
}

extern "C" int switch_block_volume_path(unsigned, char *path, unsigned capacity)
{
    if (path && capacity)
        path[0] = 0;
    return SWITCH_BLOCK_NOT_READY;
}
