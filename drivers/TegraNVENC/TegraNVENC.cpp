// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 NVENC (video encoder 5.0, a falcon behind host1x): bring-up, the job engine and sleep. tegra_nvenc_probe()
// runs the whole bring-up by default on its own thread:
//   E0  host1x (drivers/TegraHost1x, shared with NVDEC)
//   E1  NVENC's SMMU address space (TegraNVENCMemory.cpp), power partition, clock, reset, falcon firmware loaded and
//       started
//   E2  proof of life through host1x channel 0 in NVENC's class: an IMMEDIATE syncpoint increment (the engine's host
//       interface answers) then an OP_DONE one (the falcon firmware answers), each completed by interrupt
//   E3  the TegraNVENC service and its user client published (TegraNVENCUserClient.cpp)
// One line per stage ("TegraNVENC: E1 ok ..."), or the failing stage's FAIL line and nothing after it. Until E3
// publishes, every failure is that one line: the engine is put back in reset with its clock off, its address space is
// given back and nothing is published, so VideoToolbox has no hardware encoder. From E3 on a job past its bound is
// cancelled with one line (channel torn down, engine reset and rebooted, the jobs in flight failed) and the next job
// runs; a lost syncpoint interrupt panics in TegraHost1x. switch-no-nvdec turns it off.
//
// A CPU-written input picture is 16x16 tiles with chroma at the surface config's offset (TegraNVENCInfo): the layout
// and chroma reading the unit encoded IPCM and IDR pictures from exactly on every boot of 11-06 (r30 archives, "E3 IPCM
// ok input layout tiled 16x16, chroma offset"). A submit that names an IOSurface has VIC write it into the input buffer
// block linear first (TegraNVENCSource.cpp), the job's stream waiting for VIC's fence.
//
// Jobs (tegra_nvenc_run): host1x channel 0 in NVENC's class (0x21), each method written through the engine's THI
// METHOD0/METHOD1 pair as NVDEC's, the picture setup from a kernel-only slot, then an OP_DONE increment of NVENC's
// syncpoint: the job's fence. No job inherits an address from another client's: the falcon OS zeroes every method word
// after each job (nvenc.bin 0xb28-0xb51), and the kernel writes the addresses of features no submit uses as well. At
// most 4 jobs are queued, which bounds the push buffer NVENC shares with NVDEC (8 x 197 + 4 x 190 words < 4096) and the
// 8 setup slots; the OS takes a queued job's methods into its own queue while an app runs and applies them only after
// it (0x13b-0x1a1, 0x20e-0x235), so they cannot change the running job. Every job runs with the GP timer on (the
// checker requires it); both apps arm it with width x height x 24 cycles, at most 0x1600000 (H.264 0x3217-0x3256, H.265
// 0x1f7e-0x1fbd), and neither reads the setup's gpTimer_timeout_val. Waits are bounded by kJobBoundUs per queued job
// whatever a client asks for, and the largest picture the checker admits (36864 macroblocks) encodes well inside it.
// The GP timer does not end a stalled job: its interrupt only records an error, and either app's end-of-picture spin
// waits for macroblocks or CTBs the engine never reports (tegra_nvenc_app_dump), so the kernel's bound is what ends it.
//
// Firmware: linux-firmware's nvidia/tegra210/nvenc.bin, byte-identical to L4T's nvhost_nvenc050.fw (firmware/SOURCE),
// booted non-securely as L4T's nvhost and TegraNVDEC boot theirs.
//
// Sources (Linux's drivers are GPL-2.0-only, so this follows them without copying):
//   tegra210.dtsi nvenc@544c0000 (0x40000; CAR clock and reset 219; power domain pd_nvenc = PMC partition "mpe", 6),
//   clk-tegra-periph.c (CLK_SOURCE_NVENC 0x6a0, the NVDEC/NVJPG mux: PLLP is code 4), clk-tegra210.c
//   L4T r32.7.4 powergate-t21x.c (the MPE partition, its MC client flush and release, the MBIST workaround on T210
//   before B01 only) and t210/cg_regs.c (the prod registers), through SwitchEnginePower.h,
//   memory/tegra/tegra210.c (MC client hot reset NVENC: control 0x200 bit 11; SMMU ASID NVENC 0x264),
//   include/linux/host1x.h (HOST1X_CLASS_NVENC 0x21), soc/tegra/pmc.c tegra_powergate_power_up, drm/tegra/falcon.c
//   falcon_boot (shared with TegraNVDEC.cpp), L4T nvhost t210.c (NVENC 5.0, nvhost_nvenc050.fw).
//   Clock: NVDEC's (kits/SwitchCAR/SwitchEngineClock.h: PLLC3 at the 637 mV point, or PLLP's 408 MHz); the T210B01
//   core DVFS table gives NVENC and NVDEC the same points.
// Waits on hardware sequencing with no interrupt of its own (partition status, IMEM/DMEM scrub, the falcon's DMA
// queue) are bounded reads, as in TegraNVDEC; every completion of work is a host1x syncpoint interrupt.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include "../TegraHost1x/TegraHost1x.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../../kits/SwitchBootArgs/SwitchBootArgs.h"
#include "TegraNVENC.h"
#include "TegraNVENCCheck.h"
#include "TegraNVENCInputs.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchEngineClock.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchEngineGate/SwitchEngineGateKernel.h"
#include "../../kits/SwitchEngineGate/SwitchEnginePower.h"
#include "../../kits/SwitchFalcon/SwitchFalcon.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" {
extern const uint8_t tegra_nvenc_fw_start[] __asm__("_tegra_nvenc_fw_start");
extern const uint8_t tegra_nvenc_fw_end[] __asm__("_tegra_nvenc_fw_end");
}

