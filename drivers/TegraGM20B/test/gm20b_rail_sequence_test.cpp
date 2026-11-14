// platform.c's GPU rail-gating and ungating against the Tegra X1 TRM's procedure for the GPU power domain (12.2.7.17),
// step for step: "asynchronous resets arriving in GM20b are made synchronous to the osc_div_clk domain but are then
// consumed in multiple other clock domains", so the order of clamp, reset and clock gates is the hardware's contract.
// The real platform.c and TegraSMMU.cpp (the MC hot-reset owner) run on a register file that turns every CAR, PMC, MC
// and MAX77812 write into the TRM step it is.
#include "drivers/TegraGM20B/shim/include/nvxnu.h"
#include "drivers/TegraI2C/TegraI2C.h"
#include "gm20b_rail_sequence_stubs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace
{
constexpr uintptr_t kCar = 0x60006000, kPmc = 0x7000e400, kMc = 0x70019000;
constexpr uint32_t kClkOutEnbX = 0x280, kClkEnbXSet = 0x284, kClkEnbXClr = 0x288;
constexpr uint32_t kRstDevicesX = 0x28c, kRstDevXSet = 0x290, kRstDevXClr = 0x294;
constexpr uint32_t kGpuRgCntrl = 0x2d4, kHotresetCtrl1 = 0x970, kHotresetStatus1 = 0x974;
constexpr uint32_t kXGpu = 1u << 24, kXPllgRef = 1u << 29, kMcGpu = 1u << 2;
constexpr uint8_t kEnCtrl = 0x06, kM1Vout = 0x23, kEnM1 = 0x01, kEnM4 = 0x40;

struct Step {
	std::string what;
	bool inReset;
};

std::map<uintptr_t, uint32_t> gRegisters;
std::vector<Step> gSteps;
uint8_t gEnCtrl, gVout;
bool gPmicFails;
unsigned gPmicTransfers;

uint32_t &reg(uintptr_t base, uint32_t offset)
{
	return gRegisters[base + offset];
}

void step(const std::string &what)
{
	gSteps.push_back({what, (reg(kCar, kRstDevicesX) & kXGpu) != 0});
}

void clockStep(uint32_t bits, int on)
{
	const char *value = on ? "1" : "0";
	if (bits & kXGpu)
		step(std::string("CLK_ENB_GPU=") + value);
	if (bits & kXPllgRef)
		step(std::string("CLK_ENB_PLLG_REF=") + value);
}

// TRM reset values: CLK_OUT_ENB_X 0x23000780 (5.2.87: GPU and PLLG_REF clocks on), RST_DEVICES_X 0x01e42049 (5.2.90:
// SWR_GPU_RST set), GPU_RG_CNTRL 1 (12.6.178: clamped), MC CLIENT_HOTRESET_CTRL_1 0 (18.11.1.114); VDD_GPU off.
void coldBoot()
{
	gRegisters.clear();
	gSteps.clear();
	reg(kCar, kClkOutEnbX) = 0x23000780;
	reg(kCar, kRstDevicesX) = 0x01e42049;
	reg(kPmc, kGpuRgCntrl) = 1;
	gEnCtrl = kEnM4;
	gPmicFails = false;
	ASSERT_EQ(tegra_gm20b_platform_map(), 0);
	gSteps.clear();
}

void railedDown()
{
	int vdd = -1;
	coldBoot();
	ASSERT_EQ(tegra_gm20b_rail_ungate(), 0);
	ASSERT_EQ(tegra_gm20b_rail_gate(&vdd), 0);
	ASSERT_EQ(vdd, 0);
	gSteps.clear();
}

// The steps the TRM names, in order (delays, PLLP_OUT5 and the PMIC's voltage left out).
std::vector<std::string> trmSteps()
{
	std::vector<std::string> steps;
	for (const Step &s : gSteps) {
		if (s.what != "delay" && s.what.rfind("PLLP_OUT5", 0) != 0 && s.what != "M1_VOUT")
			steps.push_back(s.what);
	}
	return steps;
}

const std::vector<std::string> kGating = {
	"GPU_FLUSH_ENABLE=1", "GPU_HOTRESET_STATUS=FLUSH_DONE",
	"RAIL_CLAMP=1",       "SWR_GPU_RST=1",
	"CLK_ENB_GPU=0",      "CLK_ENB_PLLG_REF=0",
	"VDD_GPU=off",
};

const std::vector<std::string> kUngating = {
	"VDD_GPU=on",    "CLK_ENB_GPU=1",      "CLK_ENB_PLLG_REF=1", "RAIL_CLAMP=1",
	"RAIL_CLAMP=0",  "SWR_GPU_RST=1",      "CLK_ENB_GPU=0",      "SWR_GPU_RST=0",
	"CLK_ENB_GPU=1", "GPU_FLUSH_ENABLE=0",
};

size_t indexOf(const std::string &what, size_t from = 0)
{
	for (size_t i = from; i < gSteps.size(); ++i) {
		if (gSteps[i].what == what)
			return i;
	}
	return gSteps.size();
}

void expectUngated()
{
	EXPECT_EQ(reg(kCar, kClkOutEnbX) & (kXGpu | kXPllgRef), kXGpu | kXPllgRef);
	EXPECT_EQ(reg(kCar, kRstDevicesX) & kXGpu, 0u);
	EXPECT_EQ(reg(kPmc, kGpuRgCntrl), 0u);
	EXPECT_EQ(reg(kMc, kHotresetCtrl1) & kMcGpu, 0u);
	EXPECT_EQ(gEnCtrl, kEnM4 | kEnM1);
}

void expectUngatingSteps()
{
	EXPECT_EQ(trmSteps(), kUngating);
	const size_t reset = indexOf("SWR_GPU_RST=1");
	const size_t clockOff = indexOf("CLK_ENB_GPU=0", reset);
	ASSERT_LT(clockOff, gSteps.size());
	EXPECT_NE(indexOf("delay", reset), gSteps.size())
		<< "TRM ungating step 5: a delay after SWR_GPU_RST=1";
	EXPECT_LT(indexOf("delay", reset), clockOff);
	const size_t unclamp = indexOf("RAIL_CLAMP=0");
	ASSERT_LT(unclamp, gSteps.size());
	EXPECT_TRUE(gSteps[unclamp].inReset)
		<< "the clamp comes off a GPU that is not held in reset";
}

TEST(Gm20bRailSequence, TheGateIsTheTrmsRailGatingStepForStep)
{
	coldBoot();
	ASSERT_EQ(tegra_gm20b_rail_ungate(), 0);
	gSteps.clear();
	int vdd = -1;
	EXPECT_EQ(tegra_gm20b_rail_gate(&vdd), 0);
	EXPECT_EQ(vdd, 0);
	EXPECT_EQ(trmSteps(), kGating);
	EXPECT_EQ(reg(kCar, kClkOutEnbX) & (kXGpu | kXPllgRef), 0u);
	EXPECT_EQ(reg(kCar, kRstDevicesX) & kXGpu, kXGpu);
	EXPECT_EQ(reg(kPmc, kGpuRgCntrl), 1u);
	EXPECT_EQ(reg(kMc, kHotresetCtrl1) & kMcGpu, kMcGpu)
		<< "the client stays blocked while the rail is off";
}

TEST(Gm20bRailSequence, TheUngateIsTheTrmsRailUngatingStepForStepFromAColdBoot)
{
	coldBoot();
	EXPECT_EQ(tegra_gm20b_rail_ungate(), 0);
	expectUngatingSteps();
	expectUngated();
}

TEST(Gm20bRailSequence, TheUngateIsTheTrmsRailUngatingStepForStepAfterTheGate)
{
	railedDown();
	EXPECT_EQ(tegra_gm20b_rail_ungate(), 0);
	expectUngatingSteps();
	expectUngated();
}

TEST(Gm20bRailSequence, RailCyclesEndWhereTheyStarted)
{
	railedDown();
	for (int cycle = 0; cycle < 3; ++cycle) {
		int vdd = -1;
		ASSERT_EQ(tegra_gm20b_rail_ungate(), 0);
		expectUngated();
		ASSERT_EQ(tegra_gm20b_rail_gate(&vdd), 0);
		ASSERT_EQ(vdd, 0);
	}
}

// A transfer TegraI2C fails (it reports lost arbitration once, with its own line) fails that PMIC step: no second try
TEST(Gm20bRailSequence, AFailedPmicTransferIsNotRepeated)
{
	coldBoot();
	ASSERT_EQ(tegra_gm20b_rail_ungate(), 0);
	gPmicFails = true;
	gPmicTransfers = 0;
	int vdd = 0;
	EXPECT_EQ(tegra_gm20b_rail_gate(&vdd), 0);
	EXPECT_EQ(gPmicTransfers, 1u);
}

TEST(Gm20bRailSequence, TheGateClampsAndResetsTheGpuEvenWhenThePmicCannotBeRead)
{
	coldBoot();
	ASSERT_EQ(tegra_gm20b_rail_ungate(), 0);
	gSteps.clear();
	gPmicFails = true;
	int vdd = 0;
	EXPECT_EQ(tegra_gm20b_rail_gate(&vdd), 0);
	EXPECT_NE(vdd, 0);
	const std::vector<std::string> soc(kGating.begin(), kGating.end() - 1);
	EXPECT_EQ(trmSteps(), soc);
}
}  // namespace

