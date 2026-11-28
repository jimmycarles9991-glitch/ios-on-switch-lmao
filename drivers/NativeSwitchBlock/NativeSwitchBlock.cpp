// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 SDMMC1 block devices for the APFS root and the shared SD volume.
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
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <os/log.h>
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
#include <sys/kdebug.h>
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
#include "SwitchSdPick.h"
#include "SwitchSdPool.h"
#include "SwitchSdRoot.h"
#include "SwitchSdVolume.h"
#include "SwitchSdWindow.h"
#include "SwitchSdWriteFence.h"
#include "SwitchSdBus.h"
#include "SwitchSdCard.h"
#include "SwitchSdCardDetect.h"
#include "SwitchSdPipeline.h"
#include "SwitchSdTrim.h"
#if SWITCH_STATS_IO
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "SwitchSdIo.h"
#endif

#if DEVELOPMENT || DEBUG || SWITCH_STATS_IO
#define SWITCH_SD_STATS 1
#else
#define SWITCH_SD_STATS 0
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
constexpr uint64_t kTrimRunMs = 500, kTrimMarginNs = 500000000ULL;
constexpr uint64_t kTrimQuietNs = 2000000000ULL, kTrimQuietLitNs = 3000000000ULL;
constexpr uint64_t kTrimBootSectors = 16ULL << 21;
constexpr uint64_t kTrimCursorEveryAus = 64;
constexpr uint32_t kSdEraseArgument = 0;
constexpr uint64_t kEraseRefusal = switch_refusal_key("SwitchSD", "erase");
constexpr uint64_t kFlushRefusal = switch_refusal_key("SwitchSD", "cache flush");
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
    UHS_DDR50 = 4,
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
    // CLK_SOURCE bits 31:29 (hekate clock.c:725-727); PLLP_OUT0 is 408 MHz with a U7.1 divider below.
    SOURCE_PLLP = 0,
    SOURCE_PLLC4_OUT2 = 3,
    SOURCE_PLLC4_OUT0 = 7,
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
// UHS DDR200 (hekate clock.c:793-799, sdmmc.c:1426-1464): PLLC4_OUT0 (998.4 MHz) / CAR 2.5 (3) = 399.36 MHz,
// SDHCI /2 (divider 1) in UHS_DDR50 host mode -> 199.68 MHz DDR (~170-190 MB/s on non-DLL cards).
constexpr uint32_t kDdr200CmdSys = 14, kDdr200CarDivider = 3, kDdr200SdhciDivider = 1, kDdr200KHz = 199680;

#if SWITCH_RECORDER || SWITCH_STATS_IO
// Each pick of the queue in the flight recorder's ring (our tracepoint subclass, DBG_DRIVERS/18, which its filter keeps;
// kernel/switch/include/switch/recorder.h's ranges): arg1 the chain's first LBA; arg2 its sectors | its requests << 32;
// arg3 its IOStoragePriority class | 0x100 when it passed older requests | 0x200 for a write | the requests queued
// before it was taken << 16; arg4 how long the queue's head had waited (us).
static constexpr uint32_t kSdPickKdbg = (6u << 24) | (18u << 16) | (0x3d8u << 2);
#endif

struct Request {
    Request *next;
    IOMemoryDescriptor *buffer;
    uint64_t lba;  // absolute card LBA
    uint32_t sectors;
    // r6-lows: the request's IOStoragePriority class (IOStorageAttributes priority >> 6: 0 high (I/O tier 0, the UI's page-ins),
    // 1 default, 2 low, 3 background).
    uint8_t klass;
    bool write : 1;
    bool chained : 1;  // r12-sdio: taken into the chain being picked (SwitchQueue.h), under queueLock
    bool pooled : 1;   // owned by requestPool slab (SwitchSdPool.h); false for overflow IOMalloc nodes
    bool shared : 1;
    bool replay : 1;  // read by the kernel's boot replay: queued in kSwitchSdReplayClass until it ages (SwitchSdPick.h)
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

// One doSynchronize / doSynchronizeCache waiter, flushed by the worker once no chain is in flight.
struct SyncWaiter {
    SyncWaiter *next;
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
    int issueStep;  // issue()'s result when the command never reached the card
#if SWITCH_SD_STATS
    uint64_t beganNs;  // when beginBatch started
#endif
#if SWITCH_STATS_IO
    uint64_t oldestQueuedNs, issuedNs;
    bool wokeFromIdle;
#endif
    IOReturn result;  // endBatch's, for every request of the chain
};

struct PipeSlot : SwitchSdPipeSlot {
    static constexpr uint32_t kDescWords = (kAdmaDescriptors + 63) / 64;
    switch_dma_buffer tableMemory = {};
    SwitchAdmaTable table = {};
    uint64_t tablePA = 0;
    const SwitchSdWindow *window = nullptr;
    bool tableUncached = false;
    uint64_t descUncached[kDescWords] = {};
    Staging staging = {};
    Staging::Stage *stages = nullptr;
    Batch batch = {};
};

// Which request a chain starts with: SwitchSdPick.h. Writes keep FIFO order among themselves.
constexpr unsigned kClasses = kSwitchSdPickClasses;

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
    // Volatile write cache flushes (doSynchronize / suspend / shutdown).
    uint64_t cacheFlushes, cacheFlushNs;
    // Primary-IRQ fast-issued ADMA2 commands (SwitchSdPipeline.h).
    uint64_t fastIssued;
#if SWITCH_STATS_IO
    // The last chain's busy end and its completions' span, to take overlap out of the next chain's busy time.
    uint64_t lastBusyEndNs, lastCompleteStartNs, lastCompleteEndNs;
    // SwitchSdBus.h: commands refused because the bus was not free at their one PRESENT_STATE read, and DAT0 releases
    // awaited by CMD13 with busy (the card still programming after a transfer complete). ADMA2 chains that reached a
    // non-cacheable page (dmaPage).
    uint64_t busRefused, releases, uncachedChains;
#endif
};
}

#if SWITCH_STATS_IO
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
    const SwitchSdWindow *window = nullptr;
    bool shared = false;

public:
    bool configure(NativeSwitchBlock *, const SwitchSdWindow *, bool shared);

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
    IOReturn doSetPriority(IOBlockStorageDeviceExtent *extents, UInt32 count, IOStoragePriority priority) override;
    IOReturn raise(const IOBlockStorageDeviceExtent *extents, UInt32 count, IOStoragePriority priority, bool cryptex);

    char *getVendorString() override
    {
        static char vendor[] = "Switch";
        return vendor;
    }

    char *getProductString() override
    {
        static char rootName[] = "SD APFS", sharedName[] = "SD Card";
        return shared ? sharedName : rootName;
    }

    char *getRevisionString() override
    {
        static char revision[] = "2.0";
        return revision;
    }

    char *getAdditionalDeviceInfoString() override
    {
        static char rootInfo[] = "SDMMC1 APFS partition", sharedInfo[] = "SDMMC1 shared partition";
        return shared ? sharedInfo : rootInfo;
    }

    IOReturn reportBlockSize(UInt64 *value) override
    {
        *value = kSector;
        return kIOReturnSuccess;
    }

    IOReturn reportMaxValidBlock(UInt64 *value) override
    {
        *value = window->count - 1;
        return kIOReturnSuccess;
    }

    IOReturn reportEjectability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn reportRemovability(bool *value) override
    {
        *value = shared;
        return kIOReturnSuccess;
    }

    IOReturn reportWriteProtection(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn reportMediaState(bool *present, bool *changed) override;

    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override;
    IOReturn doSynchronizeCache() override;
    IOReturn getWriteCacheState(bool *value) override;

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
    thread_t workerThread = THREAD_NULL;
    SwitchSdWriteFence rootWrites, filesWrites;

    SwitchSdWriteFence &writes(const SwitchSdWindow *window)
    {
        return window == &rootWindow ? rootWrites : filesWrites;
    }

    const SwitchSdWindow *windowOf(bool shared) const
    {
        return shared ? &filesWindow : &rootWindow;
    }

    Request *requestSlab = nullptr;
    SwitchSdPool<Request> requestPool = {};
    Request *head = nullptr, *tail = nullptr;
    Unmap *unmaps = nullptr;      // under queueLock, oldest first
    SyncWaiter *syncs = nullptr;  // under queueLock: callers awaiting CMD49 Flush Cache
    bool wantCache = false;       // SD 6.00 volatile write cache enabled at boot (re-enabled on resume)
    bool cacheEnabled = false;    // card's volatile write cache is currently enabled
    bool writesDirty = false;     // under queueLock: writes completed since last successful flush
    uint32_t activeWrites = 0;    // under queueLock: write slots currently executing in the pipeline
    // Erase-based trim (cards without discard): the map and the quiet-period timer, under queueLock.
    SwitchSdTrimMap trim;
    uint64_t *trimBits = nullptr;
    size_t trimBytes = 0;
    thread_call_t trimCall = nullptr;
    thread_call_t filesRemoval = nullptr;
    uint64_t lastIoNs = 0, trimBootSectors = 0, trimSinceSave = 0, trimLastSaveNs = 0;
    static constexpr uint64_t kTrimSaveMinNs = 60ULL * 1000000000ULL;
    bool trimCursorLoaded = false, trimFailed = false;
    bool workerSleeping = false;
    uint32_t suspendWaiters = 0;
    unsigned chainsSinceUnmap = 0;        // under queueLock: chains picked while a discard waited
    uint32_t queued = 0;                  // requests waiting, under queueLock
    uint32_t queuedReads[kClasses] = {};  // r6-lows: reads waiting per class, under queueLock
    uint32_t queuedReplay = 0;            // queued replay reads the ager may move (switchSdAgeable), under queueLock
#if SWITCH_SD_STATS
    ClassWait waits[kClasses] = {};        // r6-lows: per class since the last throughput line, under queueLock
    uint64_t passed = 0, passedTotal = 0;  // r6-lows: reads taken before older requests (this line, since boot)
#endif
    SwitchSdPickState pickState = {};  // under queueLock
#if SWITCH_STATS_IO
    // Raises since boot, under queueLock: XNU's (doSetPriority) and SwitchCryptex's (switch_block_raise), each as calls
    // and the queued requests they raised.
    uint64_t xnuRaiseCalls = 0, xnuRaised = 0, cryptexRaiseCalls = 0, cryptexRaised = 0;
#endif
#if SWITCH_SD_STATS
    uint64_t submitted = 0, serviced = 0;  // for the root-wait report
    uint64_t lastEnd = 0;                  // card LBA after the previous request (sequential count)
    static constexpr uint64_t statsEveryNs = 10 * 1000000000ULL;
    uint64_t statsLastNs = 0;
#endif
    static constexpr bool merge = true;
    // r12-sdio: reads and contiguous writes merge anywhere in the queue (SwitchQueue.h) until a chain fails its check.
    bool mergeAnywhere = true;
    // Requests' bytes through IOMemoryDescriptor::readBytes/writeBytes, with the clean+invalidate of every destination
    // line they add on this kernel (SwitchClientCopy.h), for A/B builds; it keeps ADMA2 off too, so every request takes
    // the bounce buffer as before r10-adma2.
    static constexpr bool copyFlush = false;
    // Two-slot prepared ADMA2 pipeline (SwitchSdPipeline.h): each slot owns its 16 KiB descriptor table and read
    // staging array; hwLock synchronizes interruptHandler's fast-issue path with the worker.
    PipeSlot slots[2] = {};
    SwitchSdPipeline<PipeSlot> pipe = {};
    IOSimpleLock *hwLock = nullptr;
    int workerEvent = 0;
    bool pipeWaiting = false;
    bool admaSelected = false;
    bool adma = false;
    uint8_t admaErrorState = 0;
    uint64_t admaErrorAddress = 0;
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
    // waits for and sleeps on irqEvent; the primary handler disables them again (lowering the line) and wakes it.
    IOService *irqNub = nullptr;
    IOService *cdNub = nullptr;
    bool cardDetectArmed = false;
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
    bool ddr200 = false;
    bool ddr200Allowed = true;
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
    static void interruptHandler(OSObject *, void *, IOService *, int);
    void startCardDetect();
    static void cardDetectHandler(OSObject *, void *, IOService *, int);
    void checkCardPresent(const char *where);
    [[noreturn]] void cardRemovedPanic(const char *where, int level);
    bool waitSignal(uint32_t signals, uint64_t timeoutNs);
    uint32_t awaitStatus(uint32_t awaited, uint64_t limitNs);
    [[noreturn]] void interruptLost(uint32_t awaited);
    void resetLines();
    void recover(bool data);
    int command(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool cmd23 = false,
                const SwitchSdWindow *window = nullptr, uint64_t descPA = 0);
    // r12-sdio: command()'s halves, so the worker can do other work while the card moves the data. r14-sdcq: `cmd23`
    // frames a multi-block command with Auto CMD23 instead of Auto CMD12 (switchSdBlockFrame).
    int issue(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, bool cmd23 = false,
              const SwitchSdWindow *window = nullptr, uint64_t descPA = 0);
    int complete(uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool *early, bool cmd23 = false);
    bool failed(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write, unsigned descriptors, bool retry,
                uint64_t descPA = 0);
    int run(uint64_t lba, uint32_t sectors, bool write, unsigned descriptors, const SwitchSdWindow *window);
    int retry(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write, unsigned descriptors,
              const SwitchSdWindow *window, uint64_t descPA = 0);
    void emitSlotCommand(PipeSlot &slot);
    void launchSlot(PipeSlot &slot);
    void prepareAndPublishSlot(PipeSlot &slot);
    void finishSlowActive(PipeSlot &slot);
    void completeSlot(PipeSlot &slot, int step);
    void waitForPipeline(bool wakeOnSubmit);
    static void worker(void *, wait_result_t);
    void enqueueLocked(Request *request);
    Request *drainQueueLocked();
    void releaseRequestPool();
    Request *pickLocked(uint32_t *sectors, uint32_t *count, const SwitchLbaRange *inFlight = nullptr,
                        unsigned inFlightCount = 0);
    void takenLocked(Request *first, uint32_t count, uint64_t pickNs);
#if SWITCH_SD_STATS
    void account(const PipeSlot &);
#endif
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
    SwitchAdmaRefusal buildTable(PipeSlot &slot, Request *first, uint32_t sectors, bool write);
    void prepareDirect(PipeSlot &slot, bool write);
    void finishDirect(PipeSlot &slot, bool write);
    bool stageChain(Request *first, uint32_t from, uint32_t length, uint8_t *buffer, bool toClient);
    bool bounceChain(Request *first, uint32_t sectors);
    bool copyStaged(PipeSlot &slot);
    const char *setupAdma();
    void dmaSuspend();
    void dmaResume();
#if SWITCH_SD_STATS
    void logStats();
    void logThroughput();
#endif
    // SDR104 bring-up (setup() and re-tuning only).
    int cardCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data = 0);
    int appCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data = 0);
    void readR2(uint8_t out[16]);
    // r14-sdcq: the card's identity and capabilities, read and logged once at boot.
    bool extRead(uint8_t fno, uint32_t address, uint32_t length, uint8_t *out);
    void identify();
    void disableFoundCache();
    void configureCache();
    bool enableCache(bool resume);
    bool flushCache();
    void powerOffNotify();
    int switchFunction(bool set, unsigned group, uint32_t value);
    void setTiming(int uhs, bool highSpeed, int source, uint32_t carDivider, uint32_t divider);
    bool calibratePads();
    bool switchTo1V8(uint8_t ldo2);
    bool enablePllc4();
    void tuneStep(int manualTap = -1);
    bool tune();
    bool tuneDdr200(uint32_t *windowOut = nullptr);
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
#if SWITCH_STATS_IO
    // When the last 10 s line of the request accounting (gIo) was made, and when the driver began and finished its
    // setup (kern.switch_sd's setup_us).
    uint64_t ioLineNs = 0, setupBeganNs = 0, setupEndedNs = 0;
    uint64_t lastIssuedNs = 0, lastIrqNs = 0, lastWakeNs = 0, lastCmdIrqNs = 0, lastCmdWakeNs = 0, prevIrqNs = 0;
    void logIo(uint64_t now);
    size_t formatTotals(char *out, size_t size) const;
