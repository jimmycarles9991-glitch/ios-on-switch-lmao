// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include "../SwitchGpuBindings.h"

namespace
{
struct Binding {
	uint64_t va, range;
};

TEST(GpuBindings, EmptyAndGapsHaveNoResource)
{
	EXPECT_EQ(switch_gpu_binding_find<Binding>(nullptr, 0, 0x1000, 1), nullptr);
	const Binding bindings[] = {{0x1000, 0x1000}, {0x3000, 0x2000}};
	for (uint64_t address : {0ull, 0xfffull, 0x2000ull, 0x2fffull, 0x5000ull, UINT64_MAX})
		EXPECT_EQ(switch_gpu_binding_find(bindings, 2, address, 1), nullptr);
	EXPECT_EQ(switch_gpu_binding_find(bindings, 2, 0x1000, 0), nullptr);
}

TEST(GpuBindings, FullExtentMustStayInOneBinding)
{
	const Binding bindings[] = {{0x1000, 0x1000}, {0x2000, 0x1000}};
	EXPECT_EQ(switch_gpu_binding_find(bindings, 2, 0x1000, 0x1000), &bindings[0]);
	EXPECT_EQ(switch_gpu_binding_find(bindings, 2, 0x1fff, 1), &bindings[0]);
	EXPECT_EQ(switch_gpu_binding_find(bindings, 2, 0x1fff, 2), nullptr);
	EXPECT_EQ(switch_gpu_binding_find(bindings, 2, 0x1000, UINT64_MAX), nullptr);
	EXPECT_EQ(switch_gpu_binding_find(bindings, 2, 0x2000, 1), &bindings[1]);
	EXPECT_EQ(switch_gpu_binding_find(bindings, 2, 0x2fff, 1), &bindings[1]);
}

TEST(GpuBindings, UpperBoundPreservesInsertionOrder)
{
	const Binding bindings[] = {{0x1000, 0x1000}, {0x3000, 0x1000}, {0x9000, 0x1000}};
	EXPECT_EQ(switch_gpu_binding_upper(bindings, 3, 0), 0u);
	EXPECT_EQ(switch_gpu_binding_upper(bindings, 3, 0x1000), 1u);
	EXPECT_EQ(switch_gpu_binding_upper(bindings, 3, 0x2000), 1u);
	EXPECT_EQ(switch_gpu_binding_upper(bindings, 3, 0x5000), 2u);
	EXPECT_EQ(switch_gpu_binding_upper(bindings, 3, UINT64_MAX), 3u);
}

TEST(GpuBindings, ExactAliasExtentResolvesToBackingOffsetWithoutOverflow)
{
	struct Bo {
		uint64_t bytes;
	} bo{0x8000};

	struct Alias {
		uint64_t va, range;
		Bo *bo;
		uint64_t bo_offset;
	} alias{0x100000, 0x1000, &bo, 0x3000};

	uint64_t offset = 0;
	EXPECT_TRUE(switch_gpu_binding_offset(alias, 0x100080, 64, offset));
	EXPECT_EQ(offset, 0x3080u);
	EXPECT_FALSE(switch_gpu_binding_offset(alias, 0x100fff, 2, offset));
	alias.bo_offset = 0x7800;
	EXPECT_FALSE(switch_gpu_binding_offset(alias, 0x100800, 1, offset));
	alias.bo_offset = UINT64_MAX;
	EXPECT_FALSE(switch_gpu_binding_offset(alias, 0x100000, 1, offset));
}

TEST(GpuBindings, LargeBindingTableFindsEveryInteriorAndRejectsEveryGap)
{
	std::array<Binding, 4096> bindings{};
	for (uint64_t i = 0; i < bindings.size(); ++i)
		bindings[i] = {0x100000 + i * 0x10000, 0x8000};
	for (const auto &binding : bindings) {
		for (uint64_t offset : {0ull, 1ull, 0x7000ull, 0x7fffull})
			EXPECT_EQ(switch_gpu_binding_find(bindings.data(), bindings.size(),
							  binding.va + offset, 1),
				  &binding);
		EXPECT_EQ(switch_gpu_binding_find(bindings.data(), bindings.size(),
						  binding.va + 0x8000, 1),
			  nullptr);
	}
}
}  // namespace