IOSimpleLock *IOSimpleLockAlloc()
{
	return new IOSimpleLock{};
}

void IOSimpleLockFree(IOSimpleLock *lock)
{
	delete lock;
}

void IOSimpleLockLock(IOSimpleLock *lock)
{
	lock->held = true;
}

void IOSimpleLockUnlock(IOSimpleLock *lock)
{
	lock->held = false;
}

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
	if (address == kMc + kHotresetStatus1) {
		const uint32_t status = reg(kMc, kHotresetCtrl1);
		if ((status & kMcGpu) &&
		    (gSteps.empty() || gSteps.back().what != "GPU_HOTRESET_STATUS=FLUSH_DONE"))
			step("GPU_HOTRESET_STATUS=FLUSH_DONE");
		return status;
	}
	return gRegisters[address];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
	switch (address) {
	case kCar + kClkEnbXSet:
		reg(kCar, kClkOutEnbX) |= value;
		clockStep(value, 1);
		return;
	case kCar + kClkEnbXClr:
		reg(kCar, kClkOutEnbX) &= ~value;
		clockStep(value, 0);
		return;
	case kCar + kRstDevXSet:
		reg(kCar, kRstDevicesX) |= value;
		if (value & kXGpu)
			step("SWR_GPU_RST=1");
		return;
	case kCar + kRstDevXClr:
		reg(kCar, kRstDevicesX) &= ~value;
		if (value & kXGpu)
			step("SWR_GPU_RST=0");
		return;
	case kCar + kClkOutEnbX:
	case kCar + kRstDevicesX:
		step("a read-modify-write of a CAR word that has SET/CLR aliases");
		break;
	case kCar + 0x67c:
		if ((value ^ gRegisters[address]) & (1u << 17))
			step(std::string("PLLP_OUT5 CLKEN=") + ((value >> 17) & 1 ? "1" : "0"));
		break;
	case kPmc + kGpuRgCntrl:
		step(std::string("RAIL_CLAMP=") + (value & 1 ? "1" : "0"));
		break;
	case kMc + kHotresetCtrl1:
		step(std::string("GPU_FLUSH_ENABLE=") + (value & kMcGpu ? "1" : "0"));
		break;
	default:
		break;
	}
	gRegisters[address] = value;
}