#endif
    SwitchSdWindow rootWindow, filesWindow;
    switch_sd_files_slice filesSlice = {};
    bool filesOnline = false;
    bool filesPublishQueued = false;

    int findFiles(switch_sd_files_slice *);
    void verifyFilesCard();
    IOReturn syncLocked(const SwitchSdWindow *window, bool acknowledgeErrors = true);
    void publishFiles();
    void publishFilesNow();
    void removeFilesIfOfflineLocked();
    // Per request and per merged command, as advertised: reads kBounceSectors with ADMA2 or once setup() has checked
    // SDMA boundary resumes, else kSafeSectors; writes kDirectWriteSectors with ADMA2, else kSafeSectors.
    uint32_t maxSectors = kSafeSectors, maxWriteSectors = kSafeSectors;
    NativeSwitchBlockDevice *device = nullptr;

    bool setup();
    int transfer(uint64_t lba, uint32_t sectors, bool write, uint8_t *bytes, const SwitchSdWindow *window = nullptr);
    IOReturn submit(const Request &);
    IOReturn unmap(const IOBlockStorageDeviceExtent *extents, uint32_t count, uint64_t blocks);
    void promote(const SwitchSdWindow *window, const IOBlockStorageDeviceExtent *extents, uint32_t count, uint8_t klass,
                 bool cryptex);
    IOReturn synchronize(const SwitchSdWindow *window);

    bool writeCacheEnabled() const
    {
        return __atomic_load_n(&cacheEnabled, __ATOMIC_ACQUIRE);
    }

    bool filesPresent() const
    {
        return __atomic_load_n(&filesOnline, __ATOMIC_ACQUIRE);
    }

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
}

// The boot replay's reads reach doAsyncReadWrite on its thread (switch_vm_prefetch_is_replay_thread): vnode_pagein ->
// cluster_io -> spec_strategy -> IOMediaBSDClient -> IOBlockStorageDriver, and SwitchCryptex's window fills, which it
// issues on the requesting thread.

extern "C" bool switch_sd_trim_stats(uint64_t *erases, uint64_t *sectors, uint64_t *busy_ns)
{
    if (!gOwner)
        return false;
    gOwner->trimStats(erases, sectors, busy_ns);
    return true;
}

namespace {
bool putString(OSDictionary *dictionary, const char *key, const char *value)
{
    const OSString *string = OSString::withCStringNoCopy(value);
    if (!string)
        return false;
    const bool stored = dictionary->setObject(key, string);
    string->release();
    return stored;
}
}

bool NativeSwitchBlockDevice::configure(NativeSwitchBlock *provider, const SwitchSdWindow *range, bool files)
{
    owner = provider;
    window = range;
    shared = files;
    setProperty(kIOMaximumBlockCountReadKey, provider->maxSectors, 64);
    setProperty(kIOMaximumBlockCountWriteKey, provider->maxWriteSectors, 64);
    setProperty(kIOMaximumByteCountReadKey, static_cast<unsigned long long>(provider->maxSectors) * kSector, 64);
    setProperty(kIOMaximumByteCountWriteKey, static_cast<unsigned long long>(provider->maxWriteSectors) * kSector, 64);
    if (OSDictionary *protocol = OSDictionary::withCapacity(2)) {
        // Report SwitchSDSharedVolume as Internal so SKDisk.isInternal hides Erase in Files.app; /etc/fstab,
        // DAEnableUserFSMountInternal, and switch-sdwake handle auto-mounting it via UserFS.
        const bool hasType = putString(protocol, kIOPropertyPhysicalInterconnectTypeKey, "SD");
        const bool hasLocation =
            putString(protocol, kIOPropertyPhysicalInterconnectLocationKey, kIOPropertyInternalKey);
        const bool installed = setProperty(kIOPropertyProtocolCharacteristicsKey, protocol);
        protocol->release();
        if (shared && !(hasType && hasLocation && installed))
            return false;
    } else if (shared) {
        return false;
    }
    if (OSDictionary *device = OSDictionary::withCapacity(1)) {
        putString(device, kIOPropertyMediumTypeKey, kIOPropertyMediumTypeSolidStateKey);
        setProperty(kIOPropertyDeviceCharacteristicsKey, device);
        device->release();
    }
    // Force Unit Access only when the card's volatile write cache is off; with it on, durability goes through
    // doSynchronize / doSynchronizeCache (CMD49 Flush Cache). Unmap: SD discard or idle erase. Priority: the queue
    // orders by tier (SwitchSdPick.h) and doSetPriority raises queued requests, which makes XNU's mounts on it
    // IOSCHED_SUPPORTED (vfs_init_io_attributes).
    if (OSDictionary *features = OSDictionary::withCapacity(3)) {
        if (!provider->writeCacheEnabled())
            features->setObject(kIOStorageFeatureForceUnitAccess, kOSBooleanTrue);
        if (!shared && provider->canUnmap())
            features->setObject(kIOStorageFeatureUnmap, kOSBooleanTrue);
        features->setObject(kIOStorageFeaturePriority, kOSBooleanTrue);
        setProperty(kIOStorageFeaturesKey, features);
        features->release();
    }
    setName(shared ? "SwitchSDSharedVolume" : "SwitchSD");
    return true;
}

IOReturn NativeSwitchBlockDevice::reportMediaState(bool *present, bool *changed)
{
    *present = !shared || owner->filesPresent();
    if (changed)
        *changed = false;
    return kIOReturnSuccess;
}

IOReturn NativeSwitchBlockDevice::doSynchronize(UInt64, UInt64, IOOptionBits)
{
    return owner->synchronize(window);
}

IOReturn NativeSwitchBlockDevice::doSynchronizeCache()
{
    return owner->synchronize(window);
}

IOReturn NativeSwitchBlockDevice::getWriteCacheState(bool *value)
{
    *value = owner->writeCacheEnabled();
    return kIOReturnSuccess;
}

IOReturn NativeSwitchBlockDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                                   IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !count || block >= window->count || count > window->count - block ||
        count > owner->maxSectors || buffer->getLength() < count * kSector)
        return kIOReturnBadArgument;
    if (shared && !owner->filesPresent())
        return kIOReturnNoMedia;
    const IODirection direction = buffer->getDirection();
    if (direction != kIODirectionIn && direction != kIODirectionOut)
        return kIOReturnBadArgument;
    if (direction == kIODirectionOut && count > owner->maxWriteSectors)
        return kIOReturnBadArgument;
    // No attributes: the default class, as IOStorage's own default priority.
    const IOStoragePriority priority = attributes ? attributes->priority : kIOStoragePriorityDefault;
    const bool replay = direction == kIODirectionIn && &switch_vm_prefetch_is_replay_thread &&
                        switch_vm_prefetch_is_replay_thread(current_thread());
    const uint8_t klass = switchSdClassOf(static_cast<uint8_t>(priority >> 6), replay);
#if SWITCH_STATS_IO
    switchSdAdd(&gIo.asked[priority >> 6], 1);
    if (replay)
        switchSdAdd(&gIo.chosen, 1);
