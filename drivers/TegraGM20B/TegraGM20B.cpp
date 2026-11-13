// SPDX-License-Identifier: MIT
// TegraGM20B entry point. tegra_gm20b_probe() brings the GPU up through every stage (kStage), unless boot-arg
// switch-no-gpu is set (then SwitchAccelStub stands in, and Core Animation renders on the CPU):
//   1  M1: VDD_GPU on, clocks/reset/clamp/MC flush (nvgpu gm20b_tegra_unrailgate order), BOOT_0, PRIV ring, GPCPLL lock
//   2  M2: + nvkm device init (ACR HS boot, LS PMU), GR (FECS/GPCCS through ACR, golden context)
//   3  M3: + GPU self-test (VM, GPFIFO channel, copy-engine fill, semaphore fence)
//   4  M4: + publish AGXAccelerator : IOAcceleratorES (MetalPluginName TegraGM20BMetal) for the NVK userland
// The work runs on its own kernel thread, so the probe returns at once; switch_root_boot_prepare() calls it once the
// SD disk is published. From the end of stage 1 the GPU interrupts (SPI 157/158) drive nvkm; there is no polled mode.
// Logs one line per stage reached ("tegra-gm20b: M1 ok ..."), or the line of the stage that failed.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSKextLib.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "../SwitchSuspend/SwitchWake.h"
#include "TegraGM20BGpu.h"
#include "SwitchGpuPathStats.h"
#include "SwitchGpuShaderCompiler.h"
#include "TegraGM20BRail.h"
#include "TegraGM20BSleep.h"
#include "shim/include/switch_tiled_cache.h"

extern "C" {
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
kern_return_t OSKextLoadKextWithIdentifier(const char *kextIdentifier);
int nvxnu_trace_paced(void);  // shim/xnu_glue.cpp
// The staged accelerator class (TegraGM20BAccelerator.cpp, a late object like SwitchAccelStub.cpp): weak, so this file links
// and stages 1..3 work without it.
void tegra_gm20b_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_TegraGM20BAccelerator.cpp")
	__attribute__((weak));
IOService *tegra_gm20b_accelerator_allocate(void) __attribute__((weak));
unsigned tegra_gm20b_accelerator_instance_size(void) __attribute__((weak));
}
extern "C" OSReturn switch_kernel_classes(void (*initialize)());

// drivers/SwitchPower/SwitchThermal.cpp: a CPU raise held back for the bring-up goes ahead (a no-op before that service
// runs).
extern "C" void switch_thermal_gpu_ready(void);

// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" int vsnprintf(char *, size_t, const char *, va_list);

namespace
{
char gLine[256];
volatile int gStarted, gBringupDone;
// From tegra_gm20b_suspend() to the end of tegra_gm20b_resume(): the wake's power-up, GPCPLL raise and GR init are the
// bring-up's again, so SwitchThermal holds its CPU raise for them as it does at boot (tegra_gm20b_bringup_busy()).
volatile int gSleeping;

// Rail-gating with the display off (TegraGM20BRail.h). gRailLock orders the worker's moves against system sleep, which
// holds tgm_gpu.lock across SC7 with the GPU already down (gRailSC7Held). gRailMoving counts the moves under way (the
// worker's rail-down and a caller's rail-up can overlap) and holds SwitchThermal's CPU raise as gSleeping does across
// SC7; a rail-up made under some caller's tgm_gpu.lock leaves gRailMoved for the worker, which lets the raise go.
// gRailExpected: a wake source fired with the panel off, so the worker brings the GPU up once ahead of the panel
// (consumed by that move; the display stays off as far as the rail knows). gRailDemands counts the wakes by work since
// the display went off, the last at gRailDemandAt (mach time), for the hold; gRailHoldCall looks again when it ends
// (its own call: entering gRailCall delayed could turn an immediate entry already pending into a late one).
IOLock *gRailLock;
thread_call_t gRailCall, gRailHoldCall;
volatile int gRailDisplayOn = 1, gRailExpected, gRailMoving, gRailMoved, gRailSC7Held;
volatile uint32_t gRailDemands;
volatile uint64_t gRailDemandAt;

// The bring-up has ended (done or failed): SwitchThermal's CPU raise, which waits for it on the shared MAX77812, goes
// ahead at once, and a system sleep waiting for it goes on.
void bringupDone()
{
	__atomic_store_n(&gBringupDone, 1, __ATOMIC_RELEASE);
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the wait event is the word's address
	thread_wakeup(const_cast<int *>(&gBringupDone));
	switch_thermal_gpu_ready();
}

// System sleep may not power the GPU down under its bring-up thread. No bring-up can start later: tegra_gm20b_probe()
// runs when the SD disk is published, before the userland that asks for sleep exists.
void waitBringup()
{
	constexpr uint32_t kBringupWaitMs = 30000;
	uint64_t deadline = 0;
	clock_interval_to_deadline(kBringupWaitMs, kMillisecondScale, &deadline);
	while (!__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE)) {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the wait event is the word's address
		assert_wait_deadline(const_cast<int *>(&gBringupDone), THREAD_UNINT, deadline);
		if (__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE)) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT &&
		    !__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE)) {
			panic("tegra-gm20b: system sleep: the GPU bring-up still runs %u ms into "
			      "it",
			      kBringupWaitMs);
		}
	}
}

