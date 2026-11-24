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
// one physically contiguous bounce buffer for what ADMA2 cannot take (and for everything on a
// controller without ADMA2), CMD18/CMD25 with Auto CMD12 or Auto CMD23, with
// queued requests that continue each other on the card merged into one command;
// completion is signalled by the SDMMC1 interrupt (an interrupt that cannot be had or
// never arrives panics) on a
// dedicated worker thread so callers never block or recurse.
#include <IOKit/IOBSD.h>
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
#if DEVELOPMENT || DEBUG
#include <machine/cpu_number.h>
#include "../TegraSMMU/TegraSMMU.h"
#endif
#include <sys/types.h>
#include <sys/param.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraI2C/TegraI2C.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
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
#include "SwitchSdPool.h"
#include "SwitchSdBus.h"
#include "SwitchSdCard.h"
#include "SwitchSdTrim.h"
#if DEVELOPMENT || DEBUG
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "SwitchSdIo.h"
#endif

extern "C" {
// The ADMA2 path's page checks and cache maintenance (osfmk/arm/pmap/pmap.c, arm64/arm_vm_init.c): a page's cache
// attributes (VM_WIMG_DEFAULT 0x2, osfmk/arm/memory_types.h: write-back cacheable) and its address in the kernel's
// physical aperture.
unsigned int pmap_cache_attributes(ppnum_t pn);
extern uint64_t gPhysBase, gPhysSize;  // the RAM the kernel's physical aperture covers
unsigned panic_active(void);
}

namespace {
constexpr uint64_t kControllerPA = 0x700B0000;  // SDMMC1
constexpr uint64_t kMemoryControllerPA = 0x70019000;
// What the SDR104 bring-up touches besides the controller (hekate bdk/soc/t210.h:47/58/64): the
// clock and reset controller, APB_MISC (pinmux at +0x3000) and the PMC's page (PMC is at 0x7000E400).
constexpr uint64_t kCarPA = 0x60006000, kApbMiscPA = 0x70000000;
// SDMMC1's interrupt: GIC_SPI 14, level-high (mainline tegra210.dtsi mmc@700b0000; Atmosphere uses 46 too).
constexpr uint32_t kInterrupt = 46;
// Longest single interrupt wait before the worker checks STATUS for a lost interrupt (see command()).
constexpr uint64_t kInterruptCheckNs = 100000000;
// The longest a write's programming or an R1b's busy may hold DAT0 once its own transfer complete came (awaitRelease).
constexpr uint64_t kReleaseLimitNs = 2000000000ULL;
constexpr uint32_t kSector = 512;
// r8-sdboot: one physically contiguous 1 MiB bounce buffer, aligned to the 512 KiB SDMA boundary. A transfer of more
// than 512 KiB pauses at each boundary with the DMA interrupt and resumes when the next address is written (command(),
// as Linux sdhci_data_irq does). SDMA commands on the bounce path stay at kSafeSectors (256 KiB, never reaching a
// boundary); only a controller without ADMA2 has nothing else, and none of ours lacks it.
constexpr uint32_t kBounceBytes = 1024 * 1024;
constexpr uint32_t kBounceAlign = 512 * 1024;
constexpr uint32_t kSdmaBoundary = 512 * 1024;
constexpr uint32_t kBounceSectors = kBounceBytes / kSector;
constexpr uint32_t kSafeSectors = 256 * 1024 / kSector;
// Most requests one merged command carries (APFS issues >= 4 KiB, so 256 still fills the buffer).
constexpr uint32_t kMaxChain = 256;
constexpr uint32_t kRequestPool = 1024;
constexpr size_t kCompleteChunk = 16;
constexpr uint64_t kPoolRefusal = switch_refusal_key("SwitchSD", "request pool");
// r12-sdio: how many queued requests a read chain looks through for reads that continue it (SwitchQueue.h): as many as
// one chain may hold, so it never takes fewer than the old rule did from a queue of reads right behind each other.
constexpr unsigned kMergeScan = kMaxChain;
// r10-adma2: with ADMA2, reads and writes go up to 1 MiB per command (ADMA2 has no SDMA boundary to resume).
constexpr uint32_t kDirectWriteSectors = kBounceSectors;
// r14-sdcq: multi-block ADMA2 commands announce their length with Auto CMD23 (SET_BLOCK_COUNT) instead of ending with
// Auto CMD12 when the card's SCR lists CMD23.
// r14-sdcq: a read chain also takes queued reads that start up to kGapSectors past its end (or end that far before its
// start), and the card reads straight through the gap into a sink nobody reads (SwitchQueue.h). On r7r a command cost
// ~0.58 ms plus ~10 us per KiB moved (r12-sdio's model, 4 KiB reads at 0.62 ms): a 32 KiB gap is ~0.32 ms of transfer
// against ~0.58 ms for the separate command it saves, so even the longest gap taken saves ~0.26 ms; kGapBudget keeps one
// command's read-through to 64 KiB (~0.64 ms, against ~1.16 ms for the two commands its two longest gaps save).
constexpr uint32_t kGapSectors = 32 * 1024 / kSector, kGapBudget = 64 * 1024 / kSector;

// controlCommand()'s data: none, or one 512-byte block to the card.
enum : int {
    kRegNone,
    kRegWrite
};

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
    R1_ERRORS = kSdR1Errors,
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
    PMC_PWR_DET_VAL = 0xe4,     // kits/SwitchCAR's PMC offsets
    PMC_SDMMC1_3V3 = 1u << 12,
    PMC_NO_IOPOWER = 0x44,
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
    // r6-lows: the request's IOStoragePriority class (IOStorageAttributes priority >> 6: 0 high (I/O tier 0, the UI's page-ins),
    // 1 default, 2 low, 3 background).
    uint8_t klass;
    bool write;
    bool chained;  // r12-sdio: taken into the chain being picked (SwitchQueue.h), under queueLock
    bool pooled;   // owned by requestPool slab (SwitchSdPool.h); false for overflow IOMalloc nodes
    IOStorageCompletion completion;
    uint64_t queuedNs;
};

static_assert(sizeof(Request) <= 64, "Request fits in one cache line");

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
    bool write, direct, issued, staged;
    bool cmd23;  // r14-sdcq: framed with Auto CMD23 (a multi-block ADMA2 command while that is on for its direction)
    uint32_t gapSectors;  // r14-sdcq: of `sectors`, those in gaps between the requests (read and discarded)
    SwitchAdmaRefusal why;
    int issueStep;                // issue()'s result when the command never reached the card
    uint64_t beganNs, overlapNs;  // when beginBatch started; the previous chain's completions that ran inside this one
#if DEVELOPMENT || DEBUG
    uint64_t oldestQueuedNs, issuedNs;
    bool wokeFromIdle;