#endif
    const Request spec = {nullptr,
                          buffer,
                          window->first + block,
                          static_cast<uint32_t>(count),
                          klass,
                          direction == kIODirectionOut,
                          false,
                          false,
                          shared,
                          replay,
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

// Primary interrupt context. Masking the signals lowers the level line, so there is nothing left for a work loop.
// For pipelined ADMA2 commands, a clean completion rotates `active` -> `done` and immediately issues `prepared`
// inside the ISR; errors and DAT0-busy completions leave STATUS and bus untouched for worker recovery.
void NativeSwitchBlock::interruptHandler(OSObject *owner, void *, IOService *, int)
{
    auto *self = static_cast<NativeSwitchBlock *>(owner);
    IOSimpleLockLock(self->hwLock);
    const uint32_t s = self->r32(STATUS);
    const uint32_t en = self->r32(SIGNAL_ENABLE);
    if (!(s & en)) {
        IOSimpleLockUnlock(self->hwLock);
        return;
    }
    self->w32(SIGNAL_ENABLE, 0);
    (void)self->r32(SIGNAL_ENABLE);  // the line is low before ArmCpu ends the interrupt
#if SWITCH_SD_STATS
    __atomic_fetch_add(&self->stats.interrupts, 1, __ATOMIC_RELAXED);
#endif
#if SWITCH_STATS_IO
    const uint64_t now = switch_now_ns();
#endif
    if (!self->pipe.active) {
#if SWITCH_STATS_IO
        __atomic_store_n(&self->lastIrqNs, now, __ATOMIC_RELEASE);
#endif
        IOSimpleLockUnlock(self->hwLock);
        thread_wakeup(&self->irqEvent);
        return;
    }
    SwitchSdIrqSample sample = {};
    sample.status = s;
    sample.response = self->r32(RESPONSE);
    if (self->pipe.active->sectors > 1) {
        sample.autoCmdError = self->r16(AUTO_CMD_ERROR);
        if (!self->pipe.active->cmd23)
            sample.stopResponse = self->r32(STOP_RESPONSE);
    }
    sample.present = self->r32(PRESENT);
    if (s & INT_ADMA_ERROR) {
        sample.admaError = self->r8(ADMA_ERROR);
        sample.admaAddress = self->r32(DMA_LOW) | static_cast<uint64_t>(self->r32(DMA_HIGH)) << 32;
    }
#if SWITCH_STATS_IO
    sample.nowNs = now;
#endif
    const SwitchSdIrqAction action = self->pipe.onIrq(self->bus, sample, INT_ERRORS, R1_ERRORS, R1_STOP_ERRORS,
                                                      INT_ADMA_ERROR, [self](PipeSlot &next) {
                                                          self->emitSlotCommand(next);
                                                      });
    if (action == SwitchSdIrqAction::kDoneIdle)
        self->w32(STATUS, INT_COMMAND | INT_TRANSFER);
#if SWITCH_SD_STATS
    if (action == SwitchSdIrqAction::kFastIssued)
        __atomic_fetch_add(&self->stats.fastIssued, 1, __ATOMIC_RELAXED);
#endif
    IOSimpleLockUnlock(self->hwLock);
    thread_wakeup(&self->workerEvent);
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
#endif
#if SWITCH_STATS_IO
    // Clear before assert_wait_deadline and publish before the Device-nGnRE SIGNAL_ENABLE write:
    // on ARMv8 a plain Normal store is not ordered before a subsequent Device store without a barrier.
    __atomic_store_n(&lastIrqNs, 0, __ATOMIC_RELAXED);
#endif
    assert_wait_deadline(&irqEvent, THREAD_UNINT, deadline);
#if SWITCH_STATS_IO
    switch_mmio_write32_publish(regs, SIGNAL_ENABLE, signals);
#else
    w32(SIGNAL_ENABLE, signals);
#endif
    const wait_result_t result = thread_block(THREAD_CONTINUE_NULL);
#if SWITCH_STATS_IO
    const uint64_t wakeNs = switch_now_ns();
#endif
#if DEVELOPMENT || DEBUG
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
#if SWITCH_STATS_IO
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
    if (!nub || nub->registerInterrupt(0, this, &NativeSwitchBlock::interruptHandler, nullptr) != kIOReturnSuccess)
        panic("SwitchSD: cannot attach the SDMMC1 interrupt (INTID %u)", kInterrupt);
    irqNub = nub;
    irqNub->enableInterrupt(0);
}

void NativeSwitchBlock::cardDetectHandler(OSObject *owner, void *, IOService *, int)
{
    static_cast<NativeSwitchBlock *>(owner)->checkCardPresent("interrupt");
}

void NativeSwitchBlock::cardRemovedPanic(const char *where, int level)
{
    panic("SwitchSD: SD card removed (%s, GPIO PZ1=%d)", where, level);
}

void NativeSwitchBlock::checkCardPresent(const char *where)
{
    const bool armed = __atomic_load_n(&cardDetectArmed, __ATOMIC_ACQUIRE);
    if (!armed)
        return;
    const int level = tegra_gpio_read(TEGRA_GPIO_SD_CD);
    if (switchSdShouldPanicOnRemoval(armed, panic_active() != 0, level))
        cardRemovedPanic(where, level);
}

void NativeSwitchBlock::startCardDetect()
{
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): TegraGPIO keeps its nubs; this reference is not ours
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_SD_CD, TEGRA_GPIO_EDGE_RISING);
    if (!nub) {
        char state[256];
        tegra_gpio_describe(TEGRA_GPIO_SD_CD, state, sizeof(state));
        panic("SwitchSD: no interrupt nub for SD card detect (PZ1): %s", state);
    }
    if (nub->registerInterrupt(0, this, &NativeSwitchBlock::cardDetectHandler, nullptr) != kIOReturnSuccess)
        panic("SwitchSD: cannot attach SD card detect interrupt (GPIO %u, PZ1)", TEGRA_GPIO_SD_CD);
    cdNub = nub;
    __atomic_store_n(&cardDetectArmed, true, __ATOMIC_RELEASE);
    cdNub->enableInterrupt(0);
    checkCardPresent("boot");
}

// Software-reset the CMD and DAT state machines (hekate _sdmmc_reset_cmd_data); STATUS is left alone.
void NativeSwitchBlock::resetLines()
{
    w8(RESET, 0x06);
    for (const uint64_t start = switch_now_ns(); (r8(RESET) & 0x06) && switch_now_ns() - start < 100000000;) {
    }
    admaSelected = false;
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
                               bool cmd23, const SwitchSdWindow *window, uint64_t descPA)
{
    *status = 0;
    if (const int step = issue(lba, sectors, write, descriptors, cmd23, window, descPA))
        return step;
    return complete(sectors, write, descriptors, status, nullptr, cmd23);
}

// The command's first half: the partition check, the registers and the command itself. 0, or step 1 when the bus was not
// free (claimBus; then nothing was issued).
int NativeSwitchBlock::issue(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, bool cmd23,
                             const SwitchSdWindow *window, uint64_t descPA)
{
    if ((window && !window->contains(lba, sectors)) || (write && !window) || !sectors || sectors - 1 > UINT32_MAX - lba)
        panic("SwitchSD: command outside its partition lba=%u n=%u write=%d", lba, sectors, write);
    if (window == &filesWindow && !filesPresent())
        return -1;
    if (!claimBus(write ? SwitchSdUse::kWrite : SwitchSdUse::kRead))
        return 1;
    w32(STATUS, 0xffffffff);
    // DMA select per command: the bring-up commands' SDMA (cardCommand) and the data path share the controller.
    const uint64_t address = descriptors ? (descPA ? descPA : slots[0].tablePA) : bouncePA;
    w8(HOST_CONTROL, static_cast<uint8_t>((r8(HOST_CONTROL) & ~DMA_SELECT) | (descriptors ? DMA_ADMA2 : 0)));
    admaSelected = descriptors != 0;
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
#if SWITCH_STATS_IO
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
#if SWITCH_STATS_IO
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
#if SWITCH_STATS_IO
    if (!bus.idle())
        lastCmdIrqNs = 0;
#endif
    if (!bus.idle() && awaitRelease())
        return 10;
    return 0;
}

int NativeSwitchBlock::transfer(uint64_t lba, uint32_t sectors, bool write, uint8_t *bytes,
                                const SwitchSdWindow *window)
{
    const uint32_t length = sectors * kSector;
    if (!sectors || lba > UINT32_MAX || sectors - 1 > UINT32_MAX - lba ||
        sectors > (write ? kSafeSectors : kBounceSectors))
        return -1;
    if (write && bytes != bounce)
        memcpy(bounce, bytes, length);
    // Clean (write) or clean+invalidate (read) before the device owns the lines.
    switch_dcache_clean_invalidate_lines(bounce, length);
    switch_dcache_sync();
    if (const int step = run(lba, sectors, write, 0, window))
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
                               unsigned descriptors, bool retry, uint64_t descPA)
{
    checkCardPresent("io error");
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
              admaErrorAddress, descPA ? descPA : slots[0].tablePA, descriptors);
    }
    recover(true);
    return host;
}

int NativeSwitchBlock::run(uint64_t lba, uint32_t sectors, bool write, unsigned descriptors,
                           const SwitchSdWindow *window)
{
    uint32_t status = 0;
    const int step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status, false, window);
    return step ? retry(step, status, lba, sectors, write, descriptors, window) : 0;
}

int NativeSwitchBlock::retry(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write,
                             unsigned descriptors, const SwitchSdWindow *window, uint64_t descPA)
{
    if (window == &filesWindow && !filesPresent())
        return step;
    if (failed(step, status, lba, sectors, write, descriptors, false, descPA))
        return step;
    stats.retries++;
    if (tuned)
        retune(false);
    if (write && !descriptors) {
        switch_dcache_clean_invalidate_lines(bounce, static_cast<uintptr_t>(sectors) * kSector);
        switch_dcache_sync();
    }
    step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status, false, window, descPA);
    if (step && !failed(step, status, lba, sectors, write, descriptors, true, descPA) && tuned &&
        (ddr200 || speed + 1 < kSpeedCount))
        retune(true);
    return step;
}

IOReturn NativeSwitchBlock::syncLocked(const SwitchSdWindow *window, bool acknowledgeErrors)
{
    auto &fence = writes(window);
    SwitchSdWriteFence::Barrier barrier;
    fence.begin(barrier, switch_now_ns());
    IOReturn result = kIOReturnSuccess;
    while (fence.completed < barrier.target) {
        if (window == &filesWindow && !filesPresent())
            break;
        if (suspended || shutdownDone || current_thread() == workerThread) {
            result = kIOReturnBusy;
            break;
        }
        IOLockSleep(queueLock, &fence, THREAD_UNINT);
    }
    fence.end(barrier, !acknowledgeErrors || result == kIOReturnBusy);
    if (window == &filesWindow && !filesPresent())
        return kIOReturnNoMedia;
    return !result && barrier.failed ? kIOReturnIOError : result;
}

void NativeSwitchBlock::enqueueLocked(Request *request)
{
    if (request->queuedNs > lastIoNs)
        lastIoNs = request->queuedNs;
    if (request->write) {
        ++writes(windowOf(request->shared)).submitted;
        if (!request->shared)
            switchSdTrimClear(&trim, request->lba, request->sectors);
    }
    if (tail) {
        tail->next = request;
    } else {
        __atomic_store_n(&head, request, __ATOMIC_RELEASE);
    }
    tail = request;
#if SWITCH_STATS_IO
    gIo.depthChanging(request->queuedNs, queued);
#endif
    ++queued;
#if SWITCH_SD_STATS
    if (queued > stats.maxQueued)
        stats.maxQueued = queued;
#endif
    if (!request->write)
        ++queuedReads[request->klass];
    if (switchSdAgeable(request))
        ++queuedReplay;
#if SWITCH_SD_STATS
    if (!stats.firstNs)
        stats.firstNs = statsLastNs = request->queuedNs;
    __atomic_fetch_add(&submitted, 1, __ATOMIC_RELAXED);
#endif
    if (workerSleeping)
        IOLockWakeup(queueLock, &head, true);
    if (__atomic_load_n(&pipeWaiting, __ATOMIC_ACQUIRE))
        thread_wakeup(&workerEvent);
}