// Staged as UniXNU-sheng's publishMetal() (package/GpuPowerProbe.cpp:1217-1251) and SwitchAccelStubPublish.cpp: load
// IOGPUFamily (already loaded by the stub; idempotent) so the IOAcceleratorES metaclass exists, run our class's static
// initializer through switch_kernel_classes, then publish with the keys the original discovery reads.
// NativeSandboxGrantProcess: the comma-separated process names whose open our XNU grants in place of the sandbox
// verdict (IOUserClient.cpp native_sandbox_grant(), as in sheng's zzw-native-adreno-sandbox-grant.patch), each matched
// on at most its first 16 characters.
IOReturn publish()
{
	if (!tegra_gm20b_accelerator_static_init || !tegra_gm20b_accelerator_allocate ||
	    !tegra_gm20b_accelerator_instance_size)
		return kIOReturnUnsupported;
	const kern_return_t kr = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
	if (kr)
		return kr;
	const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
	const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
	OSSafeReleaseNULL(name);
	if (!parent || parent->getClassSize() != 136 ||
	    tegra_gm20b_accelerator_instance_size() != 136)
		return kIOReturnNotFound;
	const IOReturn ret = switch_kernel_classes(tegra_gm20b_accelerator_static_init);
	if (ret)
		return ret;
	IOService *accelerator = tegra_gm20b_accelerator_allocate();
	if (!accelerator)
		return kIOReturnNoMemory;
	if (!accelerator->init()) {
		accelerator->release();
		return kIOReturnError;
	}
	accelerator->setProperty("MetalPluginName", "TegraGM20BMetal");
	accelerator->setProperty("MetalPluginClassName", "TegraGM20BMetalDevice");
	// OpenGL ES (libGFXShared's _gfxInitializeLibrary) loads the renderer this names, as on J120's AGXG9G: Apple's
	// GLES over Metal, which runs on the Metal device above. Without it every EAGLContext is nil.
	accelerator->setProperty("IOGLESBundleName", "AppleMetalGLRenderer");
	// backboardd is the render server. iconservicesagent: with no Metal device Core Image 21H461 falls back to OpenGL ES,
	// which has no driver here, and the agent crash-loops on every SF Symbol (Settings then waits on it forever).
	// ScreenshotServicesService (the screenshot thumbnail and markup): PencilKit's -[PKMetalResourceHandler
	// newPaintShaderKernelPipelineState] takes a nil device's nil paint_kernel and Metal aborts the process
	// (setComputeFunction: nil), so every screenshot's thumbnail dies 1-2 s after the flash. Names match on their first 16
	// characters, so this also admits ClassroomKit's ScreenshotService; the plugin compares the whole name and gives that
	// one no device. Every other process too: "*" is any name for native_sandbox_grant()
	// (IOUserClient.cpp); a kernel without that change matches "*" to no process and still grants the named ones.
	accelerator->setProperty(
		"NativeSandboxGrantProcess",
		tgm_all_clients() ? "*,backboardd,iconservicesagent,ScreenshotServicesService"
				  : "backboardd,iconservicesagent,ScreenshotServicesService");
	if (!accelerator->attach(IOService::getPlatform())) {
		accelerator->release();
		return kIOReturnNotAttached;
	}
	accelerator->registerService();
	accelerator->release();
	return kIOReturnSuccess;
}