namespace {
constexpr uint64_t kNvencPA = 0x544c0000, kNvencSize = 0x40000;
constexpr uint32_t kClass = TEGRA_HOST1X_CLASS_NVENC;
constexpr uint32_t kThiMethod0 = 0x40;  // THI METHOD0 (offset) then METHOD1 (data)
constexpr uint64_t kMcPA = 0x70019000;
constexpr uint32_t kPartition = 6;  // TEGRA_POWERGATE_MPE
constexpr unsigned kClock = 219;    // TEGRA210_CLK_NVENC: CAR bank Y bit 27, also its reset

enum : uint32_t {  // clock and reset controller
    CLK_OUT_ENB_Y = 0x298,
    RST_DEVICES_Y = 0x2a4,
    LVL2_CLK_GATE_OVRE = 0x554,
    CLK_SOURCE_NVENC = 0x6a0,
    OVRE_MPE_MBIST = 1u << 29,
};

enum : uint32_t {
    PWRGATE_TOGGLE = 0x30,
    PWRGATE_STATUS = 0x38,
};

enum : uint32_t {
    MC_HOTRESET_CTRL = 0x200,
    MC_HOTRESET_STATUS = 0x204,
    MC_HOTRESET_NVENC = 1u << 11,
};

volatile uint8_t *gNvenc, *gMc;
uint32_t gClockKHz;
switch_dma_buffer gFirmware;  // stays: the falcon OS loads its apps from it through DMATRFBASE
switch_falcon_image gImage;
char gLine[256];

// Address methods of features no submit turns on, written 0 on every job: the value an instance starts with and the
// one NVIDIA's own T210 driver leaves them at (its captured streams set none of them). The firmware takes a non-zero
// address as the feature's data being there; 0 also means no job keeps another client's address in them.
constexpr uint16_t kUnusedMethods[] = {
    NVC5B7_SET_IN_RCDATA,
    NVC5B7_SET_IN_CEAHINTS_DATA,
    NVC5B7_SET_IN_QP_MAP,
    NVC5B7_SET_IN_PARTITION_BUF,
};

uint32_t gLatched;  // tegra_nvenc_latch's: bit k, latched method k held a client's address since the falcon booted

// The job engine. kJobBoundUs: the GP timer's 329 ms plus margin; a wait covers at most kInFlight queued jobs. The
// scratch buffer takes a picture's colocated output (64 bytes a macroblock) when it stands in for a latched address.
constexpr uint32_t kInFlight = 4, kSetupSlots = 8, kSetupSlotBytes = TEGRA_NVENC_MAX_SETUP;
constexpr uint32_t kSinkBytes = TEGRA_NVENC_MAX_MBS * sizeof(nvenc_h264_coloc_mb_s);
constexpr uint32_t kJobBoundUs = 500000, kWaitBoundUs = kInFlight * kJobBoundUs;
IOLock *gEngineLock;
uint32_t gSyncpt, gBase, gIssued;  // job n's fence is gBase + n
uint32_t gFailedThrough;           // with gFailed: every fence up to this one failed (wrapping compare)
uint8_t gJobCodec[kSetupSlots];    // by setup slot: the codec of the job using it
bool gUp, gWedged, gSuspended, gFailed;
switch_dma_buffer gSetup, gSink;  // setup slots and the scratch buffer, never mapped into a client
SwitchEngineGate<SwitchEngineGateKernel> gGate;
TegraNVENCInputs gInputs;
static_assert(TegraNVENCInputs::kProducers == TEGRA_NVENC_ORDER_WAITS + 1, "the order's producers and VIC's input");

const char *falconState()
{
    static char text[160];
    return switch_falcon_state(gNvenc, text, sizeof(text));
}

// The MPE partition and NVENC's memory client in L4T's order (SwitchEnginePower.h), then its clock-gating prod registers
// (t210/cg_regs.c t21x_msenc_gating_registers). The MBIST workaround (LVL2 bit 29, no slcg clock) runs only on T210
// before B01, as powergate-t21x.c's.
const SwitchEnginePower kPower = {kPartition,         kClock,         MC_HOTRESET_CTRL, MC_HOTRESET_STATUS,
                                  MC_HOTRESET_NVENC,  OVRE_MPE_MBIST, nullptr,          0,
                                  tegra_mc_hot_reset, tegra_smmu_read};
constexpr SwitchEngineProd kProd[] = {
    {0xe00, 0},            // THI_CLK_OVERRIDE
    {0x88, 0},             // THI_SLCG_OVERRIDE_HIGH_A
    {0x8c, 0},             // THI_SLCG_OVERRIDE_LOW_A
    {0x10a0, 0},           // FALCON_CGCTL
    {0x1804, 0},           // TFBIF_MCCIF_FIFOCTRL
    {0x117c, 0x30008},     // FALCON_CG2
    {0x2200, 0x800040},    // NVENC_CG1
    {0x2204, 0x10000000},  // NVENC_CG2
    {0x2208, 0},           // NVENC_CG3
};

bool powerUp(bool *wasOn)
{
    switch_car_write(CLK_SOURCE_NVENC, switch_engine_clock_source(SWITCH_ENGINE_BUS_C3, &gClockKHz));
    IODelay(2);
    if (!switch_engine_power_up(kPower, wasOn)) {
        snprintf(gLine, sizeof(gLine),
                 "E1 FAIL partition %u did not come up (PWRGATE_STATUS %#x CLAMP_STATUS %#x CLK_OUT_ENB_Y %#x "
                 "RST_DEVICES_Y %#x)",
                 kPartition, switch_pmc_read(PWRGATE_STATUS), switch_pmc_read(0x2c), switch_car_read(CLK_OUT_ENB_Y),
                 switch_car_read(RST_DEVICES_Y));
        return false;
    }
    switch_engine_prod(gNvenc, kProd, sizeof(kProd) / sizeof(kProd[0]));
    return true;
}

// The whole image goes into one contiguous buffer below 4 GiB, cleaned to DRAM: the falcon fetches it by physical
// address (SMMU bypassed), now for its OS and later for the apps.
bool loadImage()
{
    const size_t length =
        reinterpret_cast<uintptr_t>(tegra_nvenc_fw_end) - reinterpret_cast<uintptr_t>(tegra_nvenc_fw_start);
    if (!switch_falcon_parse(tegra_nvenc_fw_start, length, &gImage)) {
        snprintf(gLine, sizeof(gLine), "E1 FAIL embedded firmware is not a falcon v1 image (%lu bytes)",
                 static_cast<unsigned long>(length));
        return false;
    }
    if (!gFirmware.memory) {  // kept for resume: the falcon is reloaded from it after SC7
        if (!switch_dma_alloc(&gFirmware, gImage.size)) {
            snprintf(gLine, sizeof(gLine), "E1 FAIL no contiguous %u-byte firmware buffer below 4 GiB", gImage.size);
            return false;
        }
        memcpy(gFirmware.va, tegra_nvenc_fw_start, gImage.size);
        switch_dcache_clean(gFirmware.va, gImage.size);
    }
    return true;
}

// falcon_boot, minus its final idle poll: that the firmware runs is shown by E2's OP_DONE increment instead.
bool bootFalcon()
{
    gLatched = 0;
    if (!loadImage())
        return false;
    switch (switch_falcon_load(gNvenc, gFirmware.pa, &gImage)) {
    case SWITCH_FALCON_SCRUBBING:
        snprintf(gLine, sizeof(gLine), "E1 FAIL IMEM/DMEM scrubbing did not end (DMACTL %#x)",
                 switch_mmio_read32(gNvenc, SWITCH_FALCON_DMACTL));
        return false;
    case SWITCH_FALCON_DMA:
        snprintf(gLine, sizeof(gLine), "E1 FAIL firmware DMA did not finish (DMATRFCMD %#x)",
                 switch_mmio_read32(gNvenc, SWITCH_FALCON_DMATRFCMD));
        return false;
    case SWITCH_FALCON_OK:
        break;
    }
    switch_falcon_start(gNvenc);
    return true;
}

// Channel 0 in NVENC's class: an IMMEDIATE increment is done by the engine's host interface on arrival, an OP_DONE
// one only once the falcon reports the methods before it done.
int answer(uint32_t id, uint32_t from, const char **stage)
{
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(kClass, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, id),
    };
    int ret = switch_host1x_job_copy(TEGRA_HOST1X_CHANNEL_NVENC, stream, 3, id, from + 2);
    *stage = "submit";
    if (!ret) {
        *stage = "host interface (IMMEDIATE)";
        ret = tegra_host1x_syncpt_wait(id, from + 1, 100000);
    }
    if (!ret) {
        *stage = "falcon (OP_DONE)";
        ret = tegra_host1x_syncpt_wait(id, from + 2, 500000);
    }
    return ret;
}