IOReturn NativeSwitchBlock::submit(const Request &spec)
{
    struct Ops {
        NativeSwitchBlock *self;
        bool shared;

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
            return self->shutdownDone || (shared && !self->filesPresent());
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

    const SwitchSdPoolSubmitResult status = switchSdPoolSubmit(requestPool, spec, Ops{this, spec.shared});
    if (status == SwitchSdPoolSubmitResult::kQueued)
        return kIOReturnSuccess;
    if (status == SwitchSdPoolSubmitResult::kNoMemory)
        return kIOReturnNoMemory;
    return spec.shared && !filesPresent() ? kIOReturnNoMedia : kIOReturnNotReady;
}

IOReturn NativeSwitchBlockDevice::doUnmap(IOBlockStorageDeviceExtent *extents, UInt32 count, IOStorageUnmapOptions)
{
    return shared ? kIOReturnUnsupported : owner->unmap(extents, count, window->count);
}

// XNU raises a page-in to the tier of a thread that waits on one of its busy pages (vm_page_handle_prio_inversion ->
// DKIOCSETTIER -> IOMedia::setPriority -> here).
IOReturn NativeSwitchBlockDevice::doSetPriority(IOBlockStorageDeviceExtent *extents, UInt32 count,
                                                IOStoragePriority priority)
{
    return raise(extents, count, priority, false);
}

// `cryptex`: SwitchCryptex raising the window fills a foreground read joined (switch_block_raise), counted apart.
IOReturn NativeSwitchBlockDevice::raise(const IOBlockStorageDeviceExtent *extents, UInt32 count,
                                        IOStoragePriority priority, bool cryptex)
{
    if (count && !extents)
        return kIOReturnBadArgument;
    for (UInt32 i = 0; i < count; ++i) {
        if (extents[i].blockStart >= window->count || extents[i].blockCount > window->count - extents[i].blockStart)
            return kIOReturnBadArgument;
    }
    owner->promote(window, extents, count, static_cast<uint8_t>(priority >> 6), cryptex);
    return kIOReturnSuccess;
}

// Requests already in a pipeline slot are past the queue and keep their class.
void NativeSwitchBlock::promote(const SwitchSdWindow *window, const IOBlockStorageDeviceExtent *extents, uint32_t count,
                                uint8_t klass, bool cryptex)
{
    IOLockLock(queueLock);
    uint32_t raised = 0;
    for (uint32_t i = 0; i < count; ++i)
        raised += switchSdPromote(head, window->first + extents[i].blockStart, extents[i].blockCount, klass,
                                  queuedReads, queuedReplay);
#if SWITCH_STATS_IO
    ++(cryptex ? cryptexRaiseCalls : xnuRaiseCalls);
    (cryptex ? cryptexRaised : xnuRaised) += raised;
#else
    (void)raised;
    (void)cryptex;
#endif
    IOLockUnlock(queueLock);
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
            switchSdTrimMark(&trim, rootWindow.first + extents[i].blockStart, extents[i].blockCount);
        if (workerSleeping)
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
    if (workerSleeping)
        IOLockWakeup(queueLock, &head, true);
    while (!job.finished)
        IOLockSleep(queueLock, &unmaps, THREAD_UNINT);
    IOLockUnlock(queueLock);
    return job.result;
}

IOReturn NativeSwitchBlock::synchronize(const SwitchSdWindow *window)
{
    IOLockLock(queueLock);
    const IOReturn drained = syncLocked(window);
    if (drained != kIOReturnSuccess || !writeCacheEnabled()) {
        IOLockUnlock(queueLock);
        return drained;
    }
    if (shutdownDone) {
        IOLockUnlock(queueLock);
        return kIOReturnNotReady;
    }
    if (!writesDirty && !activeWrites) {
        IOLockUnlock(queueLock);
        return kIOReturnSuccess;
    }
    if (current_thread() == workerThread) {
        IOLockUnlock(queueLock);
        return kIOReturnBusy;
    }
    SyncWaiter waiter = {nullptr, kIOReturnSuccess, false};
    waiter.next = syncs;
    syncs = &waiter;
    if (workerSleeping)
        IOLockWakeup(queueLock, &head, true);
    while (!waiter.finished)
        IOLockSleep(queueLock, &syncs, THREAD_UNINT);
    IOLockUnlock(queueLock);
    return waiter.result;
}

// Worker thread, no chain in flight: the job's next discard. Extents are the APFS partition's blocks: rootWindow.first
// maps them onto the card. True once the job is finished (result set).
bool NativeSwitchBlock::stepUnmap(Unmap &job)
{
    if (job.at == job.end) {
        const IOBlockStorageDeviceExtent &extent = job.extents[job.index++];
        job.at = rootWindow.first + extent.blockStart;
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
    if (last < first || !rootWindow.contains(first, last - first + 1))
        return -1;
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
    const uint64_t words = switchSdTrimWords(rootWindow.first, rootWindow.count, sdStatus.auSectors);
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
    switchSdTrimInit(&trim, trimBits, rootWindow.first, rootWindow.count, sdStatus.auSectors);
}

void NativeSwitchBlock::trimFired(thread_call_param_t owner, thread_call_param_t)
{
    auto *self = static_cast<NativeSwitchBlock *>(owner);
    IOLockLock(self->queueLock);
    if (self->workerSleeping)
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
        if (head || unmaps || syncs || suspended || switch_now_ns() < trimQuietUntil())
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
    const uint64_t nowSave = switch_now_ns();
    const bool save =
        trimSinceSave >= kTrimCursorEveryAus || trimBootSectors >= kTrimBootSectors ||
        (trim.pending == 0u && trimSinceSave > 0 && (!trimLastSaveNs || nowSave - trimLastSaveNs >= kTrimSaveMinNs));
    const uint64_t cursor = trim.cursor;
    if (save) {
        trimSinceSave = 0;
        trimLastSaveNs = nowSave;
    }
    working = false;
    if (suspendWaiters)
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
static void syncLines(switch_dcache_op op, const SwitchAdmaTable &table, bool uncached, const uint64_t *uncachedBits,
                      uint64_t skip = 0, uint64_t skipBytes = 0)
{
    for (uint32_t i = 0; i < table.count; ++i) {
        const uint64_t pa = switchAdmaAddress(table.entries[i]);
        if (pa - skip < skipBytes)
            continue;
        if (uncached && uncachedBits && (uncachedBits[i >> 6] & (1ULL << (i & 63))) != 0)
            continue;
        switch_dcache_phys_lines(op, pa, table.entries[i].length);
    }
}

SwitchAdmaRefusal NativeSwitchBlock::buildTable(PipeSlot &slot, Request *first, uint32_t sectors, bool write)
{
    SwitchAdmaTable &table = slot.table;
    Staging &staging = slot.staging;
    table.reset();
    if (slot.tableUncached) {
        bzero(slot.descUncached, sizeof(slot.descUncached));
        slot.tableUncached = false;
    }
    staging.count = 0;
    table.limit = kAdmaDmaLimit;
    const auto pageKind = [](uint64_t page) -> uint8_t {
        const DmaPage kind = dmaPage(page);
        return kind == DmaPage::kCacheable ? 1 : (kind == DmaPage::kUncached ? 2 : 0);
    };
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): a chain always has its first request
    uint64_t lba = first->lba;
    const auto gap = [&](const Request *r) -> uint64_t {
        return (r->lba - lba) * kSector;
    };
    if (!write && slot.stages) {
        staging.reset(&table, bouncePA, slot.stages, kStages);
        table.limit = kAdmaDmaLimit;
        for (const Request *r = first; r; r = r->next) {
            SwitchAdmaRefusal why = kAdmaOk;
            if (gap(r)) {
                why = gap(r) > static_cast<uint64_t>(kGapSectors) * kSector || !sinkPA
                          ? kAdmaMissing
                          : staging.addSink(sinkPA, gap(r));
            }
            if (!why) {
                why = staging.addMemoryClassified(r->buffer, static_cast<uint64_t>(r->sectors) * kSector, pageKind,
                                                  slot.descUncached, PipeSlot::kDescWords, &slot.tableUncached);
            }
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
        if (!why) {
            why = table.addMemoryClassified(r->buffer, static_cast<uint64_t>(r->sectors) * kSector, pageKind,
                                            slot.descUncached, PipeSlot::kDescWords, &slot.tableUncached);
        }
        if (why)
            return why;
        lba = r->lba + r->sectors;
    }
    return table.bytes == static_cast<uint64_t>(sectors) * kSector && table.finish() ? kAdmaOk : kAdmaMissing;
}

void NativeSwitchBlock::prepareDirect(PipeSlot &slot, bool write)
{
#if SWITCH_STATS_IO
    const uint64_t began = switch_now_ns();
#endif
    switch_dcache_clean_lines(slot.table.entries, slot.table.count * sizeof(SwitchAdmaDescriptor));
    if (write) {
        syncLines(SWITCH_DCACHE_CLEAN, slot.table, slot.tableUncached, slot.descUncached);
    } else {
        syncLines(SWITCH_DCACHE_INVALIDATE, slot.table, slot.tableUncached, slot.descUncached, sinkPA,
                  sinkPA ? static_cast<uint64_t>(kGapSectors) * kSector : 0);
    }
    if (slot.tableUncached)
        SWITCH_SD_DEBUG_COUNT(uncachedChains);
    switch_dcache_sync();
#if SWITCH_STATS_IO
    stats.syncNs += switch_now_ns() - began;
#endif
}

bool NativeSwitchBlock::copyStaged(PipeSlot &slot)
{
#if SWITCH_STATS_IO
    const uint64_t began = switch_now_ns();
#endif
    bool ok = true;
    for (uint32_t i = 0; ok && i < slot.staging.count; ++i) {
        const Staging::Stage &stageAt = slot.staging.stages[i];
        ok = stage(stageAt.memory, static_cast<uint32_t>(stageAt.offset), bounce + stageAt.at, stageAt.length, true);
#if SWITCH_STATS_IO
        switchSdAdd(&gIo.bounceBytes, stageAt.length);
#endif
    }
#if SWITCH_STATS_IO
    stats.copyNs += switch_now_ns() - began;
#endif
    return ok;
}

void NativeSwitchBlock::finishDirect(PipeSlot &slot, bool write)
{
    if (write)
        return;
#if SWITCH_STATS_IO
    const uint64_t began = switch_now_ns();
#endif
    syncLines(SWITCH_DCACHE_INVALIDATE, slot.table, slot.tableUncached, slot.descUncached, sinkPA,
              sinkPA ? static_cast<uint64_t>(kGapSectors) * kSector : 0);
    switch_dcache_sync();
#if SWITCH_STATS_IO
    stats.syncNs += switch_now_ns() - began;
#endif
}

// The chain's bytes [from, from + length) between the requests' memory and the start of `buffer` (the bounce buffer).
// r14-sdcq: toward the requests a read chain's gaps are skipped; from the requests (a write) every byte must come from
// one (switchChainCopy).
bool NativeSwitchBlock::stageChain(Request *first, uint32_t from, uint32_t length, uint8_t *buffer, bool toClient)
{
#if SWITCH_STATS_IO
    const uint64_t began = switch_now_ns();
#endif
    const bool ok = switchChainCopy(
        first, from, length, buffer,
        [&](Request *r, uint32_t offset, uint8_t *at, uint32_t n) {
            return stage(r->buffer, offset, at, n, toClient);
        },
        toClient);
#if SWITCH_STATS_IO
    stats.copyNs += switch_now_ns() - began;
    switchSdAdd(&gIo.bounceBytes, length);
#endif
    return ok;
}

// The chain through the bounce buffer by SDMA, in commands of kSafeSectors (a chain merged for ADMA2 can be longer), each
// piece staged before or copied out after its command.
bool NativeSwitchBlock::bounceChain(Request *first, uint32_t sectors)
{
    const bool write = first->write;
    const SwitchSdWindow *const bounds = windowOf(first->shared);
    for (uint32_t done = 0; done < sectors;) {
        const uint32_t n = sectors - done < kSafeSectors ? sectors - done : kSafeSectors;
        if (write && !stageChain(first, done * kSector, n * kSector, bounce, false))
            return false;
        if (transfer(first->lba + done, n, write, bounce, bounds))
            return false;
        if (!write && !stageChain(first, done * kSector, n * kSector, bounce, true))
            return false;
        done += n;
    }
    return true;
}

// Caller holds hwLock with bus claimed; cache maintenance already executed dsb sy and Device-nGnRE writes stay ordered.
void NativeSwitchBlock::emitSlotCommand(PipeSlot &slot)
{
    if (!slot.window || !slot.window->contains(slot.lba, slot.sectors))
        panic("SwitchSD: command outside its partition lba=%llu n=%u write=%d", slot.lba, slot.sectors, slot.write);
    w32(STATUS, 0xffffffff);
    if (!admaSelected) {
        w8(HOST_CONTROL, static_cast<uint8_t>((r8(HOST_CONTROL) & ~DMA_SELECT) | DMA_ADMA2));
        admaSelected = true;
    }
    w32(DMA_LOW, static_cast<uint32_t>(slot.tablePA));
    w32(DMA_HIGH, static_cast<uint32_t>(slot.tablePA >> 32));
    w16(BLOCK_SIZE, kSector | 0x7000);
    w16(BLOCK_COUNT, static_cast<uint16_t>(slot.sectors));
    w32(ARGUMENT, static_cast<uint32_t>(slot.lba));
    if (slot.frame.argument2)
        w32(ARGUMENT2, slot.frame.argument2);
    w16(TRANSFER, slot.frame.transfer);
    w16(COMMAND, slot.frame.command);
    slot.issuedAt = switch_now_ns();
    w32(SIGNAL_ENABLE, INT_TRANSFER | ERROR_SIGNALS);
}

void NativeSwitchBlock::launchSlot(PipeSlot &slot)
{
    const SwitchSdUse use = slot.write ? SwitchSdUse::kWrite : SwitchSdUse::kRead;
    slot.irqDone = false;
    slot.timedOut = false;
    slot.fastIssued = false;
    slot.issueStep = 0;
    slot.irqStatus = 0;
    slot.irqAt = 0;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const IOInterruptState istate = IOSimpleLockLockDisableInterrupt(hwLock);
        const SwitchSdBusState state = bus.claim(r32(PRESENT), use);
        if (state == SwitchSdBusState::kFree) {
            pipe.active = &slot;
            emitSlotCommand(slot);
            IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
#if SWITCH_STATS_IO
            slot.batch.issuedNs = slot.issuedAt;
            gIo.roundTrip[kSwitchSdRtPrep].add(slot.issuedAt > slot.batch.beganNs ? slot.issuedAt - slot.batch.beganNs
                                                                                  : 0);
            if (slot.batch.wokeFromIdle)
                gIo.roundTrip[kSwitchSdRtSubmit].add(
                    slot.issuedAt > slot.batch.oldestQueuedNs ? slot.issuedAt - slot.batch.oldestQueuedNs : 0);
            if (prevIrqNs)
                gIo.roundTrip[kSwitchSdRtGap].add(slot.issuedAt > prevIrqNs ? slot.issuedAt - prevIrqNs : 0);
            prevIrqNs = 0;
#endif
            return;
        }
        IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
        if (attempt == 0 && state == SwitchSdBusState::kCardBusy && awaitRelease() == 0)
            continue;
        SWITCH_SD_DEBUG_COUNT(busRefused);
        const IOInterruptState istate2 = IOSimpleLockLockDisableInterrupt(hwLock);
        slot.issueStep = 1;
        slot.irqDone = true;
        pipe.active = &slot;
        IOSimpleLockUnlockEnableInterrupt(hwLock, istate2);
        return;
    }
}

void NativeSwitchBlock::prepareAndPublishSlot(PipeSlot &slot)
{
    Batch &b = slot.batch;
    Request *const first = b.first;
    const uint32_t sectors = b.sectors;
    const SwitchSdWindow *const bounds = windowOf(first->shared);
    if (!bounds->contains(first->lba, sectors))
        panic("SwitchSD: chain outside its partition lba=%llu n=%u", first->lba, sectors);
    uint64_t payload = 0;
    uint64_t prevEnd = first->lba;
    for (const Request *r = first; r; r = r->next) {
        if (r->shared != first->shared || !bounds->contains(r->lba, r->sectors))
            panic("SwitchSD: request outside its partition lba=%llu n=%u", r->lba, r->sectors);
        if (r->lba < prevEnd || (!first->write && r->lba - prevEnd > kGapSectors))
            panic("SwitchSD: chain order lba=%llu prev=%llu n=%u", r->lba, prevEnd, r->sectors);
        prevEnd = r->lba + r->sectors;
        payload += r->sectors;
    }
    if (prevEnd != first->lba + sectors || payload > sectors)
        panic("SwitchSD: chain span lba=%llu n=%u end=%llu", first->lba, sectors, prevEnd);
    if (first->write && payload != sectors)
        panic("SwitchSD: write chain lba=%llu n=%u carries %llu sectors of requests", first->lba, sectors, payload);
    b.gapSectors = static_cast<uint32_t>(sectors - payload);
    b.write = first->write;
#if SWITCH_SD_STATS
    b.beganNs = switch_now_ns();
#endif
    b.issued = false;
    b.issueStep = 0;
    b.why = kAdmaOk;
    const bool offline = first->shared && !filesPresent();
    b.direct = !offline && adma && (b.why = buildTable(slot, first, sectors, b.write)) == kAdmaOk;
    b.staged = b.direct && (slot.staging.count != 0u);
    b.cmd23 = b.direct && sectors > 1 && autoCmd23;

    slot.window = bounds;
    slot.lba = first->lba;
    slot.sectors = sectors;
    slot.descriptors = slot.table.count;
    slot.write = b.write;
    slot.direct = b.direct;
    slot.staged = b.staged;
    slot.cmd23 = b.cmd23;
    slot.preparedCache = false;
    slot.irqDone = false;
    slot.timedOut = false;
    slot.fastIssued = false;
    slot.issueStep = 0;
    slot.irqStatus = 0;
    slot.frame = switchSdBlockFrame(sectors, b.write, b.cmd23);

    PipeSlot &other = (&slot == &slots[0]) ? slots[1] : slots[0];
    if (!b.direct) {
#if SWITCH_STATS_IO
        prevIrqNs = 0;
#endif
        if (other.inUse) {
            pipe.stashed = &slot;
            return;
        }
        completeSlot(slot, 0);
        return;
    }
    if (other.inUse) {
        const uint64_t sinkBytes = sinkPA ? static_cast<uint64_t>(kGapSectors) * kSector : 0;
        const bool paOverlap = switchAdmaTablesOverlap(slot.table, other.table, sinkPA, sinkBytes);
        if (!SwitchSdPipeline<PipeSlot>::canOverlap(slot, other, paOverlap)) {
            pipe.stashed = &slot;
            return;
        }
    }
    prepareDirect(slot, b.write);
    slot.preparedCache = true;
    const IOInterruptState istate = IOSimpleLockLockDisableInterrupt(hwLock);
    if (pipe.active) {
        if (!pipe.active->irqDone) {
            pipe.prepared = &slot;
        } else {
            pipe.stashed = &slot;
        }
        IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
        return;
    }
    IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
    launchSlot(slot);
}

void NativeSwitchBlock::finishSlowActive(PipeSlot &slot)
{
    int step = slot.issueStep;
    uint32_t status = slot.irqStatus;
    if (!step) {
        if (slot.timedOut) {
            step = (status & INT_COMMAND) ? 6 : 7;
        } else if (status & INT_ADMA_ERROR) {
            admaErrorState = slot.irqAdmaState;
            admaErrorAddress = slot.irqAdmaAddress;
            step = kStepAdmaError;
        } else if (status & INT_ERRORS) {
            step = (status & INT_COMMAND) ? 3 : 2;
        } else {
            ended(status);
            w32(STATUS, INT_COMMAND | INT_TRANSFER);
            if (!(status & INT_COMMAND) || (r32(RESPONSE) & R1_ERRORS)) {
                step = 4;
            } else if (slot.sectors > 1) {
                if (r16(AUTO_CMD_ERROR) & 0x9f) {
                    step = 8;
                } else if (!slot.cmd23 && (r32(STOP_RESPONSE) & R1_STOP_ERRORS)) {
                    step = 9;
                }
            }
            if (!step && !bus.idle() && awaitRelease())
                step = 10;
        }
    }
    if (step)
        step = retry(step, status, slot.lba, slot.sectors, slot.write, slot.table.count, slot.window, slot.tablePA);
    completeSlot(slot, step);
}

void NativeSwitchBlock::completeSlot(PipeSlot &slot, int step)
{
    Batch &b = slot.batch;
    IOReturn result = kIOReturnSuccess;
    if (b.direct) {
#if SWITCH_STATS_IO
        if (!step && slot.irqAt && slot.issuedAt) {
            gIo.roundTrip[kSwitchSdRtWire].add(slot.irqAt > slot.issuedAt ? slot.irqAt - slot.issuedAt : 0);
            if (slot.fastIssued && prevIrqNs)
                gIo.roundTrip[kSwitchSdRtGap].add(slot.issuedAt > prevIrqNs ? slot.issuedAt - prevIrqNs : 0);
            if (lastWakeNs > slot.irqAt)
                gIo.roundTrip[kSwitchSdRtWake].add(lastWakeNs - slot.irqAt);
            prevIrqNs = slot.irqAt;
        } else {
            prevIrqNs = 0;
        }
#endif
#if SWITCH_SD_STATS
        if (b.cmd23)
            ++stats.cmd23;
        if (slot.fastIssued)
            ++stats.overlapped;
#endif
        finishDirect(slot, b.write);
        if (!step && b.staged) {
            if (!copyStaged(slot))
                result = kIOReturnIOError;
#if SWITCH_SD_STATS
            ++stats.stagedChains;
            stats.stagedBytes += slot.staging.staged;
#endif
        }
        if (step == kStepAdmaError) {
            adma = false;
            b.direct = false;
            IOLog("SwitchSD: ADMA2 off until reboot (step %d); requests go through the bounce buffer\n", step);
        } else if (step) {
            result = kIOReturnIOError;
        }
    }
    if (b.first->shared && !filesPresent()) {
        result = kIOReturnNoMedia;
    } else if (b.direct) {
#if SWITCH_SD_STATS
        stats.direct += b.count;
        stats.descriptors += slot.table.count;
#endif
    } else {
        if (!bounceChain(b.first, b.sectors))
            result = kIOReturnIOError;
#if SWITCH_SD_STATS
        stats.bounced[b.why] += b.count;
#endif
    }
    b.result = result;
#if SWITCH_SD_STATS
    account(slot);
#endif
    if (b.write) {
        IOLockLock(queueLock);
        writesDirty = true;
        --activeWrites;
        auto &fence = writes(windowOf(b.first->shared));
        fence.finish(b.count, result != kIOReturnSuccess);
        if (fence.waiters)
            IOLockWakeup(queueLock, &fence, false);
        IOLockUnlock(queueLock);
    }
    completeBatch(b);
    slot.inUse = false;
}

void NativeSwitchBlock::waitForPipeline(bool wakeOnSubmit)
{
    const IOInterruptState istate = IOSimpleLockLockDisableInterrupt(hwLock);
    if (!pipe.shouldSleep()) {
        IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
        return;
    }
    if (wakeOnSubmit && __atomic_load_n(&head, __ATOMIC_ACQUIRE) != nullptr) {
        IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
        return;
    }
    PipeSlot *const expected = pipe.active;
    const uint64_t limit = expected->write ? 5000000000ULL : 2000000000ULL;
    const uint64_t now = switch_now_ns();
    const uint64_t elapsed = now > expected->issuedAt ? now - expected->issuedAt : 0;
    const uint64_t left = elapsed < limit ? limit - elapsed : 0;
    if (!left) {
        const uint32_t s = r32(STATUS);
        w32(SIGNAL_ENABLE, 0);
        (void)pipe.noteTimeout(expected, s, now, limit);
        IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
        return;
    }
    const uint64_t waitNs = left < kInterruptCheckNs ? left : kInterruptCheckNs;
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(waitNs, &interval);
    const uint64_t deadline = mach_absolute_time() + interval;
#if DEVELOPMENT || DEBUG
    const uint32_t interruptsBefore = switch_interrupt_count(kInterrupt);
    const uint64_t filtersBefore = __atomic_load_n(&stats.interrupts, __ATOMIC_RELAXED);
#endif
    assert_wait_deadline(&workerEvent, THREAD_UNINT, deadline);
    if (wakeOnSubmit && __atomic_load_n(&head, __ATOMIC_ACQUIRE) != nullptr) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
        return;
    }
    IOSimpleLockUnlockEnableInterrupt(hwLock, istate);
    const wait_result_t result = thread_block(THREAD_CONTINUE_NULL);
    if (result == THREAD_AWAKENED) {
#if SWITCH_STATS_IO
        lastWakeNs = switch_now_ns();
#endif
        return;
    }
    const IOInterruptState istate2 = IOSimpleLockLockDisableInterrupt(hwLock);
    if (pipe.active == expected && !expected->irqDone) {
        const uint32_t awaited = INT_TRANSFER | ERROR_SIGNALS;
        const uint32_t s = r32(STATUS);
        if (s & awaited) {
#if DEVELOPMENT || DEBUG
            waitFailure.status = s;
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
#endif
            w32(SIGNAL_ENABLE, 0);
            IOSimpleLockUnlockEnableInterrupt(hwLock, istate2);
            interruptLost(awaited);
        }
        if (pipe.noteTimeout(expected, s, switch_now_ns(), limit))
            w32(SIGNAL_ENABLE, 0);
    }
    IOSimpleLockUnlockEnableInterrupt(hwLock, istate2);
}

// The stats for a chain whose command has ended. Card and copies first, completions after: busy time
// covers the card, the copies and the cache maintenance, completeNs the callbacks (IOStorageFamily/APFS work that runs on
// this thread; r12-sdio: while the next command runs, their time inside this chain's span is not this chain's busy time).
#if SWITCH_SD_STATS
void NativeSwitchBlock::account(const PipeSlot &slot)
{
    const Batch &b = slot.batch;
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
    const uint64_t now = switch_now_ns();
#if SWITCH_STATS_IO
    uint64_t idleCallback = 0;
    if (slot.direct && slot.irqAt && stats.lastCompleteEndNs > slot.irqAt) {
        const uint64_t cbStart = stats.lastCompleteStartNs > slot.irqAt ? stats.lastCompleteStartNs : slot.irqAt;
        idleCallback = stats.lastCompleteEndNs - cbStart;
    }
    const uint64_t cmdStart = (slot.direct && slot.issuedAt && (slot.fastIssued || b.beganNs < stats.lastBusyEndNs))
                                  ? slot.issuedAt
                                  : b.beganNs;
    const uint64_t cmdSpan = now > cmdStart ? now - cmdStart : 0;
    const uint64_t latency = cmdSpan > idleCallback ? cmdSpan - idleCallback : 0;
    const uint64_t busyStart = b.beganNs > stats.lastBusyEndNs ? b.beganNs : stats.lastBusyEndNs;
    const uint64_t busySpan = now > busyStart ? now - busyStart : 0;
    const uint64_t busyDelta = busySpan > idleCallback ? busySpan - idleCallback : 0;
    if (now > stats.lastBusyEndNs)
        stats.lastBusyEndNs = now;
#else
    const uint64_t latency = now > b.beganNs ? now - b.beganNs : 0;
    const uint64_t busyDelta = latency;
#endif
    stats.commands++;
    if (b.gapSectors) {
        ++stats.gapChains;
        stats.gapBytes += static_cast<uint64_t>(b.gapSectors) * kSector;
    }
    stats.busyNs += busyDelta;
    (write ? stats.writeNs : stats.readNs) += busyDelta;
#if SWITCH_STATS_IO
    gIo.command[write ? 1 : 0][switchSdSize(static_cast<uint64_t>(b.sectors) * kSector)].add(latency);
#endif
    stats.merged += b.count - 1;
    if (latency > stats.maxLatencyNs)
        stats.maxLatencyNs = latency;
}
#endif

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
#if SWITCH_STATS_IO
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
#if SWITCH_SD_STATS
            __atomic_fetch_add(&self->serviced, 1, __ATOMIC_RELAXED);
#endif
        }
    };