#endif
    IOReturn result;  // endBatch's, for every request of the chain
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
    // through the bounce buffer by why (SwitchAdmaRefusal; kAdmaOk: ADMA2 off), ADMA errors, worker time on the ADMA2
    // path's cache maintenance (part of busyNs), and busy time per direction.
    uint64_t direct, descriptors, bounced[kAdmaRefusals], admaErrors, syncNs, readNs, writeNs;
    // r12-sdio: commands issued before the previous chain's completions ran, and of those the ones already finished when
    // the worker came back to them (no interrupt taken). Read chains that went by ADMA2 with some segments staged through
    // the bounce buffer, and those bytes.
    uint64_t overlapped, early, stagedChains, stagedBytes;
    // r14-sdcq: data commands framed with Auto CMD23; read chains with gaps and the gaps' bytes read through and
    // discarded.
    uint64_t cmd23, gapChains, gapBytes;
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
namespace {
// The request accounting (SwitchSdIo.h) of the one SD controller: since boot, the 10 s line's copy of it, and what the
// last line was formatted against. Outside the service, whose size IOKit caps at 16 KiB.
SwitchSdIo gIo, gIoNow, gIoLine;
}  // namespace

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
    // already durable. Nothing turns the card's cache on, and setup() turns off one found on before any write
    // (disableFoundCache).
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
    volatile uint8_t *car = nullptr, *apb = nullptr;  // SDR104 bring-up only
    volatile uint8_t *mc = nullptr;                   // memory controller (SMMU state for SDMMC1)
    switch_dma_buffer bounceMemory = {};
    uint8_t *bounce = nullptr;
    uint64_t bouncePA = 0;
    IOLock *queueLock = nullptr;
    Request *requestSlab = nullptr;
    SwitchSdPool<Request> requestPool = {};
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
    // r12-sdio: reads and contiguous writes merge anywhere in the queue (SwitchQueue.h) until a chain fails its check.
    bool mergeAnywhere = true;
    // Requests' bytes through IOMemoryDescriptor::readBytes/writeBytes, with the clean+invalidate of every destination
    // line they add on this kernel (SwitchClientCopy.h), for A/B builds; it keeps ADMA2 off too, so every request takes
    // the bounce buffer as before r10-adma2.
    static constexpr bool copyFlush = false;
    // r10-adma2: the ADMA2 descriptor table (one, rebuilt for every ADMA2 command; 16 KiB contiguous below 4 GiB), whether
    // ADMA2 carries data (the controller has it and no ADMA error since), and the state of the last ADMA error
    // (command()).
    switch_dma_buffer tableMemory = {};
    SwitchAdmaTable table = {};
    uint64_t tablePA = 0;
    bool tableUncached = false;  // the table reaches a non-cacheable page (dmaPage), which syncLines leaves alone
    bool adma = false;
    uint8_t admaErrorState = 0;
    uint64_t admaErrorAddress = 0;
    // r12-sdio: a read chain's segments that may not go straight to the card go through the bounce buffer alone
    // (SwitchAdmaStaging).
    Staging staging = {};
    Staging::Stage *stages = nullptr;
    // r14-sdcq: multi-block ADMA2 commands framed by Auto CMD23 (the card's SCR lists CMD23).
    bool autoCmd23 = false;
    // r14-sdcq: read chains with gaps (SwitchQueue.h) while gapsOn: the sink their gaps are read into (kGapSectors,
    // contiguous below 4 GiB, never read or written by the CPU after setup).
    switch_dma_buffer sinkMemory = {};
    uint64_t sinkPA = 0;
    bool gapsOn = false;
    // The interrupt status the last controlCommand() ended with, for the discard and erase refusal lines.
    uint32_t cardLastIrq = 0;
    // SDMMC1 interrupt: the controller's line is (STATUS & SIGNAL_ENABLE). The worker enables the signals it
    // waits for and sleeps on irqEvent; the filter disables them again (lowering the line) and wakes it.
    IOWorkLoop *irqLoop = nullptr;
    IOFilterInterruptEventSource *irqSource = nullptr;
    int irqEvent = 0;
#if DEVELOPMENT || DEBUG
    struct {
        uint64_t deadline, returned, filtersBefore, filtersAfter;
        uint32_t status, signals, interruptsBefore, interruptsAfter;
        int result, cpu;
        char controller[192];
    } waitFailure = {};
#endif
    SwitchSdBus bus;
    // Bus state: what the card runs at, its RCA after our re-initialization, the last STATUS a bring-up
    // command saw, and whether SDR104 tuning is in effect (a failed transfer is re-tuned before its retry)
    // at which kSpeeds step.
    const char *busMode = "inherited";
    uint32_t busKHz = 0;
    uint32_t rca = 0, initStatus = 0;
    bool tuned = false;
    unsigned speed = 0;
    // System sleep and shutdown: the worker stops between commands while suspended; the card was in 1.8 V signalling
    // at boot (so it is taken back there after SC7); LBA 1 as read at boot, compared after re-initialization.
    bool suspended = false, working = false, lowVoltageBus = false, shutdownDone = false;
    bool inShutdown = false, shutdownIrqLost = false;
    uint8_t gptHeader[kSector] = {};
    // r14-sdcq: the card's registers as identify() read them once at boot (CID and CSD during reinitialize(), the rest in
    // transfer state), for the identification lines, CMD23 and the card's cache (disableFoundCache).
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
    int issue(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, bool cmd23 = false);
    int complete(uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool *early, bool cmd23 = false);
    bool failed(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write, unsigned descriptors,
                bool retry);
    int run(uint64_t lba, uint32_t sectors, bool write, unsigned descriptors);
    int retry(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write, unsigned descriptors);
    static void worker(void *, wait_result_t);
    void enqueueLocked(Request *request);
    Request *drainQueueLocked();
    void releaseRequestPool();
    Request *pickLocked(uint32_t *sectors, uint32_t *count, const SwitchLbaRange *inFlight = nullptr,
                        unsigned inFlightCount = 0);
    void takenLocked(Request *first, uint32_t count, uint64_t pickNs);
    void beginBatch(Batch &);
    void endBatch(Batch &);
    void account(Batch &);
    void completeBatch(Batch &);
    int controlCommand(uint32_t index, uint32_t argument, uint32_t flags, uint32_t *response, int data,
                       uint64_t limitNs = 1500000000ULL, bool allowRelease = true);
    bool stepUnmap(Unmap &job);
    int erase(uint64_t first, uint64_t last, uint32_t argument, uint64_t limitNs);
    bool trimRunnable() const;
    uint64_t trimQuietUntil() const;
    void trimStep();
    static void saveTrimCursor(uint64_t cursor);
    static void trimFired(thread_call_param_t owner, thread_call_param_t);
    // r10-adma2: the ADMA2 path, the bounce path in SDMA-sized pieces, the setup, the power-state hooks.
    SwitchAdmaRefusal buildTable(Request *first, uint32_t sectors, bool write);
    void prepareDirect(bool write);
    void finishDirect(bool write);
    bool stageChain(Request *first, uint32_t from, uint32_t length, uint8_t *buffer, bool toClient);
    bool bounceChain(Request *first, uint32_t sectors);
    bool copyStaged();
    const char *setupAdma();
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
    void disableFoundCache();
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

    struct ExtRegBus {
        NativeSwitchBlock *self;
        bool allowRelease;

        int writeByte(uint8_t fno, uint32_t address, uint8_t value, uint32_t *r1) const;
        int readBytes(uint8_t fno, uint32_t address, uint32_t length, uint8_t *out, uint32_t *r1) const;

        static uint64_t nowNs()
        {
            return switch_now_ns();
        }

        static void sleepMs(uint32_t ms)
        {
            IOSleep(ms);
        }
    };

public:
    using IOService::callPlatformFunction;
    IOReturn callPlatformFunction(const OSSymbol *function, bool wait, void *p1, void *p2, void *p3, void *p4) override;
    void stop(IOService *provider) override;
    void free() override;
    void suspend();
    void resume();
    void shutdown();
    Stats stats = {};
#if DEVELOPMENT || DEBUG
    // When the last 10 s line of the request accounting (gIo) was made, and when the driver began and finished its
    // setup (kern.switch_sd's setup_us).
    uint64_t ioLineNs = 0, setupBeganNs = 0, setupEndedNs = 0;
    uint64_t lastIssuedNs = 0, lastIrqNs = 0, lastWakeNs = 0, lastCmdIrqNs = 0, lastCmdWakeNs = 0, prevIrqNs = 0;
    void logIo(uint64_t now);
    size_t formatTotals(char *out, size_t size) const;
#endif
    uint64_t firstSector = 0, sectorCount = 0;
    // Per request and per merged command, as advertised: reads kBounceSectors with ADMA2 or once setup() has checked
    // SDMA boundary resumes, else kSafeSectors; writes kDirectWriteSectors with ADMA2, else kSafeSectors.
    uint32_t maxSectors = kSafeSectors, maxWriteSectors = kSafeSectors;
    NativeSwitchBlockDevice *device = nullptr;

    bool setup();
    int transfer(uint64_t lba, uint32_t sectors, bool write, uint8_t *bytes);
    IOReturn submit(const Request &);
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
    // No attributes: the default class, as IOStorage's own default priority. The boot prefetch: high.
    IOStoragePriority priority = attributes ? attributes->priority : kIOStoragePriorityDefault;