bool proofOfLife(uint64_t *us, uint32_t *id, uint32_t *from)
{
    if (tegra_host1x_syncpt_alloc(id)) {
        snprintf(gLine, sizeof(gLine), "E2 FAIL no syncpoint left");
        return false;
    }
    *from = tegra_host1x_syncpt_read(*id);
    const uint64_t began = switch_now_us();
    const char *stage = nullptr;
    const int ret = answer(*id, *from, &stage);
    *us = switch_now_us() - began;
    if (ret) {
        char host[200];
        tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_NVENC, host, sizeof(host));
        snprintf(gLine, sizeof(gLine), "E2 FAIL %s %s syncpoint %u (%u->%u, %d); %s; %s", stage,
                 ret == TEGRA_HOST1X_ELOST ? "incremented, but no interrupt came for" : "did not increment", *id, *from,
                 tegra_host1x_syncpt_read(*id), ret, falconState(), host);
        return false;
    }
    return true;
}

void engineSuspend();
void engineResume();
int gateOn(void *);
int gateOff(void *, bool);

bool drained()
{
    return static_cast<int32_t>(tegra_host1x_syncpt_read(gSyncpt) -
                                (gBase + __atomic_load_n(&gIssued, __ATOMIC_ACQUIRE))) >= 0;
}