#if TGM_STATS
// debug.switch_gpu_clkgate (debug kernels): 1 clock gating on, 0 off, for a frame-time A/B within one boot; BlueTool
// writes it when the Mac posts com.brunostjohn.switch.debug.gpu-clkgate-on / -off.
int gClkgateOn = 1;

int clkgateSysctl SYSCTL_HANDLER_ARGS
{
	int on = gClkgateOn;
	int error = SYSCTL_OUT(req, &on, sizeof(on));
	if (error || !req->newptr)
		return error;
	error = SYSCTL_IN(req, &on, sizeof(on));
	if (error)
		return error;
	if (tgm_clkgate_switch(static_cast<int>(on != 0)))
		return EBUSY;
	gClkgateOn = static_cast<int>(on != 0);
	return 0;
}

struct sysctl_oid gClkgateOid;

// debug.switch_gpu_regdump (debug kernels, read-only): the registers L4T's capture.sh reads, as text for diff_regs.py
// (TegraGM20BRegDump.h). Reading it wakes the GPU if it is rail-gated.
int regdumpSysctl SYSCTL_HANDLER_ARGS
{
	if (req->newptr)
		return EPERM;
	const size_t cap = tgm_regdump_bytes();
	if (!req->oldptr)
		return SYSCTL_OUT(req, nullptr, cap);
	auto *buf = static_cast<char *>(IOMallocData(cap));
	if (!buf)
		return ENOMEM;
	size_t len = 0;
	const int ret = tgm_regdump(buf, cap, &len);
	const int error = ret ? -ret : SYSCTL_OUT(req, buf, len);
	IOFreeData(buf, cap);
	return error;
}

struct sysctl_oid gRegdumpOid;

void registerClkgateSysctl()
{
	switch_sysctl_oid(
		&gClkgateOid, &sysctl__debug_children, "switch_gpu_clkgate",
		static_cast<int>(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED),
		clkgateSysctl, "I", "GPU clock gating on (1) or off (0), debug A/B");
	sysctl_register_oid(&gClkgateOid);
	switch_sysctl_oid(
		&gRegdumpOid, &sysctl__debug_children, "switch_gpu_regdump",
		static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED),
		regdumpSysctl, "A", "GM20B registers for the L4T diff");
	sysctl_register_oid(&gRegdumpOid);
}
#endif

// NOLINTNEXTLINE(misc-const-correctness): kernel_thread_start's thread_continue_t signature
void bringup(void *arg, wait_result_t)
{
	const int stage = static_cast<int>(reinterpret_cast<intptr_t>(arg));
	for (int s = 1; s <= (stage < 3 ? stage : 3); s++) {
		const int ret = tgm_bringup(s, gLine, sizeof(gLine));
		IOLog("tegra-gm20b: %s\n", gLine[0] ? gLine : "no result");
		if (ret) {
			bringupDone();
			return;
		}
		// Bring-up: the text console lags the log, and logd flushes every few seconds; let both catch up so a stage
		// that hangs the machine still leaves the previous stage's result on screen and on Data.
		if (TGM_STATS && nvxnu_trace_paced())
			IOSleep(3000);
	}
	if (stage >= 4) {
		const IOReturn ret = publish();
		if (ret) {
			IOLog("tegra-gm20b: M4 FAIL publish AGXAccelerator 0x%x\n", ret);
		} else {
			IOLog("tegra-gm20b: M4 ok published AGXAccelerator (MetalPluginName "
			      "TegraGM20BMetal)\n");
		}
	}
#if TGM_STATS
	registerClkgateSysctl();
	switch_gpu_path_register();
#endif
#if SWITCH_GPU_COMPILER_LINE
	switch_gpu_compiler_register_sysctl();
#endif
	bringupDone();
}