#if DEVELOPMENT || DEBUG
    switchSdAdd(&gIo.asked[priority >> 6], 1);
#endif
    if (current_thread() == __atomic_load_n(&gPrefetchThread, __ATOMIC_ACQUIRE)) {
#if DEVELOPMENT || DEBUG
        if (priority >> 6)
            switchSdAdd(&gIo.chosen, 1);
#endif
        priority = kIOStoragePriorityHigh;
    }
    const Request spec = {nullptr,
                          buffer,
                          owner->firstSector + block,
                          static_cast<uint32_t>(count),
                          static_cast<uint8_t>(priority >> 6),
                          direction == kIODirectionOut,
                          false,
                          false,
                          *completion,
                          0};
    buffer->retain();
    const IOReturn status = owner->submit(spec);
    if (status != kIOReturnSuccess) {
        buffer->release();
        return status;
    }
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
#if DEVELOPMENT || DEBUG
    __atomic_store_n(&self->lastIrqNs, switch_now_ns(), __ATOMIC_RELEASE);
#endif
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
    const uint64_t deadline = mach_absolute_time() + interval;
#if DEVELOPMENT || DEBUG
    const uint32_t interruptsBefore = switch_interrupt_count(kInterrupt);
    const uint64_t filtersBefore = __atomic_load_n(&stats.interrupts, __ATOMIC_RELAXED);
    // Clear before assert_wait_deadline and publish before the Device-nGnRE SIGNAL_ENABLE write:
    // on ARMv8 a plain Normal store is not ordered before a subsequent Device store without a barrier.
    __atomic_store_n(&lastIrqNs, 0, __ATOMIC_RELAXED);
#endif
    assert_wait_deadline(&irqEvent, THREAD_UNINT, deadline);
#if DEVELOPMENT || DEBUG
    switch_mmio_write32_publish(regs, SIGNAL_ENABLE, signals);
#else
    w32(SIGNAL_ENABLE, signals);
#endif
    const wait_result_t result = thread_block(THREAD_CONTINUE_NULL);
#if DEVELOPMENT || DEBUG
    const uint64_t wakeNs = switch_now_ns();
    if (result != THREAD_AWAKENED) {
        waitFailure.status = r32(STATUS);
        waitFailure.signals = r32(SIGNAL_ENABLE);
        switch_interrupt_describe(kInterrupt, waitFailure.controller, sizeof waitFailure.controller);
        waitFailure.returned = mach_absolute_time();
        waitFailure.deadline = deadline;
        waitFailure.interruptsBefore = interruptsBefore;
        waitFailure.interruptsAfter = switch_interrupt_count(kInterrupt);
        waitFailure.filtersBefore = filtersBefore;
        waitFailure.filtersAfter = __atomic_load_n(&stats.interrupts, __ATOMIC_RELAXED);
        waitFailure.result = result;
        waitFailure.cpu = cpu_number();
    }
#endif
    w32(SIGNAL_ENABLE, 0);
#if DEVELOPMENT || DEBUG
    if (result == THREAD_AWAKENED)
        lastWakeNs = wakeNs;
#endif
    return result == THREAD_AWAKENED;
}