// A bounded wait for a fence, the bound counted from when the jobs up to it have their inputs (TegraNVENCInputs.h).
int boundedWait(uint32_t fence)
{
    return gInputs.wait(tegra_host1x_syncpt_read(gSyncpt), fence, TEGRA_HOST1X_ETIMEDOUT, [fence] {
        return tegra_host1x_syncpt_wait(gSyncpt, fence, kWaitBoundUs);
    });
}

bool failedFence(uint32_t fence)
{
    return __atomic_load_n(&gFailed, __ATOMIC_ACQUIRE) && static_cast<int32_t>(fence - gFailedThrough) <= 0;
}

void releaseFailed()
{
    for (uint32_t now = tegra_host1x_syncpt_read(gSyncpt); static_cast<int32_t>(gFailedThrough - now) > 0;
         now = tegra_host1x_syncpt_read(gSyncpt)) {
        const uint32_t step = gFailedThrough - now > 2 ? 2 : gFailedThrough - now;
        if (tegra_host1x_syncpt_release_failed(gSyncpt, now + step, step))
            break;
    }
}

// A fence past its bound: the engine stopped making progress on a job (a lost interrupt panics in TegraHost1x
// instead). As L4T's nvhost does on a channel timeout, the job is cancelled, not the engine: channel 2 is torn down so
// nothing of the stream reaches the engine again, the memory client is flushed, the engine reset and its falcon
// rebooted and proven, the jobs that were in flight fail (their waiters released with -5) and the next job runs on the
// rebooted engine. One line per cancellation. gEngineLock held.
int stalled(uint32_t fence)
{
    if (failedFence(fence))
        return -5;
    char host[200];
    tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_NVENC, host, sizeof(host));
    const uint32_t reached = tegra_host1x_syncpt_read(gSyncpt);
    const char *app = tegra_nvenc_app_dump(gNvenc, gJobCodec[(reached + 1 - gBase) % kSetupSlots]);
    const char *falcon = tegra_nvenc_falcon_dump(gNvenc);
    tegra_host1x_channel_teardown(TEGRA_HOST1X_CHANNEL_NVENC);
    (void)switch_engine_power::flush(kPower);
    switch_car_reset_assert(kClock);
    IODelay(10);
    bool wasOn = false;
    gLine[0] = 0;
    const bool reset = powerUp(&wasOn);
    gFailedThrough = gBase + gIssued;
    __atomic_store_n(&gFailed, true, __ATOMIC_RELEASE);
    releaseFailed();
    tegra_host1x_channel_restart(TEGRA_HOST1X_CHANNEL_NVENC);
    const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
    const char *stage = nullptr;
    const bool back = reset && bootFalcon() && !answer(gSyncpt, from, &stage);
    gBase = tegra_host1x_syncpt_read(gSyncpt);
    __atomic_store_n(&gIssued, 0, __ATOMIC_RELEASE);
    gWedged = !back;
    IOLog("TegraNVENC: job fence %u not reached in %u ms (syncpoint %u at %u, GP timer bound passed); %s; %s; %s; "
          "cancelled through fence %u, engine reset: %s%s\n",
          fence, kWaitBoundUs / 1000, gSyncpt, reached, app, falcon, host, gFailedThrough,
          back ? "running again" : "it did not come back, NVENC refuses work: ",
          back ? ""
               : (gLine[0] ? gLine
                  : stage  ? stage
                           : "falcon"));
    return -5;
}