uint16_t switch_mmio_model_read16(uintptr_t address)
{
	return static_cast<uint16_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write16(uintptr_t address, uint16_t value)
{
	switch_mmio_model_write32(address, value);
}

uint8_t switch_mmio_model_read8(uintptr_t address)
{
	return static_cast<uint8_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write8(uintptr_t address, uint8_t value)
{
	switch_mmio_model_write32(address, value);
}

void IOLog(const char *, ...)
{
}

uint64_t mach_absolute_time(void)
{
	return 0;
}

void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *result)
{
	*result = 0;
}

uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
	return phys_addr;
}

volatile void *switch_linux_ioremap(uint64_t pa, uint64_t, int)
{
	return reinterpret_cast<volatile void *>(static_cast<uintptr_t>(pa));
}

void switch_linux_udelay(uint64_t)
{
	if (gSteps.empty() || gSteps.back().what != "delay")
		step("delay");
}

void switch_linux_usleep_range(uint64_t, uint64_t)
{
	switch_linux_udelay(0);
}

void nvxnu_log(const char *, ...)
{
}

void switch_car_fuse_readable(void)
{
}

void switch_mc_errors_resume(void)
{
}

uint32_t tegra_mc_error_latest(uint32_t *intstatus, uint32_t *status, uint32_t *address)
{
	*intstatus = *status = *address = 0;
	return 0;
}

void nvxnu_step_impl(const char *, ...)
{
}

int tegra_i2c_init(unsigned)
{
	return 0;
}

int tegra_i2c_read(unsigned, uint8_t, uint8_t reg, void *buf, size_t len)
{
	++gPmicTransfers;
	if (gPmicFails || len != 1)
		return TEGRA_I2C_EIO;
	if (reg != kEnCtrl && reg != kM1Vout)
		return TEGRA_I2C_EIO;
	*static_cast<uint8_t *>(buf) = reg == kEnCtrl ? gEnCtrl : gVout;
	return 0;
}

int tegra_i2c_write(unsigned, uint8_t, uint8_t reg, const void *buf, size_t len)
{
	++gPmicTransfers;
	if (gPmicFails || len != 1)
		return TEGRA_I2C_EIO;
	const uint8_t value = *static_cast<const uint8_t *>(buf);
	if (reg == kM1Vout) {
		gVout = value;
		step("M1_VOUT");
		return 0;
	}
	if (reg != kEnCtrl)
		return TEGRA_I2C_EIO;
	if ((value ^ gEnCtrl) & kEnM1)
		step(value & kEnM1 ? "VDD_GPU=on" : "VDD_GPU=off");
	gEnCtrl = value;
	return 0;
}
}