// A wait that ran out with an awaited bit latched in STATUS: the controller raised it, the CPU never took the interrupt.
void NativeSwitchBlock::interruptLost(uint32_t awaited)
{
    char gic[192];
    switch_interrupt_describe(kInterrupt, gic, sizeof(gic));
#if DEVELOPMENT || DEBUG
    uint32_t mcInterrupts = 0, mcStatus = 0, mcAddress = 0;
    const uint32_t mcCount = tegra_mc_error_latest(&mcInterrupts, &mcStatus, &mcAddress);
    panic("SwitchSD: SDMMC1 interrupt lost: STATUS %#x awaited %#x; wait %d CPU %d deadline %#llx returned %#llx "
          "dispatch %u->%u filter %llu->%llu before-mask %#x/%#x [%s]; after [%s]; MC %u intstatus %#x status %#x "
          "address %#x",
          r32(STATUS), awaited, waitFailure.result, waitFailure.cpu, waitFailure.deadline, waitFailure.returned,
          waitFailure.interruptsBefore, waitFailure.interruptsAfter, waitFailure.filtersBefore,
          waitFailure.filtersAfter, waitFailure.status, waitFailure.signals, waitFailure.controller, gic, mcCount,
          mcInterrupts, mcStatus, mcAddress);
#else
    panic("SwitchSD: SDMMC1 interrupt lost: STATUS %#x awaited %#x, %s", r32(STATUS), awaited, gic);
#endif
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
        if (!waitSignal(signals, left < kInterruptCheckNs ? left : kInterruptCheckNs) && (r32(STATUS) & signals)) {
            if (inShutdown) {
                const uint32_t lost = r32(STATUS);
                if (!shutdownIrqLost) {
                    shutdownIrqLost = true;
                    char gic[192];
                    switch_interrupt_describe(kInterrupt, gic, sizeof(gic));
                    IOLog("SwitchSD: SDMMC1 interrupt lost during shutdown: STATUS %#x awaited %#x, %s\n", lost,
                          signals, gic);
                }
                return lost;
            }
            interruptLost(signals);
        }
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
int NativeSwitchBlock::issue(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, bool cmd23)
{
    // Every card write passes here, whichever path staged it: none may reach outside the APFS partition. (Until setup()
    // has found the partition, sectorCount is 0 and so no write can be issued at all.)
    if (write && (lba < firstSector || sectors > sectorCount || lba - firstSector > sectorCount - sectors)) {
        panic("SwitchSD: write lba=%u n=%u outside the APFS partition [%llu, +%llu)", lba, sectors, firstSector,
              sectorCount);
    }
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
    w32(ARGUMENT, lba);
    // DMA | block-count | Auto CMD12 (or r14-sdcq: Auto CMD23, its block count in Argument 2) | read | multi, as the
    // qualified sequence.
    const SwitchSdFrame frame = switchSdBlockFrame(sectors, write, cmd23);
    if (frame.argument2)
        w32(ARGUMENT2, frame.argument2);
    w16(TRANSFER, frame.transfer);
    switch_dcache_sync();
#if DEVELOPMENT || DEBUG
    lastIssuedNs = switch_now_ns();
#endif
    w16(COMMAND, frame.command);
    return 0;
}

// The command's second half, for the command issue() started: 0 or a nonzero step code. Its time limit runs from here,
// not from issue(): the worker may have completed the previous chain's requests in between. `*early` (when asked): the
// command had already finished when this began, so it took no interrupt.
int NativeSwitchBlock::complete(uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool *early,
                                bool cmd23)
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
#if DEVELOPMENT || DEBUG
    lastCmdIrqNs = waited ? __atomic_load_n(&lastIrqNs, __ATOMIC_ACQUIRE) : 0;
    lastCmdWakeNs = waited ? lastWakeNs : 0;
#endif
    ended(s);
    w32(STATUS, INT_COMMAND | INT_TRANSFER);
    if (!(s & INT_COMMAND) || (r32(RESPONSE) & R1_ERRORS))
        return 4;
    if (multi) {
        // Auto CMD12's or (r14-sdcq) Auto CMD23's errors; only a CMD12 leaves a response in STOP_RESPONSE.
        if (r16(AUTO_CMD_ERROR) & 0x9f)
            return 8;
        if (!cmd23 && (r32(STOP_RESPONSE) & R1_STOP_ERRORS))
            return 9;
    }
    // A write's transfer complete follows the card's busy release, which DAT0 confirms (ended()).
#if DEVELOPMENT || DEBUG
    if (!bus.idle())
        lastCmdIrqNs = 0;
#endif
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

void NativeSwitchBlock::enqueueLocked(Request *request)
{
    if (request->queuedNs > lastIoNs)
        lastIoNs = request->queuedNs;
    if (request->write)
        switchSdTrimClear(&trim, request->lba, request->sectors);
    if (tail) {
        tail->next = request;
    } else {
        head = request;
    }
    tail = request;
#if DEVELOPMENT || DEBUG
    gIo.depthChanging(request->queuedNs, queued);
#endif
    if (++queued > stats.maxQueued)
        stats.maxQueued = queued;
    if (!request->write)
        ++queuedReads[request->klass];
    if (!stats.firstNs)
        stats.firstNs = statsLastNs = request->queuedNs;
    __atomic_fetch_add(&submitted, 1, __ATOMIC_RELAXED);
    IOLockWakeup(queueLock, &head, true);
}

IOReturn NativeSwitchBlock::submit(const Request &spec)
{
    struct Ops {
        NativeSwitchBlock *self;

        void lock() const
        {
            IOLockLock(self->queueLock);
        }

        void unlock() const
        {
            IOLockUnlock(self->queueLock);
        }

        bool isShutdown() const
        {
            return self->shutdownDone;
        }

        static uint64_t nowNs()
        {
            return switch_now_ns();
        }

        static void onExhausted(uint32_t capacity)
        {
            switch_refusal_k(kPoolRefusal, "SwitchSD", "request pool",
                             "request pool of %u entries exhausted; allocating from heap", capacity);
        }

        static Request *allocHeap()
        {
            return static_cast<Request *>(IOMalloc(sizeof(Request)));
        }

        static void freeHeap(Request *request)
        {
            IOFree(request, sizeof(*request));
        }

        void enqueueLocked(Request *request) const
        {
            self->enqueueLocked(request);
        }
    };

    const SwitchSdPoolSubmitResult status = switchSdPoolSubmit(requestPool, spec, Ops{this});
    if (status == SwitchSdPoolSubmitResult::kQueued)
        return kIOReturnSuccess;
    return status == SwitchSdPoolSubmitResult::kNoMemory ? kIOReturnNoMemory : kIOReturnNotReady;
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
        if (shutdownDone) {
            IOLockUnlock(queueLock);
            return kIOReturnNotReady;
        }
        for (uint32_t i = 0; i < count; ++i)
            switchSdTrimMark(&trim, firstSector + extents[i].blockStart, extents[i].blockCount);
        IOLockWakeup(queueLock, &head, true);
        IOLockUnlock(queueLock);
        return kIOReturnSuccess;
    }
    Unmap job = {nullptr, extents, count, 0, 0, 0, kIOReturnSuccess, false};
    IOLockLock(queueLock);
    if (shutdownDone) {
        IOLockUnlock(queueLock);
        return kIOReturnNotReady;
    }
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
    int step = controlCommand(32, static_cast<uint32_t>(first), RSP_R1, &r1, kRegNone);
    if (!step && !(r1 & R1_ERRORS))
        step = controlCommand(33, static_cast<uint32_t>(last), RSP_R1, &r1, kRegNone);
    if (!step && !(r1 & R1_ERRORS))
        step = controlCommand(38, argument, RSP_R1B, &r1, kRegNone, limitNs);
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
// after it: drops what the A57 prefetched meanwhile). The table only holds whole lines of managed RAM (SwitchAdma.h), so no other data shares a line, the card overwrites every
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
// buffer instead.
SwitchAdmaRefusal NativeSwitchBlock::buildTable(Request *first, uint32_t sectors, bool write)
{
    table.reset();
    tableUncached = false;
    staging.count = 0;
    table.limit = kAdmaDmaLimit;
    const auto pageOk = [&](uint64_t page) {
        const DmaPage kind = dmaPage(page);
        if (kind == DmaPage::kUncached)
            tableUncached = true;
        return kind != DmaPage::kRefused;
    };
    // r14-sdcq: a read chain's gaps go to the sink, one descriptor each (no gap is longer than the sink).
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): a chain always has its first request
    uint64_t lba = first->lba;
    const auto gap = [&](const Request *r) -> uint64_t {
        return (r->lba - lba) * kSector;
    };
    if (!write && stages) {
        // r12-sdio: a read's refused segments alone through the bounce buffer (SwitchAdmaStaging).
        staging.reset(&table, bouncePA, stages, kStages);
        table.limit = kAdmaDmaLimit;
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
// written back.
void NativeSwitchBlock::prepareDirect(bool write)
{
    const uint64_t began = switch_now_ns();
    switch_dcache_clean_lines(table.entries, table.count * sizeof(SwitchAdmaDescriptor));
    if (write) {
        syncLines(SWITCH_DCACHE_CLEAN, table, tableUncached);
    } else {
        syncLines(SWITCH_DCACHE_INVALIDATE, table, tableUncached, sinkPA,
                  sinkPA ? static_cast<uint64_t>(kGapSectors) * kSector : 0);
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
#if DEVELOPMENT || DEBUG
        switchSdAdd(&gIo.bounceBytes, stageAt.length);
#endif
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

// The chain's bytes [from, from + length) between the requests' memory and the start of `buffer` (the bounce buffer).
// r14-sdcq: toward the requests a read chain's gaps are skipped; from the requests (a write) every byte must come from
// one (switchChainCopy).
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
#if DEVELOPMENT || DEBUG
    switchSdAdd(&gIo.bounceBytes, length);
#endif
    return ok;
}

// The chain through the bounce buffer by SDMA, in commands of kSafeSectors (a chain merged for ADMA2 can be longer), each
// piece staged before or copied out after its command.
bool NativeSwitchBlock::bounceChain(Request *first, uint32_t sectors)
{
    const bool write = first->write;
    for (uint32_t done = 0; done < sectors;) {
        const uint32_t n = sectors - done < kSafeSectors ? sectors - done : kSafeSectors;
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
    // puts on the card comes from its requests (switchGatherWrites tiles with zero gaps).
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
    // r10-adma2: straight between the card and the requests' pages by ADMA2 when it is on and the whole chain qualifies;
    // through the bounce buffer otherwise, counted by why.
    b.why = kAdmaOk;
    b.direct = adma && (b.why = buildTable(first, sectors, b.write)) == kAdmaOk;
    b.staged = b.direct && (staging.count != 0u);
    b.cmd23 = b.direct && sectors > 1 && autoCmd23;
    if (!b.direct) {
#if DEVELOPMENT || DEBUG
        prevIrqNs = 0;
#endif
        return;
    }
    prepareDirect(b.write);
    b.issueStep = issue(static_cast<uint32_t>(first->lba), sectors, b.write, table.count, b.cmd23);
    b.issued = true;
#if DEVELOPMENT || DEBUG
    if (!b.issueStep) {
        b.issuedNs = lastIssuedNs;
        gIo.roundTrip[kSwitchSdRtPrep].add(b.issuedNs > b.beganNs ? b.issuedNs - b.beganNs : 0);
        if (b.wokeFromIdle)
            gIo.roundTrip[kSwitchSdRtSubmit].add(b.issuedNs > b.oldestQueuedNs ? b.issuedNs - b.oldestQueuedNs : 0);
        if (prevIrqNs)
            gIo.roundTrip[kSwitchSdRtGap].add(b.issuedNs > prevIrqNs ? b.issuedNs - prevIrqNs : 0);
    }
    prevIrqNs = 0;
#endif
}

// endBatch: the issued command waited for (the interrupt, unless it finished meanwhile), retried and checked; or the whole
// bounce path. The stats.
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
#if DEVELOPMENT || DEBUG
        if (!b.issueStep && !step && !early && lastCmdIrqNs) {
            gIo.roundTrip[kSwitchSdRtWire].add(lastCmdIrqNs > b.issuedNs ? lastCmdIrqNs - b.issuedNs : 0);
            gIo.roundTrip[kSwitchSdRtWake].add(lastCmdWakeNs > lastCmdIrqNs ? lastCmdWakeNs - lastCmdIrqNs : 0);
            prevIrqNs = lastCmdIrqNs;
        } else {
            prevIrqNs = 0;
        }
#endif
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
        // r12-sdio: staged segments out to their requests.
        if (!step && b.staged) {
            if (!copyStaged())
                result = kIOReturnIOError;
            ++stats.stagedChains;
            stats.stagedBytes += staging.staged;
        }
        if (step == kStepAdmaError) {
            // The host's DMA failed, not the card: this chain and every later one through the bounce buffer.
            adma = false;
            b.direct = false;
            IOLog("SwitchSD: ADMA2 off until reboot (step %d); requests go through the bounce buffer\n", step);
        } else if (step) {
            result = kIOReturnIOError;
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
    b.result = result;
    account(b);
}

// The stats for a chain whose command has ended. Card and copies first, completions after: busy time
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
#if DEVELOPMENT || DEBUG
    gIo.command[write ? 1 : 0][switchSdSize(static_cast<uint64_t>(b.sectors) * kSector)].add(latency);
#endif
    stats.merged += b.count - 1;
    if (latency > stats.maxLatencyNs)
        stats.maxLatencyNs = latency;
}

// completeBatch: every request of the chain completes with endBatch's result.
void NativeSwitchBlock::completeBatch(Batch &b)
{
    struct Done {
        IOMemoryDescriptor *buffer;
        IOStorageCompletion completion;
        uint32_t length;
    };

    struct Ops {
        NativeSwitchBlock *self;
        IOReturn result;

        void lock() const
        {
            IOLockLock(self->queueLock);
        }

        void unlock() const
        {
            IOLockUnlock(self->queueLock);
        }

        static Done capture(const Request &r)
        {
#if DEVELOPMENT || DEBUG
            const uint64_t now = switch_now_ns();
            gIo.total[r.klass].add(now > r.queuedNs ? now - r.queuedNs : 0);
#endif
            return {r.buffer, r.completion, r.sectors * kSector};
        }

        static void freeHeap(Request *r)
        {
            IOFree(r, sizeof(*r));
        }

        void finish(const Done &d) const
        {
            d.buffer->release();
            // IOStorage::complete, without binding another original IOStorageFamily symbol.
            if (d.completion.action)
                d.completion.action(d.completion.target, d.completion.parameter, result, result ? 0 : d.length);
            __atomic_fetch_add(&self->serviced, 1, __ATOMIC_RELAXED);
        }
    };

    const uint64_t began = switch_now_ns();
    Request *const first = b.first;
    b.first = nullptr;
    switchSdPoolComplete<kCompleteChunk, Request, Done>(requestPool, first, Ops{this, b.result});
    stats.completeNs += switch_now_ns() - began;
}

// A command outside the data path (the discard and erase sequence; setup's CMD49), waited for by interrupt as the data
// commands are (complete()): no data (CMD32/CMD33, R1; CMD38, R1b, whose busy end raises transfer complete on this
// controller, the hardware busy detection L4T relies on with MMC_CAP_WAIT_WHILE_BUSY), or one 512-byte block to the card
// by SDMA from the bounce buffer (CMD49, setup only: no data command is in flight then; transfer complete follows its
// busy). 0 or a step code (1 bus not free (claimBus), 2/3 an error interrupt without/with command complete, 6/7 no end
// within limitNs: CMD49's busy is at most 1 s, 5.7.2.2, a cache flush included, 4.17; 10 the card's DAT0 release after
// transfer complete failed (awaitRelease), as complete() checks a write); a failure resets the CMD and DAT lines.
// *response gets the R1, also on a failure when the command's response came (command complete seen: steps 3, 6 and 10);
// the interrupt status the command ended with is kept for the refusal lines (cardLastIrq).
int NativeSwitchBlock::controlCommand(uint32_t index, uint32_t argument, uint32_t flags, uint32_t *response, int data,
                                      uint64_t limitNs, bool allowRelease)
{
    *response = 0;
    SwitchSdUse use = SwitchSdUse::kCommand;
    if (data == kRegWrite) {
        use = SwitchSdUse::kWrite;
    } else if ((flags & 3) == 3) {
        use = SwitchSdUse::kBusy;
    }
    if (!claimBus(use)) {
        cardLastIrq = r32(PRESENT);
        return 1;
    }
    w32(STATUS, 0xffffffff);
    if (data != kRegNone) {
        switch_dcache_clean_invalidate_lines(bounce, kSector);
        switch_dcache_sync();
        w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);
        w32(DMA_LOW, static_cast<uint32_t>(bouncePA));
        w32(DMA_HIGH, static_cast<uint32_t>(bouncePA >> 32));
        w16(BLOCK_SIZE, kSector | 0x7000);
        w16(BLOCK_COUNT, 1);
    }
    w32(ARGUMENT, argument);
    uint16_t transfer = 0;
    if (data == kRegWrite)
        transfer = 0x01;
    w16(TRANSFER, transfer);
    switch_dcache_sync();
    w16(COMMAND, static_cast<uint16_t>(index << 8 | flags | (data != kRegNone ? CMD_DATA : 0)));
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
    // Transfer complete follows a CMD49's or an R1b's busy; DAT0 confirms it. RESPONSE is read first: CMD13 would
    // replace it.
    if (!step && !bus.idle() && (!allowRelease || awaitRelease()))
        step = 10;
    cardLastIrq = s;
    if (step) {
        // Not after a command error (timeout, CRC, end bit, index): this controller sets command complete with a timeout
        // too (irq 0x00018001) and RESPONSE still holds the previous command's.
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
    return 0;
}

// The next chain off the queue (queueLock held, queue not empty), unlinked: the head, or a passing read (r6-lows), and the
// queued requests that continue it on the card.
Request *NativeSwitchBlock::pickLocked(uint32_t *chainSectors, uint32_t *chainCount, const SwitchLbaRange *inFlight,
                                       unsigned inFlightCount)
{
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
    const uint32_t limit = first->write ? maxWriteSectors : maxSectors;
    bool gathered = false;
    if (merge && mergeAnywhere) {
        uint64_t start = 0;
        bool fits = false;
        if (first->write) {
            count = switchGatherWrites(head, first, limit, kMaxChain, kMergeScan, &start, &sectors, inFlight,
                                       inFlightCount);
            fits = switchWriteChainTiles(head, start, sectors, count);
        } else {
            const uint32_t maxGap = gapsOn ? kGapSectors : 0;
            const uint32_t gapBudget = gapsOn ? kGapBudget : 0;
            count =
                switchGatherReads(head, first, maxSectors, kMaxChain, kMergeScan, &start, &sectors, maxGap, gapBudget);
            fits = switchChainFits(head, start, sectors, count, maxGap, gapBudget);
        }
        if (fits) {
            first = switchUnlinkChain(head, tail, count);
            gathered = true;
        } else {
            switchClearChain(head);
            mergeAnywhere = false;
            IOLog("SwitchSD: %s chain lba=%llu n=%u (%u requests) does not fit; requests merge only with the requests "
                  "right behind them until reboot\n",
                  first->write ? "write" : "read", start, sectors, count);
            sectors = first->sectors;
            count = 1;
        }
    }
    if (!gathered) {
        Request *last = first;
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
        const auto overlapsInFlight = [&](const Request *later) {
            if (!first->write)
                return false;
            for (unsigned i = 0; i < inFlightCount; ++i) {
                if (switchOverlapsRange(later, inFlight[i]))
                    return true;
            }
            return false;
        };
        while (merge && count < kMaxChain && last->next && last->next->write == first->write &&
               last->next->lba == last->lba + last->sectors && sectors + last->next->sectors <= limit &&
               !overlapsOlder(last->next) && !overlapsInFlight(last->next)) {
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
#if DEVELOPMENT || DEBUG
    gIo.depthChanging(pickNs, queued);
#endif
    queued -= count;
    for (const Request *r = first; r; r = r->next) {
        if (!r->write)
            --queuedReads[r->klass];
        ClassWait &wait = waits[r->klass];
        const uint64_t waited = pickNs > r->queuedNs ? pickNs - r->queuedNs : 0;
#if DEVELOPMENT || DEBUG
        gIo.wait[r->klass].add(waited);
#endif
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
        // Idle: an erase-based trim once the queue has been quiet long enough, else a timer for when it will have been.
        bool trimNow = false;
#if DEVELOPMENT || DEBUG
        bool wokeFromIdle = false;
#endif
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
#if DEVELOPMENT || DEBUG
            wokeFromIdle = true;
            self->prevIrqNs = 0;
#endif
            IOLockSleep(self->queueLock, &self->head, THREAD_UNINT);
        }
        if (trimNow) {
#if DEVELOPMENT || DEBUG
            self->prevIrqNs = 0;
#endif
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
#if DEVELOPMENT || DEBUG
            self->prevIrqNs = 0;
#endif
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
        if (self->head && !self->suspended && !discardTurn) {
            if (self->unmaps)
                ++self->chainsSinceUnmap;
            next = &batches[turn];
            turn ^= 1;
            const SwitchLbaRange active = pending && pending->first
                                              ? SwitchLbaRange{pending->first->lba, pending->sectors}
                                              : SwitchLbaRange{0, 0};
            next->first = self->pickLocked(&next->sectors, &next->count, &active, pending && pending->first ? 1u : 0u);
#if DEVELOPMENT || DEBUG
            next->wokeFromIdle = wokeFromIdle;
            uint64_t oldest = next->first->queuedNs;
            for (const Request *r = next->first->next; r; r = r->next) {
                if (r->queuedNs < oldest)
                    oldest = r->queuedNs;
            }
            next->oldestQueuedNs = oldest;
#endif
            self->working = true;
        }
        IOLockUnlock(self->queueLock);
        // The next chain's command goes to the card first; the previous chain's completions run while it moves data. A
        // chain that is not one ADMA2 command (the bounce path) only has its table built here, so those completions are
        // not held behind its several commands.
        if (next)
            self->beginBatch(*next);
        if (pending) {
            const uint64_t began = switch_now_ns();
            self->completeBatch(*pending);
            if (next && next->issued)
                next->overlapNs = switch_now_ns() - began;
            pending = nullptr;
        }
        if (!next) {
            IOLockLock(self->queueLock);
            self->working = false;
            if (self->suspended)
                IOLockWakeup(self->queueLock, &self->working, false);
            IOLockUnlock(self->queueLock);
#if DEVELOPMENT || DEBUG
            self->prevIrqNs = 0;
#endif
            continue;
        }
        self->endBatch(*next);
        pending = next;
        IOLockLock(self->queueLock);
        const bool drain = self->suspended;
        IOLockUnlock(self->queueLock);
        if (drain) {
            self->completeBatch(*pending);
            pending = nullptr;
            IOLockLock(self->queueLock);
            self->working = false;
            IOLockWakeup(self->queueLock, &self->working, false);
            IOLockUnlock(self->queueLock);
#if DEVELOPMENT || DEBUG
            self->prevIrqNs = 0;
#endif
            continue;
        }
#if DEVELOPMENT || DEBUG
        // Throughput line every statsEveryNs while I/O is happening (one log line).
        {
            const uint64_t now = switch_now_ns();
            if (now - self->statsLastNs >= NativeSwitchBlock::statsEveryNs) {
                self->statsLastNs = now;
                self->prevIrqNs = 0;
                self->logThroughput();
                self->logIo(now);
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
    // off = ADMA2 off), ADMA2 descriptors per ADMA2 request, cache maintenance time (sync), and MB/s per direction over
    // each direction's own busy time. r12-sdio: of the ADMA2 read chains, those with
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
    char extras[160] = "";
#if DEVELOPMENT || DEBUG
    // SwitchSdBus.h: commands refused at their PRESENT_STATE read and DAT0 releases awaited by CMD13 (both 0 per the
    // spec).
    if (stats.busRefused || stats.releases) {
        const size_t at = strlen(extras);
        snprintf(extras + at, sizeof(extras) - at, " bus refused %llu dat0 %llu", stats.busRefused, stats.releases);
    }
    if (stats.uncachedChains) {
        const size_t at = strlen(extras);
        snprintf(extras + at, sizeof(extras) - at, " uncached %llu", stats.uncachedChains);
    }
#endif
    // Erase-based trim: erases, MiB freed and the card's busy ms for them (since boot), and AUs still due.
    if (stats.trimErases || trim.pending) {
        const size_t at = strlen(extras);
        snprintf(extras + at, sizeof(extras) - at, " trim %llu/%lluM/%llums due %llu", stats.trimErases,
                 stats.trimSectors >> 11, stats.trimNs / 1000000, trim.pending);
    }
    const uint64_t readRate = stats.readNs ? stats.readBytes * 10000 / stats.readNs : 0;
    const uint64_t writeRate = stats.writeNs ? stats.writeBytes * 10000 / stats.writeNs : 0;
    // Periodic: to the log only (os_log), never the boot console.
    char out[680];
    snprintf(
        out, sizeof(out),
        "SwitchSD: %llu rq %llu MiB avg %lluK | busy %llu.%llus copy %llums sync %llums %llu.%llu MB/s "
        "(rd %llu.%llu wr %llu.%llu) %llu%% of %llus | dma %llu bounce %llu%s seg %llu.%llu part %llu/%lluK wmax %uK | "
        "<=4K %llu%% seq %llu%% merged %llu gap %llu/%lluK q%u | cmds %llu c23 %llu irq %llu ovl %llu/%llu%s | "
        "err %llu adma %llu retune %llu | "
        "%s %u.%02u MHz | wait ms%s",
        requests, bytes >> 20, requests ? (bytes / requests) >> 10 : 0, busy / 1000000000, busy / 100000000 % 10,
        stats.copyNs / 1000000, stats.syncNs / 1000000, rate / 10, rate % 10, readRate / 10, readRate % 10,
        writeRate / 10, writeRate % 10, wall ? busy * 100 / wall : 0, wall / 1000000000, stats.direct, bounced, bounces,
        perRequest / 10, perRequest % 10, stats.stagedChains, stats.stagedBytes >> 10, maxWriteSectors * kSector / 1024,
        requests ? stats.small * 100 / requests : 0, requests ? stats.sequential * 100 / requests : 0, stats.merged,
        stats.gapChains, stats.gapBytes >> 10, stats.maxQueued, stats.commands, stats.cmd23,
        __atomic_load_n(&stats.interrupts, __ATOMIC_RELAXED), stats.early, stats.overlapped, extras, stats.errors,
        stats.admaErrors, stats.retunes, busMode, busKHz / 1000, busKHz % 1000 / 10, classes);
    os_log(OS_LOG_DEFAULT, "%s", out);
}

#if DEVELOPMENT || DEBUG
size_t NativeSwitchBlock::formatTotals(char *out, size_t size) const
{
    const size_t at = static_cast<size_t>(
        snprintf(out, size, "setup_us=%llu submitted=%llu serviced=%llu irq=%llu ",
                 (setupEndedNs - setupBeganNs) / 1000, __atomic_load_n(&submitted, __ATOMIC_RELAXED),
                 __atomic_load_n(&serviced, __ATOMIC_RELAXED), __atomic_load_n(&stats.interrupts, __ATOMIC_RELAXED)));
    return at < size ? at + switchSdFormatTotals(gIo, out + at, size - at) : size - 1;
}

// The request accounting's 10 s line (SwitchSdIo.h), against the counters the last one was formatted with. The queue's
// length is integrated up to now and its longest restarts, under queueLock as every change of it.
void NativeSwitchBlock::logIo(uint64_t now)
{
    IOLockLock(queueLock);
    gIo.depthChanging(now, queued);
    gIo.copyTo(gIoNow);
    __atomic_store_n(&gIo.depthMax, queued, __ATOMIC_RELAXED);
    IOLockUnlock(queueLock);
    char out[896];
    const uint64_t since = ioLineNs ? ioLineNs : stats.firstNs;
    switchSdFormatIo(gIoNow, gIoLine, since && now > since ? now - since : 0, out, sizeof(out));
    os_log(OS_LOG_DEFAULT, "%s", out);
    gIoNow.copyTo(gIoLine);
    ioLineNs = now;
}
#endif

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
    if (response)
        *response = 0;
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
    // Data and R1b commands end in transfer complete (an R1b's when the card's busy ends), which follows command complete.
    const bool transfers = data != 0 || (flags & 3) == 3;
    const uint32_t done = INT_COMMAND | (transfers ? INT_TRANSFER : 0);
    const uint32_t s = awaitStatus(transfers ? INT_TRANSFER : INT_COMMAND, transfers ? 1500000000ULL : 100000000ULL);
    ended(s);
    const uint32_t answer = r32(RESPONSE);
    initStatus = s;
    if ((s & INT_ERRORS) || (s & done) != done) {
        if (response && (s & INT_COMMAND) && !(s & 0x000f0000u))
            *response = answer;
        recover(false);
        return (s & INT_ERRORS) ? 2 : 3;
    }
    if (response)
        *response = answer;
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
// one-shot): the identity and capabilities first, then the raw registers and the extension functions. SCR (ACMD51), SD
// Status (ACMD13) and, when the SCR lists CMD48/49, the General Information and the Power Management and Performance
// Enhancement register sets (CMD48). Read-only: nothing here writes the card. A command that fails only leaves its part
// out. Needs the RCA, so the re-initialized bus (raiseBusSpeed).
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
        // The performance register set is read only when its first 263 bytes sit in one page.
        if (!extWhy && extInfo.perf.found) {
            if (!switchSdPerfFits(extInfo.perf)) {
                extWhy = "performance register set crosses a page";
            } else if (!extRead(extInfo.perf.fno, extInfo.perf.address, kSdPerfLength, regs)) {
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
             "CMD48/49 %s | %s | queue depth %u, cache %s",
             maker ? maker : makerHex, maker ? " (" : "", maker ? makerHex : "", maker ? ")" : "",
             haveCid ? id.pnm : "?", haveCid ? id.oid : "?", static_cast<unsigned>(id.prv >> 4),
             static_cast<unsigned>(id.prv & 0xf), id.psn, static_cast<unsigned>(id.year),
             static_cast<unsigned>(id.month), size.bytes >> 30, haveCsd ? kinds[size.structure & 3] : "(no CSD)",
             haveScr ? static_cast<unsigned>(scr.major) : 0u, cmd23Support, cmd48Support, classes,
             static_cast<unsigned>(queue), cardCache);
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

int NativeSwitchBlock::ExtRegBus::writeByte(uint8_t fno, uint32_t address, uint8_t value, uint32_t *r1) const
{
    memset(self->bounce, 0, kSector);
    self->bounce[0] = value;
    return self->controlCommand(49, switchSdExtArgument(fno, address, 1), RSP_R1, r1, kRegWrite, 1500000000ULL,
                                allowRelease);
}

int NativeSwitchBlock::ExtRegBus::readBytes(uint8_t fno, uint32_t address, uint32_t length, uint8_t *out,
                                            uint32_t *r1) const
{
    *r1 = 0;
    if (!switchSdExtRange(address, length))
        return -1;
    const int step = self->cardCommand(48, switchSdExtArgument(fno, address, length), RSP_R1, r1, kSector);
    if (step)
        return step;
    memcpy(out, self->bounce, length);
    return 0;
}

void NativeSwitchBlock::disableFoundCache()
{
    const ExtRegBus cacheBus{this, true};
    const SwitchSdCacheOff off = switchSdDisableFoundCache(havePerf, extInfo.perf, perf, cacheBus);
    if (off.step == SwitchSdCacheOffStep::kSkipped)
        return;
    if (off.step == SwitchSdCacheOffStep::kDone) {
        IOLog("SwitchSD: card cache was left on at boot; flushed and disabled\n");
        return;
    }
    uint32_t irq = 0;
    if (off.step == SwitchSdCacheOffStep::kReadBackFailed) {
        irq = initStatus;
    } else if (off.step == SwitchSdCacheOffStep::kFlushFailed || off.step == SwitchSdCacheOffStep::kDisableFailed) {
        irq = cardLastIrq;
    }
    panic("SwitchSD: card cache left on at boot could not be disabled (step %u, err %d, R1 0x%08x, irq 0x%08x, "
          "cache 0x%02x flush 0x%02x)",
          static_cast<unsigned>(off.step), off.error, off.r1, irq, static_cast<unsigned>(off.cacheEnable),
          static_cast<unsigned>(off.flushCache));
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
    switch_pmc_rmw(PMC_PWR_DET_VAL, PMC_SDMMC1_3V3, 0);
    (void)switch_pmc_read(PMC_PWR_DET_VAL);
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
    if (!car || !apb) {
        IOLog("SwitchSD: SDR104 skipped (CAR/APB not mapped); inherited bus kept\n");
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
    switch_pmc_rmw(PMC_NO_IOPOWER, 0, PMC_SDMMC1_NO_POWER);
    switch_pmc_rmw(PMC_PWR_DET_VAL, 0, PMC_SDMMC1_3V3);
    (void)switch_pmc_read(PMC_PWR_DET_VAL);
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
    switch_pmc_rmw(PMC_NO_IOPOWER, PMC_SDMMC1_NO_POWER, 0);
    (void)switch_pmc_read(PMC_NO_IOPOWER);
    if (tegra_gpio_output(kCardPowerGpio, 1))
        panic("SwitchSD: cannot switch the card supply (GPIO PE4) on");
    IOSleep(10);
    switch_pmc_rmw(PMC_PWR_DET_VAL, 0, PMC_SDMMC1_3V3);
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

Request *NativeSwitchBlock::drainQueueLocked()
{
    Request *abandoned = head;
    head = tail = nullptr;
    if (abandoned) {
#if DEVELOPMENT || DEBUG
        gIo.depthChanging(switch_now_ns(), queued);
#endif
        queued = 0;
        for (unsigned c = 0; c < kClasses; ++c)
            queuedReads[c] = 0;
    }
    if (Unmap *u = unmaps) {
        unmaps = nullptr;
        while (u) {
            Unmap *next = u->next;
            u->next = nullptr;
            u->result = kIOReturnNotReady;
            u->finished = true;
            u = next;
        }
        IOLockWakeup(queueLock, &unmaps, false);
    }
    return abandoned;
}

void NativeSwitchBlock::releaseRequestPool()
{
    Request *slab = nullptr;
    if (queueLock) {
        IOLockLock(queueLock);
        slab = requestPool.detachIfIdle();
        if (slab)
            requestSlab = nullptr;
        IOLockUnlock(queueLock);
    } else {
        slab = requestPool.detachIfIdle();
        if (slab)
            requestSlab = nullptr;
    }
    if (slab)
        IOFree(slab, kRequestPool * sizeof(Request));
}

void NativeSwitchBlock::stop(IOService *provider)
{
    if (queueLock) {
        IOLockLock(queueLock);
        suspended = true;
        shutdownDone = true;
        while (working)
            IOLockSleep(queueLock, &working, THREAD_UNINT);
        Request *const abandoned = drainQueueLocked();
        IOLockUnlock(queueLock);
        if (abandoned) {
            Batch b = {};
            b.first = abandoned;
            b.result = kIOReturnNotReady;
            completeBatch(b);
        }
    }
    if (trimCall)
        thread_call_cancel(trimCall);
    releaseRequestPool();
    IOService::stop(provider);
}

void NativeSwitchBlock::free()
{
    releaseRequestPool();
    IOService::free();
}

void NativeSwitchBlock::shutdown()
{
    if (panic_active())
        return;
    if (!preemption_enabled()) {
        IOLog("SwitchSD: power off notification skipped (preemption off)\n");
        return;
    }
    IOLockLock(queueLock);
    if (shutdownDone) {
        IOLockUnlock(queueLock);
        return;
    }
    const bool alreadySuspended = suspended;
    suspended = true;
    shutdownDone = true;
    while (working)
        IOLockSleep(queueLock, &working, THREAD_UNINT);
    Request *const abandoned = drainQueueLocked();
    IOLockUnlock(queueLock);
    if (abandoned) {
        Batch b = {};
        b.first = abandoned;
        b.result = kIOReturnNotReady;
        completeBatch(b);
    }
    if (alreadySuspended)
        return;
    inShutdown = true;
    if (trimCall)
        thread_call_cancel(trimCall);
    // SD 6.00 Figure 5-18 allows only CMD48 after POFN=1, so suppress CMD13 release fallback on CMD49.
    const ExtRegBus extBus{this, false};
    const SwitchSdPowerOff pon = switchSdPowerOffNotify(havePower, extInfo.power, power, extBus);
    if (pon.step == SwitchSdPowerOffStep::kDone) {
        IOLog("SwitchSD: power off notification ready (status 0x%02x)\n", static_cast<unsigned>(pon.status));
    } else if (pon.step != SwitchSdPowerOffStep::kSkipped) {
        uint32_t irq = 0;
        if (pon.step == SwitchSdPowerOffStep::kNotifyFailed) {
            irq = cardLastIrq;
        } else if (pon.step == SwitchSdPowerOffStep::kReadBackFailed) {
            irq = initStatus;
        }
        IOLog("SwitchSD: power off notification failed (step %u, err %d, R1 0x%08x, irq 0x%08x, status 0x%02x)\n",
              static_cast<unsigned>(pon.step), pon.error, pon.r1, irq, static_cast<unsigned>(pon.status));
    }
    dmaSuspend();
    if (irqSource)
        irqSource->disable();
}

IOReturn NativeSwitchBlock::callPlatformFunction(const OSSymbol *function, bool wait, void *p1, void *p2, void *p3,
                                                 void *p4)
{
    if (function && function->isEqualTo("IOPlatformHaltRestartAction")) {
        const uint32_t message = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p2));
        if (message == kPEHaltCPU || message == kPEUPSDelayHaltCPU || message == kPERestartCPU)
            shutdown();
        return kIOReturnSuccess;
    }
    return IOService::callPlatformFunction(function, wait, p1, p2, p3, p4);
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
    IOLockLock(queueLock);
    suspended = false;
    IOLockWakeup(queueLock, &head, false);
    IOLockUnlock(queueLock);
}

// setup(): nullptr when ADMA2 carries the data path, else why not: the registers allow it (switchSdPath) and its
// descriptor table has memory. A read chain's refused segments are staged alone through the bounce
// buffer when the stages have memory too (else the chain bounces whole).
const char *NativeSwitchBlock::setupAdma()
{
    if (copyFlush)
        return "copyFlush";
    if (!switchSdPath(r32(CAPABILITIES), r16(HOST_CONTROL2), nullptr).adma)
        return "the controller reports no ADMA2, or HOST_CONTROL2 does not keep v4 64-bit addressing";
    if (!switch_dma_alloc(&tableMemory, kAdmaTableBytes, 0x00000000ffffffffULL & ~(kAdmaPage - 1)))
        return "no memory for the descriptor table";
    table.entries = reinterpret_cast<SwitchAdmaDescriptor *>(tableMemory.va);
    tablePA = tableMemory.pa;
    if (tablePA & (kAdmaPage - 1))
        return "the descriptor table is not page-aligned below 4 GiB";
    table.capacity = kAdmaDescriptors;
    table.limit = kAdmaDmaLimit;
    stages = static_cast<Staging::Stage *>(IOMalloc(kStages * sizeof(Staging::Stage)));
    return nullptr;
}

bool NativeSwitchBlock::setup()
{
#if DEVELOPMENT || DEBUG
    setupBeganNs = switch_now_ns();
#endif
    regs = switch_mmio_map(kControllerPA, 0x1000);
    mc = switch_mmio_map(kMemoryControllerPA, 0x1000);
    // CAR and APB_MISC/pinmux serve the SDR104 bring-up only; without them it is skipped.
    car = switch_mmio_map(kCarPA, 0x1000);
    apb = switch_mmio_map(kApbMiscPA, 0x4000);
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

    // GPT: header at LBA 1, then entries. Exactly one APFS partition. The header is also what a re-initialized card must
    // read the same (resume): the same card.
    (void)raiseBusSpeed();
    uint8_t header[kSector] = {};
    if (transfer(1, 1, false, header)) {
        IOLog("SwitchSD: GPT header read failed\n");
        return false;
    }
    memcpy(gptHeader, header, sizeof(gptHeader));
    identify();
    disableFoundCache();
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

    // DMA for the data path: ADMA2 straight to the requests' pages, reads and writes up to 1 MiB a command, the bounce
    // path's SDMA commands 256 KiB; without ADMA2, SDMA through the bounce buffer, 256 KiB a command.
    if (const char *why = setupAdma()) {
        IOLog("SwitchSD: ADMA2 off (%s); every request through the bounce buffer\n", why);
    } else {
        adma = true;
        maxSectors = kBounceSectors;
        maxWriteSectors = kDirectWriteSectors;
        autoCmd23 = switchSdPath(r32(CAPABILITIES), r16(HOST_CONTROL2), haveScr ? &scr : nullptr).autoCmd23;
        // r14-sdcq: gap reads need the sink and queue merging. switch_dma_alloc leaves no line of the sink dirty in the
        // cache: the engine's writes there are never overwritten by an eviction, and nobody reads them.
        if (merge &&
            switch_dma_alloc(&sinkMemory, static_cast<size_t>(kGapSectors) * kSector,
                             0x00000000ffffffffULL & ~(kAdmaPage - 1)) &&
            !(sinkMemory.pa & (kAdmaPage - 1)) && cacheableRam(sinkMemory.pa)) {
            sinkPA = sinkMemory.pa;
        }
        gapsOn = sinkPA != 0;
        IOLog("SwitchSD: ADMA2 on: commands up to %u KiB read, %u KiB write, every page below %llu GiB straight to the "
              "card%s; multi-block commands framed by %s; gap reads %s\n",
              maxSectors * kSector / 1024, maxWriteSectors * kSector / 1024, kAdmaDmaLimit >> 30,
              stages ? ", refused segments staged alone" : "",
              autoCmd23 ? "Auto CMD23" : "Auto CMD12 (no CMD23 in SCR)",
              gapsOn ? "on" : "off (no sink page below 4 GiB)");
    }

    requestSlab = static_cast<Request *>(IOMallocZero(kRequestPool * sizeof(Request)));
    if (requestSlab) {
        requestPool.init(requestSlab, kRequestPool);
    } else {
        IOLog("SwitchSD: request pool allocation failed; using per-request IOMalloc\n");
    }

    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&NativeSwitchBlock::worker, this, &thread) != KERN_SUCCESS) {
        releaseRequestPool();
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

#if DEVELOPMENT || DEBUG
extern "C" size_t switch_cryptex_io_totals(char *out, size_t size);  // SwitchCryptex.cpp

namespace {
// kern.switch_sd (debug kernels): the request accounting since boot (SwitchSdIo.h switchSdFormatTotals), the cryptex
// devices' (switch_cryptex_io_totals), how long setup() took to the device's publication, and the requests submitted
// and completed and the interrupts taken.
int sdSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    const NativeSwitchBlock *owner = gOwner;
    if (!owner)
        return SYSCTL_OUT(req, "down", 5);
    constexpr size_t kBytes = 16384;
    auto *line = static_cast<char *>(IOMallocData(kBytes));
    if (!line)
        return ENOMEM;
    size_t at = owner->formatTotals(line, kBytes);
    at += switch_cryptex_io_totals(line + at, kBytes - at);
    const int error = SYSCTL_OUT(req, line, at + 1);
    IOFreeData(line, kBytes);
    return error;
}

struct sysctl_oid gSdOid;

void registerSdSysctl()
{
    switch_sysctl_oid(&gSdOid, &sysctl__kern_children, "switch_sd",
                      static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED), sdSysctl, "A",
                      "SD requests since boot: queue wait and completion per class, command time per direction and "
                      "size, queue depth, bytes the CPU copied, setup time");
    sysctl_register_oid(&gSdOid);
}
}  // namespace
#endif

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
    // High priority runs after other platform halt/restart actions, right before SwitchPMIC cuts power.
    owner->setProperty("IOPlatformHaltRestartAction", 1000000u, 32);
    owner->registerService();
#if DEVELOPMENT || DEBUG
    owner->setupEndedNs = switch_now_ns();
    registerSdSysctl();
#endif
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