// Back to reset with the clock off after a failed bring-up: nothing will use the engine this boot.
void park()
{
    switch_car_reset_assert(kClock);
    IODelay(2);
    switch_car_clock_disable(kClock);
}

void bringUp(void *, wait_result_t)
{
    // A failed bring-up leaves nothing behind: the engine back in reset with its clock off (once it was touched), then
    // its address space and kernel buffers returned.
    const auto giveUp = [](bool touched) {
        if (touched)
            park();
        tegra_nvenc_memory_release();
        switch_dma_free(&gSetup);
        switch_dma_free(&gSink);
    };
    if (tegra_host1x_start()) {
        IOLog("TegraNVENC: E0 FAIL host1x is not up (see the TegraHost1x line)\n");
        return;
    }
    gNvenc = switch_mmio_map(kNvencPA, kNvencSize);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gEngineLock = IOLockAlloc();
    if (!gNvenc || !gMc || !gEngineLock || !gInputs.start()) {
        IOLog("TegraNVENC: E1 FAIL no mapping or lock\n");
        return;
    }
    gLine[0] = 0;
    if (!switch_dma_alloc(&gSetup, static_cast<size_t>(kSetupSlots) * kSetupSlotBytes) ||
        !switch_dma_alloc(&gSink, kSinkBytes) || !loadImage()) {
        IOLog("TegraNVENC: %s\n", gLine[0] ? gLine : "E1 FAIL no contiguous setup slots or scratch below 4 GiB");
        giveUp(false);
        return;
    }
    if (tegra_nvenc_memory_init(&gFirmware, &gSetup, &gSink, gLine, sizeof(gLine))) {
        IOLog("TegraNVENC: %s\n", gLine);
        giveUp(false);
        return;
    }
    bool wasOn = false;
    gLine[0] = 0;
    if (!powerUp(&wasOn) || !bootFalcon()) {
        IOLog("TegraNVENC: %s\n", gLine[0] ? gLine : "E1 FAIL");
        giveUp(true);
        return;
    }
    IOLog("TegraNVENC: E1 ok partition %u %s, %u kHz, falcon started (OS code %#x+%#x data %#x+%#x, %u apps)\n",
          kPartition, wasOn ? "already on" : "powered here", gClockKHz, gImage.code, gImage.code_size, gImage.data,
          gImage.data_size, gImage.apps);
    uint64_t us = 0;
    uint32_t id = 0;
    uint32_t from = 0;
    if (!proofOfLife(&us, &id, &from)) {
        IOLog("TegraNVENC: %s\n", gLine);
        giveUp(true);
        return;
    }
    IOLog("TegraNVENC: E2 ok host interface and falcon incremented syncpoint %u %u->%u by interrupt in %llu us; %s\n",
          id, from, tegra_host1x_syncpt_read(id), us, falconState());
    gSyncpt = id;
    gBase = tegra_host1x_syncpt_read(id);
    if (!gGate.start({"TegraNVENC", nullptr, gateOn, gateOff})) {
        IOLog("TegraNVENC: E2 FAIL no idle timer\n");
        giveUp(true);
        return;
    }
    gUp = true;
    tegra_host1x_set_sleep_client(engineSuspend, engineResume);
    if (!tegra_nvenc_engine_start())
        IOLog("TegraNVENC: E2 could not register as a memory engine; native pictures go through VIC\n");
    tegra_nvenc_publish();
}
}  // namespace

extern "C" bool tegra_nvenc_ready(void)
{
    return gUp && !gWedged;
}