#if SWITCH_STATS_IO
    const uint64_t began = switch_now_ns();
#endif
    Request *const first = b.first;
    b.first = nullptr;
    switchSdPoolComplete<kCompleteChunk, Request, Done>(requestPool, first, Ops{this, b.result});
#if SWITCH_STATS_IO
    const uint64_t ended = switch_now_ns();
    stats.lastCompleteStartNs = began;
    stats.lastCompleteEndNs = ended;
    stats.completeNs += ended - began;
#endif
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
        admaSelected = false;
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
    const uint64_t pickNs = switch_now_ns();
#if SWITCH_RECORDER || SWITCH_STATS_IO
    const uint64_t headWaitedUs = (pickNs - head->queuedNs) / 1000;
    const uint64_t queuedBefore = queued;
#endif
    const uint64_t awaitedBeforeNs[2] = {writes(windowOf(false)).awaitedBeforeNs(),
                                         writes(windowOf(true)).awaitedBeforeNs()};
    switchSdAgeReplay(head, pickNs, queuedReads, queuedReplay);
    const SwitchSdPicked<Request> picked = switchSdPick(head, awaitedBeforeNs, pickNs, &pickState);
    Request *const prev = picked.prev;
    Request *first = picked.first;
#if SWITCH_SD_STATS
    if (picked.passed) {
        ++passed;
        ++passedTotal;
    }
