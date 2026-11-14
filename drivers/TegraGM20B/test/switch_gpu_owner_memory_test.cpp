// SPDX-License-Identifier: MIT
// SwitchGpuOwnerMemory.h: a connection's counts through its BOs' lives, and the debug line that sums them by process,
// largest first, the rest folded into "others".
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "drivers/TegraGM20B/SwitchGpuOwnerMemory.h"

namespace
{
SwitchGpuOwnerEntry entry(int32_t pid, const char *name, uint64_t imported, uint64_t pool,
			  uint64_t allocated)
{
	SwitchGpuOwnerEntry e = {pid, {}, {}};
	strncpy(e.name, name, sizeof(e.name) - 1);
	const uint64_t bytes[] = {imported, pool, allocated};
	for (uint32_t k = 0; k < SWITCH_GPU_OWNER_KINDS; k++)
		if (bytes[k])
			e.memory.change(SwitchGpuOwnerKind(k), bytes[k], true);
	return e;
}
}  // namespace

TEST(SwitchGpuOwnerMemory, ABoCountsFromItsMakingUntilItIsReadyToRelease)
{
	SwitchGpuOwnerMemory m = {};
	m.change(SWITCH_GPU_OWNER_IMPORTED, 1 << 20, true);
	m.change(SWITCH_GPU_OWNER_IMPORTED, 2 << 20, true);
	m.change(SWITCH_GPU_OWNER_POOL, 64 << 10, true);
	m.change(SWITCH_GPU_OWNER_ALLOCATED, 128 << 10, true);
	m.change(SWITCH_GPU_OWNER_IMPORTED, 1 << 20, false);
	const SwitchGpuOwnerMemory s = m.snapshot();
	EXPECT_EQ(s.bytes[SWITCH_GPU_OWNER_IMPORTED], 2u << 20);
	EXPECT_EQ(s.count[SWITCH_GPU_OWNER_IMPORTED], 1u);
	EXPECT_EQ(s.bytes[SWITCH_GPU_OWNER_POOL], 64u << 10);
	EXPECT_EQ(s.count[SWITCH_GPU_OWNER_ALLOCATED], 1u);
	EXPECT_EQ(s.total(), (2u << 20) + (192u << 10));
	m.change(SWITCH_GPU_OWNER_POOL, 128 << 10, false);
	m.change(SWITCH_GPU_OWNER_POOL, 1, false);
	EXPECT_EQ(m.snapshot().bytes[SWITCH_GPU_OWNER_POOL], 0u);
	EXPECT_EQ(m.snapshot().count[SWITCH_GPU_OWNER_POOL], 0u);
}

TEST(SwitchGpuOwnerMemory, TheLineSumsEachProcessesConnectionsLargestFirst)
{
	SwitchGpuOwnerEntry e[] = {
		entry(30, "SpringBoard", 4 << 20, 1 << 20, 0),
		entry(69, "backboardd", 100 << 20, 16 << 20, 2 << 20),
		entry(535, "WidgetRenderer_D", 0, 0, 0),
		entry(69, "backboardd", 20 << 20, 0, 0),
	};
	char out[512];
	const unsigned long n = switch_gpu_owner_line(e, 4, 8, out, sizeof(out));
	EXPECT_EQ(n, strlen(out));
	EXPECT_EQ(std::string(out), "backboardd[69] 141312 KiB (imports 122880 KiB in 2, pool "
				    "16384 KiB in 1, allocated "
				    "2048 KiB in 1); SpringBoard[30] 5120 KiB (imports 4096 KiB in "
				    "1, pool 1024 KiB in 1, "
				    "allocated 0 KiB in 0)");
}

TEST(SwitchGpuOwnerMemory, ProcessesPastTheShownOnesAreSummed)
{
	SwitchGpuOwnerEntry e[] = {
		entry(1, "a", 1 << 20, 0, 0), entry(2, "b", 3 << 20, 0, 0),
		entry(3, "c", 2 << 20, 0, 0), entry(4, "d", 0, 512 << 10, 0),
		entry(5, "e", 0, 0, 0),
	};
	char out[512];
	switch_gpu_owner_line(e, 5, 2, out, sizeof(out));
	const std::string line(out);
	EXPECT_EQ(line.find("b[2]"), 0u);
	EXPECT_LT(line.find("c[3]"), line.find("others"));
	EXPECT_NE(line.find("others 1536 KiB in 2 processes"), std::string::npos);
	EXPECT_EQ(line.find("e[5]"), std::string::npos);
}

TEST(SwitchGpuOwnerMemory, AShortBufferIsCutAndEnded)
{
	SwitchGpuOwnerEntry e[] = {entry(69, "backboardd", 100 << 20, 0, 0),
				   entry(30, "SpringBoard", 1 << 20, 0, 0)};
	char out[24];
	memset(out, 'x', sizeof(out));
	const unsigned long n = switch_gpu_owner_line(e, 2, 8, out, sizeof(out));
	EXPECT_EQ(n, sizeof(out) - 1);
	EXPECT_EQ(out[sizeof(out) - 1], '\0');
	EXPECT_EQ(switch_gpu_owner_line(e, 2, 8, out, 0), 0u);
}