bool gpuDisabled()
{
	unsigned off = 0;
	return (PE_parse_boot_argn("switch-no-gpu", &off, sizeof(off)) != 0) && (off != 0u);
}
}  // namespace

// 1 from boot until the bring-up (VDD_GPU on, GPCPLL raise, ACR/PMU/GR, self-test) has finished or failed: for a driver
// whose own rail or clock changes must not overlap it (SwitchThermal's CPU raise on the same MAX77812, which the board
// thread starts before tegra_gm20b_probe()). 0 with switch-no-gpu.
extern "C" int tegra_gm20b_bringup_busy(void)
{
	static int wanted = -1;
	if (wanted < 0)
		wanted = static_cast<int>(!gpuDisabled());
	return static_cast<int>((wanted != 0) &&
				((__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE) == 0) ||
				 (__atomic_load_n(&gSleeping, __ATOMIC_ACQUIRE) != 0) ||
				 __atomic_load_n(&gRailMoving, __ATOMIC_ACQUIRE) > 0));
}

extern "C" int switch_tiled_cache_on(void)
{
	unsigned off = 0;
	return static_cast<int>(
		(PE_parse_boot_argn("switch-no-tiled-cache", &off, sizeof(off)) == 0) ||
		(off == 0u));
}

extern "C" int switch_gpu_compression_on(void)
{
	static bool logged = false;
	unsigned off = 0;
	const bool on = (PE_parse_boot_argn("switch-no-gpu-compression", &off, sizeof(off)) == 0) ||
			(off == 0u);
	if (!on && !__atomic_exchange_n(&logged, true, __ATOMIC_RELAXED))
		IOLog("tegra-gm20b: compression off (switch-no-gpu-compression)\n");
	return static_cast<int>(on);
}

#if DEVELOPMENT || DEBUG
extern "C" void tgm_journal_impl(const char *fmt, ...)
{
	char line[256];
	va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	switch_power_journal("%s", line);
}

#endif

extern "C" void tegra_gm20b_suspend(void)
{
	if (!__atomic_load_n(&gStarted, __ATOMIC_ACQUIRE))
		return;
	waitBringup();
	IOLockLock(gRailLock);
	__atomic_store_n(&gSleeping, 1, __ATOMIC_RELEASE);
	gRailSC7Held = tgm_rail_sc7_hold();
	if (!gRailSC7Held)
		tgm_sleep();
	IOLockUnlock(gRailLock);
}

extern "C" void tegra_gm20b_resume(void)
{
	if (!__atomic_load_n(&gSleeping, __ATOMIC_ACQUIRE))
		return;
	IOLockLock(gRailLock);
	if (gRailSC7Held) {
		tgm_rail_sc7_release();
	} else {
		tgm_wake();
	}
	gRailSC7Held = 0;
	__atomic_store_n(&gSleeping, 0, __ATOMIC_RELEASE);
	IOLockUnlock(gRailLock);
	switch_thermal_gpu_ready();
	thread_call_enter(gRailCall);
}