#endif
    uint32_t sectors = first->sectors;
    uint32_t count = 1;
    const uint32_t limit = first->write ? maxWriteSectors : maxSectors;
    bool gathered = false;
    if (merge && mergeAnywhere) {
        uint64_t start = 0;
        SwitchChainBounds bounds = {};
        bool fits = false;
        if (first->write) {
            count = switchGatherWrites(head, first, limit, kMaxChain, kMergeScan, &start, &sectors, inFlight,
                                       inFlightCount, writes(windowOf(first->shared)).waiters != nullptr, &bounds);
            fits = switchChainBoundsFit(bounds, 0, 0);
        } else {
            const uint32_t maxGap = gapsOn ? kGapSectors : 0;
            const uint32_t gapBudget = gapsOn ? kGapBudget : 0;
            count = switchGatherReads(head, first, maxSectors, kMaxChain, kMergeScan, &start, &sectors, maxGap,
                                      gapBudget, &bounds);
            fits = switchChainBoundsFit(bounds, maxGap, gapBudget);
        }
        if (fits) {
            if (count == 1) {
                first->chained = false;
                if (prev) {
                    prev->next = first->next;
                } else {
                    head = first->next;
                }
                if (tail == first)
                    tail = prev;
                first->next = nullptr;
            } else {
                first = switchUnlinkChain(head, tail, count);
            }
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
        while (merge && count < kMaxChain && last->next && last->next->shared == first->shared &&
               last->next->write == first->write && last->next->lba == last->lba + last->sectors &&
               sectors + last->next->sectors <= limit && !overlapsOlder(last->next) && !overlapsInFlight(last->next)) {
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
#if SWITCH_RECORDER || SWITCH_STATS_IO
    KDBG(kSdPickKdbg, first->lba, uint64_t(sectors) | uint64_t(count) << 32,
         uint64_t(first->klass) | (prev ? 0x100u : 0u) | (first->write ? 0x200u : 0u) | queuedBefore << 16,
         headWaitedUs);
#endif
    takenLocked(first, count, pickNs);
    *chainSectors = sectors;
    *chainCount = count;
    return first;
}

// The queue counts and the per-class wait stats for a chain just unlinked (queueLock held).
void NativeSwitchBlock::takenLocked(Request *first, uint32_t count, uint64_t pickNs)
{
#if SWITCH_STATS_IO
    gIo.depthChanging(pickNs, queued);
#endif
    queued -= count;
    for (const Request *r = first; r; r = r->next) {
        if (!r->write)
            --queuedReads[r->klass];
        if (switchSdAgeable(r))
            --queuedReplay;
#if SWITCH_SD_STATS
        ClassWait &wait = waits[r->klass];
        const uint64_t waited = pickNs > r->queuedNs ? pickNs - r->queuedNs : 0;
#if SWITCH_STATS_IO
        gIo.wait[r->klass].add(waited);
#endif
        ++wait.n;
        wait.sumNs += waited;
        if (waited > wait.maxNs)
            wait.maxNs = waited;
#endif
    }
#if !SWITCH_SD_STATS
    (void)pickNs;
#endif
}

void NativeSwitchBlock::worker(void *context, wait_result_t)
{
    auto *self = static_cast<NativeSwitchBlock *>(context);
    IOLockLock(self->queueLock);
    self->workerThread = current_thread();
    IOLockUnlock(self->queueLock);
#if SWITCH_STATS_IO
    bool wokeFromIdle = false;
#endif
    for (;;) {
        PipeSlot *finished = nullptr;
        {
            const IOInterruptState istate = IOSimpleLockLockDisableInterrupt(self->hwLock);
            finished = self->pipe.takeDone();
            IOSimpleLockUnlockEnableInterrupt(self->hwLock, istate);
        }
        if (finished) {
            self->completeSlot(*finished, 0);
#if SWITCH_STATS_IO
            const uint64_t now = switch_now_ns();
            if (now - self->statsLastNs >= NativeSwitchBlock::statsEveryNs) {
                self->statsLastNs = now;
                self->prevIrqNs = 0;
                self->logThroughput();
                self->logIo(now);
            }
#endif
            continue;
        }
        PipeSlot *slow = nullptr;
        {
            const IOInterruptState istate = IOSimpleLockLockDisableInterrupt(self->hwLock);
            if (self->pipe.active && self->pipe.active->irqDone) {
                if (PipeSlot *retracted = self->pipe.retractPrepared())
                    self->pipe.stashed = retracted;
                slow = self->pipe.active;
                self->pipe.active = nullptr;
            }
            IOSimpleLockUnlockEnableInterrupt(self->hwLock, istate);
        }
        if (slow) {
            self->finishSlowActive(*slow);
            continue;
        }
        if (self->pipe.stashed) {
            bool hwIdle = false;
            {
                const IOInterruptState istate = IOSimpleLockLockDisableInterrupt(self->hwLock);
                hwIdle = (self->pipe.active == nullptr && self->pipe.done == nullptr);
                IOSimpleLockUnlockEnableInterrupt(self->hwLock, istate);
            }
            if (hwIdle) {
                PipeSlot *s = self->pipe.stashed;
                self->pipe.stashed = nullptr;
                IOLockLock(self->queueLock);
                const bool dying = self->shutdownDone;
                IOLockUnlock(self->queueLock);
                if (dying) {
                    s->batch.result = kIOReturnNotReady;
                    if (s->batch.write) {
                        IOLockLock(self->queueLock);
                        --self->activeWrites;
                        auto &fence = self->writes(self->windowOf(s->batch.first->shared));
                        fence.finish(s->batch.count, true);
                        if (fence.waiters)
                            IOLockWakeup(self->queueLock, &fence, false);
                        IOLockUnlock(self->queueLock);
                    }
                    self->completeBatch(s->batch);
                    s->inUse = false;
                    continue;
                }
                if (s->batch.first->shared && !self->filesPresent()) {
                    s->batch.direct = false;
                    self->completeSlot(*s, 0);
                } else if (self->adma && s->direct) {
                    if (!s->preparedCache) {
                        self->prepareDirect(*s, s->write);
                        s->preparedCache = true;
                    }
                    self->launchSlot(*s);
                } else {
                    s->batch.direct = false;
                    self->completeSlot(*s, 0);
                }
                continue;
            }
        }
        // Lock order: queueLock -> hwLock.
        IOLockLock(self->queueLock);
        bool hwActive = false;
        bool hasPrepared = false;
        bool hasDone = false;
        SwitchLbaRange activeRange = {0, 0};
        {
            const IOInterruptState istate = IOSimpleLockLockDisableInterrupt(self->hwLock);
            if (self->suspended) {
                if (PipeSlot *retracted = self->pipe.retractPrepared())
                    self->pipe.stashed = retracted;
            }
            hwActive = self->pipe.active != nullptr;
            hasPrepared = self->pipe.prepared != nullptr;
            hasDone = self->pipe.done != nullptr;
            if (self->pipe.active)
                activeRange = {self->pipe.active->lba, self->pipe.active->sectors};
            IOSimpleLockUnlockEnableInterrupt(self->hwLock, istate);
        }
        if (hasDone) {
            IOLockUnlock(self->queueLock);
            continue;
        }
        const bool pipeIdle = !hwActive && !hasPrepared && self->pipe.stashed == nullptr;
        if (self->suspended) {
            if (pipeIdle) {
                self->working = false;
                if (self->suspendWaiters)
                    IOLockWakeup(self->queueLock, &self->working, false);
                self->workerSleeping = true;
                while (self->suspended)
                    IOLockSleep(self->queueLock, &self->head, THREAD_UNINT);
                self->workerSleeping = false;
                IOLockUnlock(self->queueLock);
                continue;
            }
            IOLockUnlock(self->queueLock);
            self->waitForPipeline(false);
            continue;
        }
        const bool syncTurn = self->syncs != nullptr;
        if (syncTurn) {
            if (pipeIdle) {
                SyncWaiter *batch = self->syncs;
                self->syncs = nullptr;
                const bool dirty = self->writesDirty;
                self->working = true;
#if SWITCH_STATS_IO
                self->prevIrqNs = 0;
#endif
                IOLockUnlock(self->queueLock);
                const IOReturn syncResult = (!dirty || self->flushCache()) ? kIOReturnSuccess : kIOReturnIOError;
                IOLockLock(self->queueLock);
                if (dirty && syncResult == kIOReturnSuccess)
                    self->writesDirty = false;
                while (batch) {
                    SyncWaiter *nextWaiter = batch->next;
                    batch->next = nullptr;
                    batch->result = syncResult;
                    batch->finished = true;
                    batch = nextWaiter;
                }
                IOLockWakeup(self->queueLock, &self->syncs, false);
                self->working = false;
                if (self->suspendWaiters)
                    IOLockWakeup(self->queueLock, &self->working, false);
                IOLockUnlock(self->queueLock);
                continue;
            }
            IOLockUnlock(self->queueLock);
            self->waitForPipeline(false);
            continue;
        }
        const bool discardTurn =
            (self->unmaps != nullptr) && ((self->head == nullptr) || self->chainsSinceUnmap >= kUnmapEveryChains);
        if (discardTurn) {
            if (pipeIdle) {
                Unmap *job = self->unmaps;
                self->working = true;
                self->chainsSinceUnmap = 0;
#if SWITCH_STATS_IO
                self->prevIrqNs = 0;
#endif
                IOLockUnlock(self->queueLock);
                const bool doneJob = self->stepUnmap(*job);
                IOLockLock(self->queueLock);
                if (doneJob) {
                    self->unmaps = job->next;
                    job->finished = true;
                    IOLockWakeup(self->queueLock, &self->unmaps, false);
                }
                self->working = false;
                if (self->suspendWaiters)
                    IOLockWakeup(self->queueLock, &self->working, false);
                IOLockUnlock(self->queueLock);
                continue;
            }
            IOLockUnlock(self->queueLock);
            self->waitForPipeline(false);
            continue;
        }
        if (self->head && !hasPrepared && !self->pipe.stashed) {
            if (self->unmaps)
                ++self->chainsSinceUnmap;
            PipeSlot *cand = !self->slots[0].inUse ? &self->slots[0] : &self->slots[1];
            cand->batch = {};
            cand->batch.first =
                self->pickLocked(&cand->batch.sectors, &cand->batch.count, &activeRange, hwActive ? 1u : 0u);
#if SWITCH_STATS_IO
            cand->batch.wokeFromIdle = wokeFromIdle;
            wokeFromIdle = false;
            uint64_t oldest = cand->batch.first->queuedNs;
            for (const Request *r = cand->batch.first->next; r; r = r->next) {
                if (r->queuedNs < oldest)
                    oldest = r->queuedNs;
            }
            cand->batch.oldestQueuedNs = oldest;
#endif
            cand->inUse = true;
            self->working = true;
            if (cand->batch.first->write)
                ++self->activeWrites;
            IOLockUnlock(self->queueLock);
            self->prepareAndPublishSlot(*cand);
            continue;
        }
        if (pipeIdle) {
            bool trimNow = false;
            self->workerSleeping = true;
            while (!self->head && !self->unmaps && !self->syncs && !self->suspended) {
                if (self->trimRunnable()) {
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
                self->working = false;
#if SWITCH_STATS_IO
                wokeFromIdle = true;
                self->prevIrqNs = 0;
#endif
                IOLockSleep(self->queueLock, &self->head, THREAD_UNINT);
            }
            self->workerSleeping = false;
            if (trimNow) {
#if SWITCH_STATS_IO
                self->prevIrqNs = 0;
#endif
                self->trimStep();
            }
            IOLockUnlock(self->queueLock);
            continue;
        }
        const bool wakeOnSubmit = !hasPrepared && !self->pipe.stashed;
        if (wakeOnSubmit)
            __atomic_store_n(&self->pipeWaiting, true, __ATOMIC_RELEASE);
        IOLockUnlock(self->queueLock);
        self->waitForPipeline(wakeOnSubmit);
        if (wakeOnSubmit)
            __atomic_store_n(&self->pipeWaiting, false, __ATOMIC_RELEASE);
    }
}

#if SWITCH_SD_STATS
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
            at += static_cast<size_t>(
                snprintf(classes + at, sizeof(classes) - at, " passed %llu/%llu", passed, passedTotal));
#if SWITCH_STATS_IO
        // Raise calls since boot / queued requests they raised: XNU's, then SwitchCryptex's.
        if (at < sizeof(classes)) {
            snprintf(classes + at, sizeof(classes) - at, " reprio xnu %llu/%llu cryptex %llu/%llu", xnuRaiseCalls,
                     xnuRaised, cryptexRaiseCalls, cryptexRaised);
        }
#endif
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
    char extras[192] = "";
#if SWITCH_STATS_IO
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
    if (writeCacheEnabled() || stats.cacheFlushes) {
        const size_t at = strlen(extras);
        snprintf(extras + at, sizeof(extras) - at, " cache %s %llu/%llums", writeCacheEnabled() ? "on" : "off",
                 stats.cacheFlushes, stats.cacheFlushNs / 1000000);
    }
    const uint64_t fast = __atomic_load_n(&stats.fastIssued, __ATOMIC_RELAXED);
    if (fast) {
        const size_t at = strlen(extras);
        snprintf(extras + at, sizeof(extras) - at, " fast %llu", fast);
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
#endif

#if SWITCH_STATS_IO
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
#if SWITCH_SD_STATS
    logStats();
    IOLog("SwitchSD: %s present=0x%08x status=0x%08x hc=0x%02x hc2=0x%04x clock=0x%04x submitted=%llu serviced=%llu "
          "queued=%d\n",
          why, r32(PRESENT), r32(STATUS), r8(HOST_CONTROL), r16(HOST_CONTROL2), r16(CLOCK), submitted, serviced,
          static_cast<int>(head != nullptr));
#else
    IOLog("SwitchSD: %s present=0x%08x status=0x%08x hc=0x%02x hc2=0x%04x clock=0x%04x errors=%llu last=%u/0x%x "
          "queued=%d\n",
          why, r32(PRESENT), r32(STATUS), r8(HOST_CONTROL), r16(HOST_CONTROL2), r16(CLOCK), stats.errors,
          stats.lastError, stats.lastStatus, static_cast<int>(head != nullptr));
#endif
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
        admaSelected = false;
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

bool NativeSwitchBlock::enableCache(bool resume)
{
    const ExtRegBus cacheBus{this, true};
    const SwitchSdCacheOn on = switchSdEnableCache(havePerf, extInfo.perf, perf, cacheBus);
    if (on.step == SwitchSdCacheOnStep::kDone) {
        __atomic_store_n(&cacheEnabled, true, __ATOMIC_RELEASE);
        writesDirty = false;
        if (!resume)
            IOLog("SwitchSD: card volatile write cache enabled\n");
        return true;
    }
    __atomic_store_n(&cacheEnabled, false, __ATOMIC_RELEASE);
    if (on.step != SwitchSdCacheOnStep::kSkipped) {
        const uint32_t irq = on.step == SwitchSdCacheOnStep::kReadBackFailed ? initStatus : cardLastIrq;
        IOLog("SwitchSD: card volatile write cache enable failed%s (step %u, err %d, R1 0x%08x, irq 0x%08x, "
              "cache 0x%02x)\n",
              resume ? " after sleep" : "", static_cast<unsigned>(on.step), on.error, on.r1, irq,
              static_cast<unsigned>(on.cacheEnable));
    }
    return false;
}

void NativeSwitchBlock::configureCache()
{
    unsigned noCache = 0;
    const bool allowCache = !(PE_parse_boot_argn("switch-no-sd-cache", &noCache, sizeof(noCache)) && noCache);
    if (allowCache && havePerf && perf.cache && havePower && power.offNotification) {
        if (enableCache(false)) {
            wantCache = true;
            return;
        }
    }
    disableFoundCache();
}

bool NativeSwitchBlock::flushCache()
{
    if (!writeCacheEnabled())
        return true;
#if SWITCH_SD_STATS
    const uint64_t began = switch_now_ns();
#endif
    const ExtRegBus cacheBus{this, true};
    SwitchSdFlush flush = switchSdFlushCache(havePerf, extInfo.perf, cacheBus);
    if ((flush.step == SwitchSdFlushStep::kFlushFailed || flush.step == SwitchSdFlushStep::kReadBackFailed) && tuned &&
        !inShutdown) {
        retune(false);
        flush = switchSdFlushCache(havePerf, extInfo.perf, cacheBus);
    }
    if (flush.step == SwitchSdFlushStep::kDone || flush.step == SwitchSdFlushStep::kSkipped) {
#if SWITCH_SD_STATS
        ++stats.cacheFlushes;
        stats.cacheFlushNs += switch_now_ns() - began;
#endif
        return true;
    }
    const uint32_t irq = flush.step == SwitchSdFlushStep::kReadBackFailed ? initStatus : cardLastIrq;
    switch_refusal_k(switch_refusal_key_with(kFlushRefusal, static_cast<uint64_t>(flush.step)), "SwitchSD",
                     "cache flush", "card cache flush failed (step %u, err %d, R1 0x%08x, irq 0x%08x, flush 0x%02x)",
                     static_cast<unsigned>(flush.step), flush.error, flush.r1, irq,
                     static_cast<unsigned>(flush.flushCache));
    return false;
}

void NativeSwitchBlock::powerOffNotify()
{
    // SD 6.00 Figure 5-18 allows only CMD48 after POFN=1, so suppress CMD13 release fallback on CMD49.
    const ExtRegBus extBus{this, false};
    const SwitchSdPowerOff pon = switchSdPowerOffNotify(havePower, extInfo.power, power, extBus);
    if (pon.step == SwitchSdPowerOffStep::kDone) {
        if (inShutdown)
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
// controller compares it itself and moves to the next tap (or uses `manualTap` for DDR200);
// buffer-read-ready says the step ended.
void NativeSwitchBlock::tuneStep(int manualTap)
{
    if (!awaitTuningIdle(2000000000)) {
        resetLines();
        return;
    }
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);  // as hekate tunes: SDMA selected, never the data path's ADMA2
    admaSelected = false;
    w16(BLOCK_SIZE, 64);
    w16(BLOCK_COUNT, 1);
    w16(TRANSFER, 0x10);  // read, no DMA
    w32(STATUS_ENABLE, r32(STATUS_ENABLE) | INT_READ_READY);
    w16(STATUS, r16(STATUS));
    w16(CLOCK, r16(CLOCK) & ~4u);
    if (manualTap >= 0) {
        w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) & ~(1u << 17));
        w32(VENDOR_CLOCK, (r32(VENDOR_CLOCK) & ~0xff0000u) | (static_cast<uint32_t>(manualTap) & 0xffu) << 16);
        w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) | 1u << 17);
    }
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

// hekate sdmmc_tuning_execute_ddr200 (sdmmc_driver.c:732): manual 128-tap sweep in UHS_DDR50 host mode.
bool NativeSwitchBlock::tuneDdr200(uint32_t *windowOut)
{
    uint32_t results[4] = {};
    w32(VENDOR_TUNING1, 0);
    w32(VENDOR_TUNING0, (r32(VENDOR_TUNING0) & 0xffff1fffu) | 2u << 13);
    w32(VENDOR_TUNING0, (r32(VENDOR_TUNING0) & 0xffffe03fu) | 1u << 6);
    w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) | 1u << 17);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | CONTROL2_EXEC_TUNING);
    for (uint32_t i = 0; i < kSdManualTuneTaps; ++i) {
        tuneStep(static_cast<int>(i));
        if (r16(HOST_CONTROL2) & CONTROL2_TUNED)
            results[i / 32] |= 1u << (i % 32);
        if (!(r16(HOST_CONTROL2) & CONTROL2_EXEC_TUNING))
            break;
    }
    w32(STATUS, 0xffffffff);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) & ~CONTROL2_EXEC_TUNING);
    const SwitchSdTapWindow best = switchSdBestTapWindow(results, kSdManualTuneTaps, kSdMinTuneWindow);
    if (windowOut)
        *windowOut = best.window;
    if (!best.ok || !awaitTuningIdle(2000000000)) {
        w16(HOST_CONTROL2, r16(HOST_CONTROL2) & ~(CONTROL2_EXEC_TUNING | CONTROL2_TUNED));
        resetLines();
        return false;
    }
    w16(CLOCK, r16(CLOCK) & ~4u);
    w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) & ~(1u << 17));
    w32(VENDOR_CLOCK, (r32(VENDOR_CLOCK) & ~0xff0000u) | (best.tap & 0xffu) << 16);
    w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) | 1u << 17);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | CONTROL2_TUNED);
    w16(CLOCK, r16(CLOCK) | 4);
    (void)r16(CLOCK);
    return true;
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
    if (ddr200) {
        uint32_t window = 0;
        if (!slower && tuneDdr200(&window)) {
            IOLog("SwitchSD: DDR200 re-tuned at %u kHz, tap %u (window %u)\n", busKHz, (r32(VENDOR_CLOCK) >> 16) & 0xff,
                  window);
            return;
        }
        // Step down from DDR200 to SDR104 without cutting card power (preserving any dirty volatile write cache).
        setTiming(UHS_DDR50, false, SOURCE_PLLP, kPllpDivider48MHz, 1);
        (void)switchFunction(true, 1, 0);
        (void)switchFunction(true, 0, UHS_SDR104);
        ddr200 = false;
        ddr200Allowed = false;
        if (clockAndTune(0)) {
            IOLog("SwitchSD: stepped down from DDR200 to SDR104 %u kHz, tap %u\n", busKHz,
                  (r32(VENDOR_CLOCK) >> 16) & 0xff);
            return;
        }
        panic("SwitchSD: DDR200 step-down to SDR104 found no sampling window (hc2 0x%04x, %llu errors); "
              "boot with switch-no-sd-ddr200",
              r16(HOST_CONTROL2), stats.errors);
    }
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
    unsigned noDdr200 = 0;
    ddr200Allowed = !(PE_parse_boot_argn("switch-no-sd-ddr200", &noDdr200, sizeof(noDdr200)) && noDdr200);
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
    ddr200 = false;
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
    } else if (haveCid) {
        uint8_t currentCid[16];
        readR2(currentCid);
        if (memcmp(currentCid, cid, 15))
            fail("boot card changed", 0);
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
    const uint32_t cmdSystem = bounce[11] | static_cast<uint32_t>(bounce[10]) << 8;
    const uint32_t power = bounce[7] | static_cast<uint32_t>(bounce[6]) << 8;
    const char *limit = "";
    uint32_t ddr200Window = 0;
    if (lowVoltage && ddr200Allowed && (cmdSystem & (1u << kDdr200CmdSys)) && (r32(CAPABILITIES_HI) & 4)) {
        if (switchFunction(true, 3, power & 2 ? 1 : 0)) {
            limit = ", power limit unchanged";
        } else {
            limit = ((static_cast<uint32_t>(bounce[15]) >> 4) & 0xf) == 1 ? ", 1.44 W" : ", 0.72 W";
        }
        const int checkErr = switchFunction(false, 1, kDdr200CmdSys);
        if (!checkErr && ((bounce[16] >> 4) & 0xf) == kDdr200CmdSys &&
            (static_cast<uint32_t>(bounce[0]) << 8 | bounce[1]) <= 800) {
            const int setErr = switchFunction(true, 1, kDdr200CmdSys);
            if (!setErr && ((bounce[16] >> 4) & 0xf) == kDdr200CmdSys) {
                setTiming(UHS_DDR50, false, SOURCE_PLLC4_OUT0, kDdr200CarDivider, kDdr200SdhciDivider);
                const bool tuneOk = tuneDdr200(&ddr200Window);
                uint32_t statusR1 = 0;
                const int statusErr = tuneOk ? cardCommand(13, rca << 16, RSP_R1, &statusR1) : -1;
                if (tuneOk && !statusErr && !(statusR1 & R1_ERRORS) && ((statusR1 >> 9) & 0xf) == 4) {
                    ddr200 = true;
                    tuned = true;
                    speed = 0;
                    busMode = "DDR200";
                    busKHz = kDdr200KHz;
                } else {
                    IOLog("SwitchSD: DDR200 rejected (window %u < %u, CMD13 err %d); power-cycling to SDR104\n",
                          ddr200Window, kSdMinTuneWindow, statusErr);
                    ddr200Allowed = false;
                    if (irqNub)
                        irqNub->disableInterrupt(0);
                    powerOffCard();
                    powerOnController();
                    if (irqNub)
                        irqNub->enableInterrupt(0);
                    return reinitialize(PLLP_KHZ * 2 / (kPllpDivider48MHz + 2), 0xc0 | kLdo3V3, true, resume);
                }
            }
        }
    }
    if (!tuned && lowVoltage && (access & 1u << UHS_SDR104)) {
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
        if (ddr200) {
            IOLog("SwitchSD: bus now %s %u kHz %s V 4-bit%s, tap %u (window %u), RCA 0x%04x, %llu ms\n", busMode,
                  busKHz, lowVoltage ? "1.8" : "3.3", limit, (r32(VENDOR_CLOCK) >> 16) & 0xff, ddr200Window, rca,
                  (switch_now_ns() - start) / 1000000);
        } else {
            IOLog("SwitchSD: bus now %s %u kHz %s V 4-bit%s, tap %u%s, RCA 0x%04x, %llu ms\n", busMode, busKHz,
                  lowVoltage ? "1.8" : "3.3", limit, (r32(VENDOR_CLOCK) >> 16) & 0xff, tuned ? " (tuned)" : "", rca,
                  (switch_now_ns() - start) / 1000000);
        }
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
    admaSelected = false;
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
    (void)syncLocked(&rootWindow, false);
    if (filesPresent())
        (void)syncLocked(&filesWindow, false);
    suspended = true;
    IOLockWakeup(queueLock, &rootWrites, false);
    IOLockWakeup(queueLock, &filesWrites, false);
    thread_wakeup(&workerEvent);
    ++suspendWaiters;
    while (working)
        IOLockSleep(queueLock, &working, THREAD_UNINT);
    --suspendWaiters;
    const bool dirty = writesDirty;
    const bool saveCursor = trimSinceSave > 0;
    const uint64_t cursor = trim.cursor;
    if (saveCursor)
        trimSinceSave = 0;
    IOLockUnlock(queueLock);
    if (saveCursor)
        saveTrimCursor(cursor);
    if (writeCacheEnabled() && dirty && flushCache()) {
        IOLockLock(queueLock);
        writesDirty = false;
        IOLockUnlock(queueLock);
    }
    powerOffNotify();
    dmaSuspend();
    __atomic_store_n(&cardDetectArmed, false, __ATOMIC_RELEASE);
    if (cdNub)
        cdNub->disableInterrupt(0);
    if (irqNub)
        irqNub->disableInterrupt(0);
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
    admaSelected = false;
    w32(DMA_LOW, 0);
    w32(DMA_HIGH, 0);
    for (PipeSlot &slot : slots)
        slot.table.reset();
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
#if SWITCH_STATS_IO
        gIo.depthChanging(switch_now_ns(), queued);
#endif
        queued = 0;
        for (unsigned c = 0; c < kClasses; ++c)
            queuedReads[c] = 0;
        queuedReplay = 0;
        uint32_t rootAbandoned = 0, filesAbandoned = 0;
        for (const Request *r = abandoned; r; r = r->next) {
            if (r->write)
                ++(r->shared ? filesAbandoned : rootAbandoned);
        }
        if (rootAbandoned)
            rootWrites.finish(rootAbandoned, true);
        if (filesAbandoned)
            filesWrites.finish(filesAbandoned, true);
    }
    IOLockWakeup(queueLock, &rootWrites, false);
    IOLockWakeup(queueLock, &filesWrites, false);
    if (SyncWaiter *s = syncs) {
        syncs = nullptr;
        while (s) {
            SyncWaiter *next = s->next;
            s->next = nullptr;
            s->result = kIOReturnNotReady;
            s->finished = true;
            s = next;
        }
        IOLockWakeup(queueLock, &syncs, false);
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
    __atomic_store_n(&cardDetectArmed, false, __ATOMIC_RELEASE);
    if (cdNub) {
        cdNub->disableInterrupt(0);
        cdNub->unregisterInterrupt(0);
        cdNub = nullptr;
    }
    if (queueLock) {
        IOLockLock(queueLock);
        suspended = true;
        shutdownDone = true;
        thread_wakeup(&workerEvent);
        ++suspendWaiters;
        while (working)
            IOLockSleep(queueLock, &working, THREAD_UNINT);
        --suspendWaiters;
        Request *const abandoned = drainQueueLocked();
        IOLockUnlock(queueLock);
        if (abandoned) {
            Batch b = {};
            b.first = abandoned;
            b.result = kIOReturnNotReady;
            completeBatch(b);
        }
    }
    if (irqNub) {
        irqNub->disableInterrupt(0);
        irqNub->unregisterInterrupt(0);
        irqNub = nullptr;
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
    thread_wakeup(&workerEvent);
    ++suspendWaiters;
    while (working)
        IOLockSleep(queueLock, &working, THREAD_UNINT);
    --suspendWaiters;
    const bool saveCursor = trimSinceSave > 0;
    const uint64_t cursor = trim.cursor;
    if (saveCursor)
        trimSinceSave = 0;
    Request *const abandoned = drainQueueLocked();
    IOLockUnlock(queueLock);
    if (saveCursor)
        saveTrimCursor(cursor);
    __atomic_store_n(&cardDetectArmed, false, __ATOMIC_RELEASE);
    if (cdNub)
        cdNub->disableInterrupt(0);
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
    if (writeCacheEnabled() && writesDirty && flushCache())
        writesDirty = false;
    powerOffNotify();
    dmaSuspend();
    if (irqNub)
        irqNub->disableInterrupt(0);
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
    if (cdNub) {
        __atomic_store_n(&cardDetectArmed, true, __ATOMIC_RELEASE);
        cdNub->enableInterrupt(0);
        checkCardPresent("resume");
    }
    powerOnController();
    if (irqNub)
        irqNub->enableInterrupt(0);
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
    verifyFilesCard();
    dmaResume();
    if (wantCache)
        (void)enableCache(true);
    IOLockLock(queueLock);
    suspended = false;
    if (workerSleeping)
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
    for (PipeSlot &slot : slots) {
        if (!switch_dma_alloc(&slot.tableMemory, kAdmaTableBytes, 0x00000000ffffffffULL & ~(kAdmaPage - 1)))
            return "no memory for the descriptor table";
        slot.table.entries = reinterpret_cast<SwitchAdmaDescriptor *>(slot.tableMemory.va);
        slot.tablePA = slot.tableMemory.pa;
        if (slot.tablePA & (kAdmaPage - 1))
            return "the descriptor table is not page-aligned below 4 GiB";
        slot.table.capacity = kAdmaDescriptors;
        slot.table.limit = kAdmaDmaLimit;
        slot.stages = static_cast<Staging::Stage *>(IOMalloc(kStages * sizeof(Staging::Stage)));
    }
    return nullptr;
}

bool NativeSwitchBlock::setup()
{
#if SWITCH_STATS_IO
    setupBeganNs = switch_now_ns();
#endif
    regs = switch_mmio_map(kControllerPA, 0x1000);
    mc = switch_mmio_map(kMemoryControllerPA, 0x1000);
    // CAR and APB_MISC/pinmux serve the SDR104 bring-up only; without them it is skipped.
    car = switch_mmio_map(kCarPA, 0x1000);
    apb = switch_mmio_map(kApbMiscPA, 0x4000);
    queueLock = IOLockAlloc();
    hwLock = IOSimpleLockAlloc();
    if (!regs || !mc || !queueLock || !hwLock) {
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
    admaSelected = false;
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
    if (!haveCid)
        haveCid = switch_sd_boot_card_cid(cid);
    identify();
    configureCache();
    SwitchSdGptArray array;
    if (!switchSdGptArray(header, &array)) {
        IOLog("SwitchSD: invalid GPT header\n");
        return false;
    }
    const uint32_t entrySectors = switchSdGptArraySectors(array);
    if (!entrySectors)
        return false;
    static uint8_t entries[kSwitchSdGptArrayBytes];
    for (uint32_t done = 0; done < entrySectors;) {
        const uint32_t n = entrySectors - done > 64 ? 64 : entrySectors - done;
        if (transfer(array.lba + done, n, false, entries + static_cast<size_t>(done) * kSector)) {
            IOLog("SwitchSD: GPT entry read failed\n");
            return false;
        }
        done += n;
    }
    const SwitchSdRoot root = switchSdFindRoot(entries, array, kApfsType, kPartitionName);
    rootWindow = root.window;
    if (!root.unique()) {
        IOLog("SwitchSD: expected one APFS or \"%s\" partition, found %u\n", kPartitionName, root.found);
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
              slots[0].stages ? ", refused segments staged alone" : "",
              autoCmd23 ? "Auto CMD23" : "Auto CMD12 (no CMD23 in SCR)",
              gapsOn ? "on" : "off (no sink page below 4 GiB)");
    }

    const int lookup = findFiles(&filesSlice);
    const int files = switchSdFilesStatus(lookup, haveCid, cid, filesSlice.cid);
    if (files == SWITCH_GPT_OK) {
        if (!haveCid) {
            memcpy(cid, filesSlice.cid, sizeof(cid));
            haveCid = true;
        }
        filesWindow = {filesSlice.first_sector, filesSlice.sector_count};
        __atomic_store_n(&filesOnline, true, __ATOMIC_RELEASE);
    } else {
        IOLog("SwitchSD: no shared SD volume (%s)\n", switchSdFilesReason(files));
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
    startCardDetect();
    return true;
}

int NativeSwitchBlock::findFiles(switch_sd_files_slice *slice)
{
    const auto read = [](void *arg, uint64_t lba, uint8_t *sector) {
        return static_cast<NativeSwitchBlock *>(arg)->transfer(lba, 1, false, sector);
    };
    return switch_sd_files_volume(read, this, rootWindow.first, rootWindow.count, slice);
}

void NativeSwitchBlock::verifyFilesCard()
{
    if (!filesPresent())
        return;
    switch_sd_files_slice current = {};
    if (findFiles(&current) == SWITCH_GPT_OK && current.first_sector == filesWindow.first &&
        current.sector_count == filesWindow.count && current.exfat == filesSlice.exfat &&
        !memcmp(current.unique_guid, filesSlice.unique_guid, sizeof(current.unique_guid)))
        return;
    IOLockLock(queueLock);
    __atomic_store_n(&filesOnline, false, __ATOMIC_RELEASE);
    IOLockWakeup(queueLock, &filesWrites, false);
    removeFilesIfOfflineLocked();
    IOLockUnlock(queueLock);
    IOLog("SwitchSD: shared SD volume no longer matches its boot geometry\n");
}

void NativeSwitchBlock::removeFilesIfOfflineLocked()
{
    if (filesPresent() || !filesRemoval)
        return;
    thread_call_enter1(filesRemoval, filesRemoval);
    filesRemoval = nullptr;
}

void NativeSwitchBlock::publishFiles()
{
    if (!filesPresent())
        return;
    const auto publish = [](thread_call_param_t arg, thread_call_param_t call) {
        static_cast<NativeSwitchBlock *>(arg)->publishFilesNow();
        thread_call_free(static_cast<thread_call_t>(call));
    };
    thread_call_t call = thread_call_allocate(publish, this);
    if (!call)
        return;
    // Wait until root's whole-disk IOMedia has claimed disk0 before publishing the shared volume.
    const auto matched = [](void *target, void *ref, IOService *client, IONotifier *notifier) -> bool {
        auto *self = static_cast<NativeSwitchBlock *>(target);
        const IOService *media = client ? client->getProvider() : nullptr;
        const IOService *driver = media ? media->getProvider() : nullptr;
        if (!driver || driver->getProvider() != self->device ||
            media->getProperty(kIOMediaWholeKey) != kOSBooleanTrue) {
            return true;
        }
        if (__atomic_exchange_n(&self->filesPublishQueued, true, __ATOMIC_ACQ_REL))
            return true;
        notifier->remove();
        thread_call_enter1(static_cast<thread_call_t>(ref), ref);
        return false;
    };
    OSDictionary *match = IOService::serviceMatching("IOMediaBSDClient");
    IONotifier *notifier =
        match ? IOService::addMatchingNotification(gIOFirstPublishNotification, match, matched, this, call) : nullptr;
    OSSafeReleaseNULL(match);
    if (!notifier)
        thread_call_free(call);
}

void NativeSwitchBlock::publishFilesNow()
{
    if (!filesPresent())
        return;
    auto *sharedDevice = OSTypeAlloc(NativeSwitchBlockDevice);
    if (!sharedDevice)
        return;
    if (sharedDevice->init(static_cast<OSDictionary *>(nullptr)) && sharedDevice->configure(this, &filesWindow, true) &&
        sharedDevice->attach(this)) {
        // Removal invokes filesystem clients, which may need our worker to finish older requests.
        const auto remove = [](thread_call_param_t device, thread_call_param_t call) {
            auto *retired = static_cast<NativeSwitchBlockDevice *>(device);
            retired->terminate();
            retired->release();
            thread_call_free(static_cast<thread_call_t>(call));
        };
        thread_call_t removal = thread_call_allocate(remove, sharedDevice);
        if (removal && sharedDevice->start(this)) {
            sharedDevice->retain();
            sharedDevice->registerService();
            IOLockLock(queueLock);
            filesRemoval = removal;
            removeFilesIfOfflineLocked();
            IOLockUnlock(queueLock);
#if DEVELOPMENT || DEBUG
            IOLog("SwitchSD: registered external %s volume at %llu for %llu sectors\n",
                  filesSlice.exfat ? "exFAT" : "FAT32", filesWindow.first, filesWindow.count);
#endif
        } else {
            if (removal)
                thread_call_free(removal);
            sharedDevice->detach(this);
        }
    }
    sharedDevice->release();
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

#if SWITCH_STATS_IO
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
        !device->configure(owner, &owner->rootWindow, false) || !device->attach(owner) || !device->start(owner)) {
        // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): a half-published device is kept, never freed
        IOLog("SwitchSD: device start failed\n");
        return gPublishStatus;
    }
    owner->device = device;
    gOwner = owner;
    // High priority runs after other platform halt/restart actions, right before SwitchPMIC cuts power.
    owner->setProperty("IOPlatformHaltRestartAction", 1000000u, 32);
    owner->registerService();
#if SWITCH_STATS_IO
    owner->setupEndedNs = switch_now_ns();
    registerSdSysctl();
#endif
    device->registerService();
    device->release();
    owner->publishFiles();
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
    *sectors = gOwner->rootWindow.count;
    *maxReadSectors = gOwner->maxSectors;
    return gOwner->device;
}

// SwitchCryptex's raise of the SD reads under a window fill a foreground read joined: the root window's doSetPriority,
// counted apart from XNU's.
extern "C" IOReturn switch_block_raise(const IOBlockStorageDeviceExtent *extents, uint32_t count,
                                       IOStoragePriority priority)
{
    if (!gOwner || !gOwner->device)
        return kIOReturnNotReady;
    return gOwner->device->raise(extents, count, priority, true);
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
        *bytes = gOwner->rootWindow.count * kSector;
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
