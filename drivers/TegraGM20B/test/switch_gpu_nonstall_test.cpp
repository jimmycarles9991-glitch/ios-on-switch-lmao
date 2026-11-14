// SPDX-License-Identifier: MIT
// The job interrupts' tree (SwitchGpuNonstall.h) on a model of GM20B's two PMC trees, as nvgpu programs them: PMC_INTR_0's
// PFIFO bit is PFIFO_INTR_0 & PFIFO_INTR_EN_0, PMC_INTR_1's is PFIFO_INTR_0 & PFIFO_INTR_EN_1, and a line is up while
// its tree's PFIFO bit passes PMC_INTR_MASK_n. The enables are the ones gk104_fifo_init writes and the masks nvkm's: the
// emulator gates both trees on one enable, so this model and the device are what can tell the routing.
#include <gtest/gtest.h>
#include <cstdint>
#include <map>
#include <regex>
#include <string>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20B/SwitchGpuNonstall.h"

using switch_ios::test::Between;
using switch_ios::test::Block;
using switch_ios::test::SourceText;

namespace
{
struct Gpu {
	uint32_t intr = 0, en0 = 0, en1 = 0, mask0 = 0, mask1 = 0;
	unsigned reads = 0, writes = 0;
	bool endsJobAtAck = false;

	bool stall() const
	{
		return (intr & en0) && (mask0 & SWITCH_GPU_PMC_PFIFO);
	}

	bool nonstall() const
	{
		return (intr & en1) && (mask1 & SWITCH_GPU_PMC_PFIFO);
	}
} gpu;

uint8_t bar0[0x3000];

uint32_t offsetOf(uintptr_t address)
{
	return uint32_t(address - reinterpret_cast<uintptr_t>(bar0));
}

uint32_t written(const std::string &init, uint32_t reg)
{
	char pattern[96];
	snprintf(pattern, sizeof(pattern), "nvkm_wr32\\(device, 0x%06x, (0x[0-9a-f]+)\\);", reg);
	std::smatch match;
	if (!std::regex_search(init, match, std::regex(pattern)))
		return 0;
	return uint32_t(std::stoul(match[1].str(), nullptr, 16));
}

// The enables and masks the kext and nvkm leave set: gk104_fifo_init's, nvkm's leaf-0 PFIFO entry (gk104_mc_intrs) and
// the kext's vector on tree 1 (gpu_nonstall.c).
void boot()
{
	const std::string init = Between(SourceText("../nouveau/nvkm/engine/fifo/gk104.c"),
					 "gk104_fifo_init(struct nvkm_fifo *fifo)\n{", "\n}\n");
	gpu = {};
	gpu.en0 = written(init, SWITCH_GPU_PFIFO_INTR_EN_0);
	gpu.en1 = written(init, SWITCH_GPU_PFIFO_INTR_EN_1);
	if (SourceText("../nouveau/nvkm/subdev/mc/gk104.c")
		    .find("{ NVKM_ENGINE_FIFO    , 0, 0, 0x00000100 },") != std::string::npos)
		gpu.mask0 = SWITCH_GPU_PMC_PFIFO;
	if (SWITCH_GPU_NONSTALL_VECTOR / 32 == 1)
		gpu.mask1 = 1u << (SWITCH_GPU_NONSTALL_VECTOR % 32);
}
}  // namespace

extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
	++gpu.reads;
	return offsetOf(address) == SWITCH_GPU_PFIFO_INTR_0 ? gpu.intr : 0;
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
	++gpu.writes;
	if (offsetOf(address) != SWITCH_GPU_PFIFO_INTR_0)
		return;
	gpu.intr &= ~value;
	if (gpu.endsJobAtAck)
		gpu.intr |= SWITCH_GPU_PFIFO_CHANNEL_INTR;
}

TEST(GpuNonstall, AJobInterruptRaisesTheNonstallLineAlone)
{
	boot();
	gpu.intr = SWITCH_GPU_PFIFO_CHANNEL_INTR;
	EXPECT_TRUE(gpu.nonstall());
	EXPECT_FALSE(gpu.stall()) << "PFIFO_INTR_EN_0 " << std::hex << gpu.en0;
}

TEST(GpuNonstall, FifoErrorsStayOnTheStallLine)
{
	boot();
	gpu.intr = 0x10000000;  // MMU_FAULT
	EXPECT_TRUE(gpu.stall());
	EXPECT_FALSE(gpu.nonstall());
}

TEST(GpuNonstall, ThePassQuietsItsLineWithOneWriteAndOneRead)
{
	boot();
	gpu.intr = SWITCH_GPU_PFIFO_CHANNEL_INTR;
	EXPECT_EQ(switch_gpu_nonstall_ack(bar0), 1);
	EXPECT_FALSE(gpu.nonstall());
	EXPECT_EQ(gpu.writes, 1u);
	EXPECT_EQ(gpu.reads, 1u);
}

// A job that ends between the acknowledgement and the fence reads raises the line again: no job interrupt is lost.
TEST(GpuNonstall, AJobEndingAtTheAcknowledgementRaisesTheLineAgain)
{
	boot();
	gpu.intr = SWITCH_GPU_PFIFO_CHANNEL_INTR;
	gpu.endsJobAtAck = true;
	EXPECT_EQ(switch_gpu_nonstall_ack(bar0), 0);
	EXPECT_TRUE(gpu.nonstall());
}

// The nonstall line's pass takes no nvkm pass (its intr.lock, the irq_polling exchange, its tree reads); the stall
// line's keeps it. The tree's mask is set before the lines are claimed, and a lost interrupt names the job line.
TEST(GpuNonstallSource, TheNonstallPassSkipsNvkmAndThePanicNamesTheLine)
{
	const std::string glue = SourceText("../shim/xnu_glue.cpp");
	const auto pass =
		Block(glue, "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)");
	EXPECT_TRUE(pass.Has("line ? switch_gpu_nonstall_ack(gPmc) : nvxnu_poll_irqs()"));
	EXPECT_TRUE(pass.Before("switch_gpu_nonstall_ack(gPmc)", "tgm_chans_completed();"));
	const auto panic = Block(
		glue,
		"[[noreturn]] void gpuPanic(const char *why, const char *what, uint32_t value)");
	EXPECT_TRUE(panic.Has("job interrupts on the %s line, INTID %u"));
	EXPECT_TRUE(panic.Has("kLines[kJobLine], kGpuIntids[kJobLine]"));
	EXPECT_TRUE(
		Block(SourceText("../gpu_bringup.c"), "static int m1(char *line, unsigned int len)")
			.Before("tgm_nonstall_init(device);", "nvxnu_irq_start();"));
}