namespace
{
// The rail's thread call: at most one move per call, under gRailLock; anything that changes meanwhile enters the call
// again, and it decides afresh. A GPU that is idle and held up only by a demand's hold looks again when the hold ends.
void railMove(thread_call_param_t, thread_call_param_t)
{
	IOLockLock(gRailLock);
	const bool up = tgm_railed() == 0;
	const bool expected = __atomic_exchange_n(&gRailExpected, 0, __ATOMIC_ACQ_REL) != 0;
	const bool displayOn = __atomic_load_n(&gRailDisplayOn, __ATOMIC_ACQUIRE) != 0;
	const bool idle = tgm_idle_now() != 0;
	uint64_t heldUntil = 0;
	const uint64_t hold = tgm_rail_hold_ns(__atomic_load_n(&gRailDemands, __ATOMIC_ACQUIRE));
	if (hold) {
		nanoseconds_to_absolutetime(hold, &heldUntil);
		heldUntil += __atomic_load_n(&gRailDemandAt, __ATOMIC_ACQUIRE);
	}
	const bool held = heldUntil > mach_absolute_time();
	const enum tgm_rail_move move = tgm_rail_next(
		static_cast<int>(up), static_cast<int>(displayOn || expected),
		static_cast<int>(idle), static_cast<int>(held),
		static_cast<int>((__atomic_load_n(&gSleeping, __ATOMIC_ACQUIRE) != 0) ||
				 (__atomic_load_n(&gBringupDone, __ATOMIC_ACQUIRE)) == 0));
	if (move == TGM_RAIL_DOWN) {
		__atomic_add_fetch(&gRailMoving, 1, __ATOMIC_ACQ_REL);
		tgm_rail_down();
		__atomic_sub_fetch(&gRailMoving, 1, __ATOMIC_ACQ_REL);
		__atomic_store_n(&gRailMoved, 1, __ATOMIC_RELEASE);
	} else if (move == TGM_RAIL_UP) {
		tgm_rail_up();
	}
	IOLockUnlock(gRailLock);
	if (__atomic_exchange_n(&gRailMoved, 0, __ATOMIC_ACQ_REL))
		switch_thermal_gpu_ready();
	if (move == TGM_RAIL_STAY && up && !displayOn && idle && held)
		thread_call_enter_delayed(gRailHoldCall, heldUntil);
}
}  // namespace

extern "C" void tegra_gm20b_display_power(bool on)
{
	if (!on)
		__atomic_store_n(&gRailDemands, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&gRailDisplayOn, on ? 1 : 0, __ATOMIC_RELEASE);
	if (gRailCall)
		thread_call_enter(gRailCall);
}

// SwitchWake: a wake source fired with the panel off (power key, Joy-Con Home, ...). The panel's half-second power-up
// follows only if iOS decides to wake; a GPU brought up for nothing goes back down once idle.
extern "C" void tegra_gm20b_display_expected(void)
{
	if (!gRailCall || __atomic_load_n(&gRailDisplayOn, __ATOMIC_ACQUIRE))
		return;
	__atomic_store_n(&gRailExpected, 1, __ATOMIC_RELEASE);
	thread_call_enter(gRailCall);
}

extern "C" void tgm_rail_moving(void)
{
	__atomic_add_fetch(&gRailMoving, 1, __ATOMIC_ACQ_REL);
}

extern "C" void tgm_rail_moved(int demand)
{
	if (demand) {
		__atomic_store_n(&gRailDemandAt, mach_absolute_time(), __ATOMIC_RELEASE);
		__atomic_add_fetch(&gRailDemands, 1, __ATOMIC_ACQ_REL);
	}
	__atomic_sub_fetch(&gRailMoving, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&gRailMoved, 1, __ATOMIC_RELEASE);
	if (gRailCall)
		thread_call_enter(gRailCall);
}

extern "C" void tgm_rail_idle(void)
{
	if (gRailCall && !__atomic_load_n(&gRailDisplayOn, __ATOMIC_ACQUIRE))
		thread_call_enter(gRailCall);
}

extern "C" int tegra_gm20b_probe(void)
{
	constexpr int kStage = 4;
	if (gpuDisabled())
		return 0;
	if (__atomic_exchange_n(&gStarted, 1, __ATOMIC_SEQ_CST))
		return 0;
	gRailLock = IOLockAlloc();
	gRailCall = thread_call_allocate_with_options(
		railMove, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	gRailHoldCall = thread_call_allocate_with_options(
		railMove, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (!gRailLock || !gRailCall || !gRailHoldCall)
		panic("tegra-gm20b: no memory for the rail-gate's lock and thread calls at boot");
	thread_t thread = nullptr;
	if (kernel_thread_start(bringup, reinterpret_cast<void *>(static_cast<intptr_t>(kStage)),
				&thread) != KERN_SUCCESS) {
		IOLog("tegra-gm20b: could not start the bring-up thread\n");
		bringupDone();
		return -1;
	}
	thread_deallocate(thread);
	return 0;
}