namespace {
int check(const TegraNVENCSubmit *s, TegraNVENCResolve resolve, void *context, TegraNVENCBlock *blocks,
          const char **why)
{
    *why = nullptr;
    if (s->entry_count > TEGRA_NVENC_MAX_ENTRIES || s->clean_count > TEGRA_NVENC_MAX_RANGES ||
        s->setup_bytes > kSetupSlotBytes) {
        *why = "counts";
        return -22;
    }
    if (s->codec >= 32 || !(kTegraNVENCCodecs & (1u << s->codec))) {
        *why = "codec";
        return -22;
    }
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVENCEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVENC_ENTRY_RELOC)
            continue;
        if (!resolve(context, e->value, &blocks[i]) || e->offset > blocks[i].size) {
            *why = "relocation outside a buffer";
            return -22;
        }
        avail[i] = blocks[i].size - e->offset;
    }
    *why = tegra_nvenc_check(s, avail);
    return *why ? -22 : 0;
}

int run(const TegraNVENCSubmit *s, TegraNVENCResolve resolve, void *context, const TegraNVENCAfter *after,
        const TegraNVENCOrder *order, uint32_t *fence, const char **why)
{
    *why = nullptr;
    if (!gUp)
        return -19;
    TegraNVENCBlock blocks[TEGRA_NVENC_MAX_ENTRIES];
    if (const int error = check(s, resolve, context, blocks, why))
        return error;
    for (uint32_t i = 0; i < s->clean_count; i++) {
        const TegraNVENCRange *r = &s->clean[i];
        TegraNVENCBlock b;
        if (!resolve(context, r->handle, &b) || static_cast<uint64_t>(r->offset) + r->length > b.size) {
            *why = "clean range";
            return -22;
        }
        switch_dcache_clean(b.va + r->offset, r->length);
    }

    // SETCLASS; the submit's methods, SET_APPLICATION_ID first (the checker requires it: the OS selects the app before
    // anything is stored for it), then just before EXECUTE: 0 in every method of kUnusedMethods, and in the ME
    // predictor pair when the submit has none; in each latched method the submit leaves out, the scratch buffer or 0
    // (tegra_nvenc_latch); the scratch buffer as the RC process state (NVIDIA's driver gives every job one; constant
    // QP leaves it unread); the setup slot (INCR METHOD0 x2: method, value, each); OP_DONE increment. Room in front for
    // the waits: the job before it, the engine that wrote its input, and a native picture's producers (TegraNVENCOrder).
    // The stream is written in host1x's command arena once the job kInFlight back is done, so at most kInFlight - 1
    // jobs hold arena room and begin never sleeps under gEngineLock, which the stall path needs.
    constexpr uint32_t kUnused = sizeof(kUnusedMethods) / sizeof(kUnusedMethods[0]);
    constexpr uint32_t kWaits = (2 + TEGRA_NVENC_ORDER_WAITS) * SWITCH_HOST1X_WAIT_WORDS;
    constexpr uint32_t kWords = kWaits + 1 + 3 * (kUnused + TEGRA_NVENC_LATCHED + 4 + TEGRA_NVENC_MAX_ENTRIES) + 1;
    static_assert((kInFlight + 1) * kWords <= SWITCH_HOST1X_ARENA_WORDS);
    uint32_t *words = nullptr;
    uint32_t n = kWaits;
    bool predictors = false;
    for (uint32_t i = 0; i < s->entry_count; i++)
        predictors |= s->entries[i].method == NVC5B7_SET_IN_MEPRED_DATA;
    const auto method = [&](uint32_t offset, uint32_t value) {
        words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
        words[n++] = offset / 4;
        words[n++] = value;
    };
    if (const int error = gGate.use())
        return error;
    IOLockLock(gEngineLock);
    while (gSuspended)
        IOLockSleep(gEngineLock, &gSuspended, THREAD_UNINT);
    if (gWedged) {
        IOLockUnlock(gEngineLock);
        gGate.done();
        return -19;
    }
    const uint32_t job = gIssued + 1;
    if (job > kInFlight) {
        int ret = boundedWait(gBase + job - kInFlight);
        if (ret == TEGRA_HOST1X_ETIMEDOUT)
            ret = stalled(gBase + job - kInFlight);
        if (ret || gWedged) {
            IOLockUnlock(gEngineLock);
            gGate.done();
            return ret ? ret : -19;
        }
    }
    if (const int error = switch_host1x_job_begin(TEGRA_HOST1X_CHANNEL_NVENC, kWords, &words)) {
        IOLockUnlock(gEngineLock);
        gGate.done();
        return error;
    }
    TegraNVENCAfter ordered[TEGRA_NVENC_ORDER_WAITS];
    uint32_t orderedCount = 0;
    if (order) {
        if (const int error = order->begin(order->context, gBase + job, ordered, &orderedCount)) {
            switch_host1x_job_cancel(TEGRA_HOST1X_CHANNEL_NVENC);
            IOLockUnlock(gEngineLock);
            gGate.done();
            return error;
        }
    }
    words[n++] = tegra_host1x_op_setclass(kClass, 0, 0);
    const uint32_t slot = job % kSetupSlots;
    gJobCodec[slot] = static_cast<uint8_t>(s->codec);
    uint8_t *setup = gSetup.va + static_cast<size_t>(slot) * kSetupSlotBytes;
    memcpy(setup, s->setup, s->setup_bytes);
    switch_dcache_clean(setup, s->setup_bytes);
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVENCEntry *e = &s->entries[i];
        if (i == s->entry_count - 1) {  // EXECUTE, last by the check
            for (const uint16_t unused : kUnusedMethods)
                method(unused, 0);
            if (!predictors) {
                method(NVC5B7_SET_IN_MEPRED_DATA, 0);
                method(NVC5B7_SET_OUT_MEPRED_DATA, 0);
            }
            uint8_t latch[TEGRA_NVENC_LATCHED];
            tegra_nvenc_latch(s, &gLatched, latch);
            for (uint32_t k = 0; k < TEGRA_NVENC_LATCHED; k++) {
                if (latch[k] != TEGRA_NVENC_LATCH_NONE)
                    method(tegra_nvenc_latched_method(k),
                           latch[k] == TEGRA_NVENC_LATCH_SCRATCH ? static_cast<uint32_t>(gSink.pa >> 8) : 0);
            }
            method(NVC5B7_SET_IO_RC_PROCESS, static_cast<uint32_t>(gSink.pa >> 8));
            method(NVC5B7_SET_IN_DRV_PIC_SETUP,
                   static_cast<uint32_t>((gSetup.pa + static_cast<uint64_t>(slot) * kSetupSlotBytes) >> 8));
        }
        method(e->method, e->kind == TEGRA_NVENC_ENTRY_RELOC ? static_cast<uint32_t>((blocks[i].iova + e->offset) >> 8)
                                                             : e->value);
    }
    words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    // L4T's NVENC channel is serialized (t210.c .serialize): a job's methods reach the engine only once the job before
    // it is done, by a host1x wait in the stream, not by the CPU.
    uint32_t first = kWaits;
    if (gIssued) {
        first -= SWITCH_HOST1X_WAIT_WORDS;
        tegra_host1x_op_wait_syncpt(words + first, gSyncpt, gBase + gIssued);
    }
    if (after) {
        first -= SWITCH_HOST1X_WAIT_WORDS;
        tegra_host1x_op_wait_syncpt(words + first, after->syncpt, after->threshold);
    }
    for (uint32_t k = 0; k < orderedCount; k++) {
        first -= SWITCH_HOST1X_WAIT_WORDS;
        tegra_host1x_op_wait_syncpt(words + first, ordered[k].syncpt, ordered[k].threshold);
    }
    const int ret =
        switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_NVENC, words + first, n - first, gSyncpt, gBase + job);
    if (!ret) {
        switch_gpu_backing_token producers[TegraNVENCInputs::kProducers];
        uint32_t count = 0;
        if (after)
            producers[count++] = after->producer;
        for (uint32_t k = 0; k < orderedCount; k++)
            producers[count++] = ordered[k].producer;
        gInputs.record(gBase + job, producers, count);
        __atomic_store_n(&gIssued, job, __ATOMIC_RELEASE);
        *fence = gBase + job;
    }
    if (order)
        order->end(order->context, gBase + job, ret == 0);
    IOLockUnlock(gEngineLock);
    gGate.done();
    return ret;
}
}  // namespace

