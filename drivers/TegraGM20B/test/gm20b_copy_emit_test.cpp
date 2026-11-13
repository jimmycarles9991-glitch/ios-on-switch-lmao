// SPDX-License-Identifier: MIT
// gpu_present.c's and gpu_scrub.c's present, read-back and pool-scrub pushes (TegraGM20BCopyEmit.c, on kits/SwitchGM20BEmit) against the
// hand-encoded words the kernel wrote for the same inputs before it moved onto the kit.
#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BCopyEmit.h"

namespace
{
using Methods = std::map<uint32_t, uint32_t>;

constexpr uint32_t kPitchIn = 0x0410, kPitchOut = 0x0414, kLaunch = 0x0300;

// Each method's last value, and the order of first writes
Methods methods(const std::vector<uint32_t> &w, std::vector<uint32_t> *order = nullptr)
{
	Methods out;
	for (size_t i = 0; i < w.size();) {
		const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff,
			       mthd = (h & 0x1fff) << 2;
		EXPECT_EQ((h >> 13) & 7, 4u);
		const uint32_t n = op == 4 ? 1 : count;
		for (uint32_t k = 0; k < n; k++) {
			const uint32_t value = op == 4 ? count : w[i++];
			if (order && !out.count(mthd + 4 * k))
				order->push_back(mthd + 4 * k);
			out[mthd + 4 * k] = value;
		}
	}
	return out;
}

std::vector<uint32_t> texels(const tgm_copy_side &src, const tgm_copy_side &dst, uint32_t width,
			     uint32_t height)
{
	std::vector<uint32_t> w(TGM_COPY_RECT_DWORDS);
	w.resize(tgm_copy_texels(w.data(), &src, &dst, width, height));
	return w;
}

// Every method the hand-encoded copy wrote, with its value, but a block-linear side's pitch, which the copy engine reads
// only for a pitch-linear side
void expectSameCopy(const std::vector<uint32_t> &ours, const std::vector<uint32_t> &hand,
		    bool srcBlock, bool dstBlock)
{
	Methods a = methods(ours), b = methods(hand);
	EXPECT_EQ(a.count(kPitchIn), 1u);
	EXPECT_EQ(a.count(kPitchOut), 1u);
	if (srcBlock)
		a.erase(kPitchIn), b.erase(kPitchIn);
	if (dstBlock)
		a.erase(kPitchOut), b.erase(kPitchOut);
	EXPECT_EQ(a, b);
	EXPECT_EQ(a.at(kLaunch) & (1u << 20), 0u)
		<< "BYPASS_L2 stays USE_PTE_SETTING, as nvgpu and NVK leave it";
	EXPECT_EQ(ours.back() >> 29, 4u) << "LAUNCH_DMA in its one-word form";
	EXPECT_LT(ours.size(), hand.size());
}

TEST(CopyEmit, PresentFromRowsIsTheHandEncodedCopy)
{
	const std::vector<uint32_t> hand = {
		0x20088100, 0x00000024, 0x12340000, 0x00000044, 0x00000000, 0x00001400,
		0x00000000, 0x00001400, 0x000002d0, 0x200681c3, 0x00001040, 0x00001400,
		0x000002d0, 0x00000001, 0x00000000, 0x00000000, 0x200180c0, 0x00000286};
	expectSameCopy(texels({0x2412340000, 5120, -1}, {0x4400000000, 5120, 4}, 1280, 720), hand,
		       false, true);
}

TEST(CopyEmit, PresentFromBlockLinearIsTheHandEncodedCopy)
{
	const std::vector<uint32_t> hand = {
		0x20088100, 0x00000024, 0x12340000, 0x00000044, 0x00000000, 0x00001500, 0x00000000,
		0x00001400, 0x000002d0, 0x200681c3, 0x00001040, 0x00001400, 0x000002d0, 0x00000001,
		0x00000000, 0x00000000, 0x200681ca, 0x00001040, 0x00001400, 0x000002d0, 0x00000001,
		0x00000000, 0x00000000, 0x200180c0, 0x00000206};
	std::vector<uint32_t> order;
	const std::vector<uint32_t> ours =
		texels({0x2412340000, 1280 * 4, 4}, {0x4400000000, 5120, 4}, 1280, 720);
	methods(ours, &order);
	expectSameCopy(ours, hand, true, true);
	EXPECT_LT(std::find(order.begin(), order.end(), 0x0728u),
		  std::find(order.begin(), order.end(), 0x070cu))
		<< "the source's block-linear state now comes first";
}

TEST(CopyEmit, ReadBacksAreTheHandEncodedCopies)
{
	expectSameCopy(texels({0x4400000000, 5120, -1}, {0x5500000000, 5120, -1}, 1280, 720),
		       {0x20088100, 0x00000044, 0x00000000, 0x00000055, 0x00000000, 0x00001400,
			0x00001400, 0x00001400, 0x000002d0, 0x200180c0, 0x00000386},
		       false, false);
	expectSameCopy(texels({0x4400000000, 5120, 4}, {0x5500000000, 5120, -1}, 1280, 720),
		       {0x20088100, 0x00000044, 0x00000000, 0x00000055, 0x00000000, 0x00000000,
			0x00001400, 0x00001400, 0x000002d0, 0x200681ca, 0x00001040, 0x00001400,
			0x000002d0, 0x00000001, 0x00000000, 0x00000000, 0x200180c0, 0x00000306},
		       true, false);
}

// scrub_submit's whole lines and last pages, and the bring-up self-test's one page
TEST(CopyEmit, FillsAreTheHandEncodedFills)
{
	std::vector<uint32_t> w(TGM_FILL_SETUP_DWORDS + TGM_FILL_PITCH_DWORDS +
				2 * TGM_FILL_LINES_DWORDS);
	uint32_t n = tgm_fill_setup(w.data(), 0);
	n += tgm_fill_pitch(w.data() + n, 128 << 10);
	n += tgm_fill_lines(w.data() + n, 0x8000400000, 128 << 10, 3, 1);
	n += tgm_fill_lines(w.data() + n, 0x8000460000, 0x5000, 1, 0);
	w.resize(n);
	const std::vector<uint32_t> hand = {
		0x200181c0, 0x00000000, 0x200181c2, 0x00030004, 0x20028104, 0x00020000,
		0x00020000, 0x20028102, 0x00000080, 0x00400000, 0x20028106, 0x00008000,
		0x00000003, 0x200180c0, 0x00000786, 0x20028102, 0x00000080, 0x00460000,
		0x20028106, 0x00001400, 0x00000001, 0x200180c0, 0x00000586};
	EXPECT_EQ(w.size() + 2, hand.size()) << "each LAUNCH_DMA in its one-word form";
	std::vector<uint32_t> ours, theirs;
	methods(w, &ours);
	methods(hand, &theirs);
	EXPECT_EQ(ours, theirs);
	for (size_t at = 0, i = 0; at < w.size(); i++) {
		ASSERT_LT(i, hand.size());
		if (w[at] >> 29 == 4) {
			EXPECT_EQ(w[at] & 0x1fff, hand[i] & 0x1fff);
			EXPECT_EQ(w[at] >> 16 & 0x1fff, hand[i + 1]);
			at++, i++;
			continue;
		}
		EXPECT_EQ(w[at++], hand[i]);
	}
	EXPECT_EQ(methods(w)[kLaunch], 0x586u);
}
}  // namespace
