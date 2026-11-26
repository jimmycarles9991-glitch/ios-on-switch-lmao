// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuSurfaceComp.h"

namespace
{
constexpr uint64_t kGranule = 65536;

TEST(SwitchGpuSurfaceComp, NonScanout4BppAnd8BppCompress)
{
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 4, 16 * kGranule, 16 * kGranule, 0, 1, 1),
		  SWITCH_GPU_KIND_C32_2CRA);
	EXPECT_EQ(switch_gpu_surface_comp_kind(128, 128, 8, 2 * kGranule, 2 * kGranule, 0, 1, 1),
		  SWITCH_GPU_KIND_C64_2CRA);
	const uint64_t big = 128 * kGranule;
	EXPECT_EQ(switch_gpu_surface_comp_kind(720, 1280, 8, big, 120 * kGranule, 0, 1, 1),
		  SWITCH_GPU_KIND_C64_2CRA);
	EXPECT_TRUE(switch_gpu_surface_kind_compressed(SWITCH_GPU_KIND_C32_2CRA));
	EXPECT_TRUE(switch_gpu_surface_kind_compressed(SWITCH_GPU_KIND_C64_2CRA));
	EXPECT_FALSE(switch_gpu_surface_kind_compressed(0));
	EXPECT_FALSE(switch_gpu_surface_kind_compressed(0xfe));
}

TEST(SwitchGpuSurfaceComp, PanelScanoutAndIneligibleSurfacesStayUncompressed)
{
	EXPECT_EQ(switch_gpu_surface_comp_kind(720, 1280, 4, 64 * kGranule, 60 * kGranule, 0, 1, 1),
		  0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(1280, 720, 4, 64 * kGranule, 60 * kGranule, 0, 1, 1),
		  0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 4, 16 * kGranule, 16 * kGranule, 1, 1, 1),
		  0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 4, 16 * kGranule, 16 * kGranule, 0, 0, 1),
		  0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 4, 16 * kGranule, 16 * kGranule, 0, 1, 0),
		  0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 4, 16 * kGranule + 4096, 16 * kGranule, 0,
					       1, 1),
		  0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 4, 16 * kGranule, 17 * kGranule, 0, 1, 1),
		  0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 4, 0, 0, 0, 1, 1), 0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 2, 8 * kGranule, 8 * kGranule, 0, 1, 1),
		  0u);
	EXPECT_EQ(switch_gpu_surface_comp_kind(512, 512, 16, 64 * kGranule, 64 * kGranule, 0, 1, 1),
		  0u);
}

TEST(SwitchGpuSurfaceComp, ExternalScanoutSuppressesOnlyCompositor4BppWithinDpBounds)
{
	EXPECT_TRUE(switch_gpu_surface_is_external_scanout(1920, 1080, 4, 1, 1));
	EXPECT_TRUE(switch_gpu_surface_is_external_scanout(2560, 1440, 4, 1, 1));
	EXPECT_TRUE(switch_gpu_surface_is_external_scanout(3440, 1440, 4, 1, 1));
	EXPECT_TRUE(switch_gpu_surface_is_external_scanout(3840, 2160, 4, 1, 1));
	EXPECT_TRUE(switch_gpu_surface_is_external_scanout(4096, 2560, 4, 1, 1));
	EXPECT_TRUE(switch_gpu_surface_is_external_scanout(2160, 3840, 4, 1, 1));
	EXPECT_TRUE(switch_gpu_surface_is_external_scanout(4096, 4096, 4, 1, 1));
	EXPECT_FALSE(switch_gpu_surface_is_external_scanout(1920, 1080, 4, 1, 0));
	EXPECT_FALSE(switch_gpu_surface_is_external_scanout(1920, 1080, 4, 0, 1));
	EXPECT_FALSE(switch_gpu_surface_is_external_scanout(1920, 1080, 8, 1, 1));
	EXPECT_FALSE(switch_gpu_surface_is_external_scanout(4097, 1440, 4, 1, 1));
	EXPECT_FALSE(switch_gpu_surface_is_external_scanout(5120, 1440, 4, 1, 1));
	EXPECT_FALSE(switch_gpu_surface_is_external_scanout(1440, 4097, 4, 1, 1));
	EXPECT_FALSE(switch_gpu_surface_is_external_scanout(0, 1080, 4, 1, 1));
}
}  // namespace