extern "C" int tegra_nvenc_check_submit(const TegraNVENCSubmit *s, TegraNVENCResolve resolve, void *context,
                                        const char **why)
{
    TegraNVENCBlock blocks[TEGRA_NVENC_MAX_ENTRIES];
    return check(s, resolve, context, blocks, why);
}

extern "C" int tegra_nvenc_run(const TegraNVENCSubmit *s, TegraNVENCResolve resolve, void *context,
                               const TegraNVENCAfter *after, const TegraNVENCOrder *order, uint32_t *fence,
                               const char **why)
{
    return run(s, resolve, context, after, order, fence, why);
}

extern "C" uint32_t tegra_nvenc_syncpt(void)
{
    return gSyncpt;
}

extern "C" int tegra_nvenc_wait(uint32_t fence)
{
    if (!gUp)
        return -19;
    // A fence no job will reach would never be reached: refused, so it cannot pass for an engine fault.
    if ((int32_t)(fence - (gBase + __atomic_load_n(&gIssued, __ATOMIC_ACQUIRE))) > 0)
        return -22;
    int ret = boundedWait(fence);
    if (ret == TEGRA_HOST1X_ETIMEDOUT) {
        IOLockLock(gEngineLock);
        ret = stalled(fence);
        IOLockUnlock(gEngineLock);
    }
    if (drained())
        gGate.idle();
    return !ret && failedFence(fence) ? -5 : ret;
}

extern "C" int tegra_nvenc_input_failure(uint32_t fence)
{
    return gInputs.failure(fence);
}

namespace {
// Idle power gating (SwitchEngineGate.h), as mainline's runtime PM does for VIC and NVDEC: off 500 ms after the last
// job, on at the next. Jobs run asynchronously and NVENC has no retire thread, so off finds work still queued unless
// the syncpoint is at the last job's fence (BUSY), and the wait that sees the last fence reached arms the timer again
// (a client's close waits for its last fence). On is E1 and E2 again on the kept image and address space (an idle
// power-off does not reset the MC), then the jobs are numbered from the syncpoint. A failed power-on is one line and
// NVENC refuses work from then on, as a wedged engine does. The MPE partition's MBIST workaround borrows no other
// engine's clock, so the gate has its own lock.
int gateOn(void *)
{
    IOLockLock(gEngineLock);
    bool wasOn = false;
    bool answered = false;
    gLine[0] = 0;
    if (!gWedged && powerUp(&wasOn) && bootFalcon()) {
        const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
        const char *stage = nullptr;
        const int ret = answer(gSyncpt, from, &stage);
        answered = !ret;
        if (ret)
            snprintf(gLine, sizeof(gLine), "%s did not answer (%d, syncpoint %u %u->%u); %s", stage, ret, gSyncpt, from,
                     tegra_host1x_syncpt_read(gSyncpt), falconState());
    }
    if (!answered && !gWedged) {
        gWedged = true;
        park();
        IOLog("TegraNVENC: power-on FAIL %s; NVENC refuses work\n", gLine[0] ? gLine : "E1");
    }
    gBase = tegra_host1x_syncpt_read(gSyncpt);
    __atomic_store_n(&gIssued, 0, __ATOMIC_RELEASE);
    gSuspended = false;
    IOLockWakeup(gEngineLock, &gSuspended, false);
    IOLockUnlock(gEngineLock);
    return answered ? 0 : -19;
}

int gateOff(void *, bool drain)
{
    IOLockLock(gEngineLock);
    if (!drain && !drained()) {
        IOLockUnlock(gEngineLock);
        return SWITCH_ENGINE_GATE_BUSY;
    }
    if (drain && !gWedged && boundedWait(gBase + gIssued) == TEGRA_HOST1X_ETIMEDOUT)
        stalled(gBase + gIssued);
    gSuspended = true;
    const bool off = switch_engine_power_down(kPower);
    IOLockUnlock(gEngineLock);
    return off ? SWITCH_ENGINE_GATE_OFF : -5;
}

// Sleep (SC7), through TegraHost1x's sleep clients. Suspend powers the engine off once its queued jobs are done (a
// stall is cancelled as at any wait), before host1x saves its syncpoints, and detaches its address space; new jobs wait
// for resume. Resume runs after host1x is back and attaches the address space again (SC7 resets the MC); the next job
// powers NVENC on.
void engineSuspend()
{
    gGate.suspend();
    IOLockLock(gEngineLock);
    tegra_nvenc_memory_detach();
    IOLockUnlock(gEngineLock);
}

void engineResume()
{
    gLine[0] = 0;
    const bool attached = !tegra_nvenc_memory_attach(gLine, sizeof(gLine));
    if (!attached) {
        IOLockLock(gEngineLock);
        gWedged = true;
        gSuspended = false;
        IOLockWakeup(gEngineLock, &gSuspended, false);
        IOLockUnlock(gEngineLock);
        IOLog("TegraNVENC: resume FAIL %s; NVENC refuses work\n", gLine);
    }
    gGate.resume(attached);
}
}  // namespace

// switch-no-nvdec turns hardware video encoding off as well as decoding.
extern "C" int tegra_nvenc_probe(void)
{
    static volatile int started;
    if (switch_boot_flag("switch-no-nvdec"))
        return 0;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST))
        return 0;
    thread_t thread = nullptr;
    if (kernel_thread_start(bringUp, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraNVENC: could not start the bring-up thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
