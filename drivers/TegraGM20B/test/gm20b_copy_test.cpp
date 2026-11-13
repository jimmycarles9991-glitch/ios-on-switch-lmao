// SPDX-License-Identifier: MIT
// TegraGM20BCopyPush.h on the host, through gm20b_ce_model.h: a plane copy's push leaves the destination exactly the source's
// rows at the new pitch (pitch sources of every alignment CoreVideo gives, a block-linear surface read as its rows, both
// planes of a two-plane picture) and nothing else, waits for the source's GPU writers and the asked syncpoint before the
// copy, and increments the asked syncpoint only after the copy is in DRAM.
#include <cstdint>
#include <cstring>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BCopyPush.h"
#include "drivers/TegraGM20B/TegraGM20BLayout.h"
#include "gm20b_ce_model.h"

namespace
{
constexpr uint64_t kSrcVa = 8ull << 32, kDstVa = 9ull << 32;
constexpr uint8_t kUntouched = 0xa5;

std::vector<uint8_t> pattern(size_t n, uint32_t seed)
{
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; i++)
		v[i] = uint8_t(i * 2654435761u >> 11 ^ seed);
	return v;
}

// Runs `job` (its VAs filled in here) over `src` into a destination of `dstBytes`
std::vector<uint8_t> copy(tgm_copy_job job, const std::vector<uint8_t> &src, uint64_t srcOffset,
			  uint64_t dstBytes, uint64_t dstOffset,
			  const std::vector<tgm_relayout_wait> &waits = {},
			  std::vector<CopyEngine::Syncpt> *syncpts = nullptr)
{
	std::vector<uint8_t> s = src, d(dstBytes, kUntouched);
	Memory memory;
	memory.map(kSrcVa, s);
	memory.map(kDstVa, d);
	job.src_va = kSrcVa + srcOffset;
	job.dst_va = kDstVa + dstOffset;
	uint32_t w[TGM_COPY_DWORDS];
	const uint32_t n = tgm_copy_push(w, &job, waits.data(), uint32_t(waits.size()));
	EXPECT_LE(n, TGM_COPY_DWORDS);
	CopyEngine ce(memory);
	ce.run(w, n);
	EXPECT_FALSE(ce.fault);
	EXPECT_EQ(ce.launches, 1u);
	EXPECT_EQ(ce.acquires.size(), waits.size());
	for (size_t i = 0; i < waits.size() && i < ce.acquires.size(); i++)
		EXPECT_EQ(ce.acquires[i], std::make_pair(waits[i].va, waits[i].seqno));
	if (syncpts)
		*syncpts = ce.syncpts;
	EXPECT_EQ(s, src) << "the source is never written";
	return d;
}

tgm_copy_job rowsJob(uint32_t srcPitch, uint32_t dstPitch, uint32_t rowBytes, uint32_t rows)
{
	return {0,
		0,
		srcPitch,
		dstPitch,
		rowBytes,
		rows,
		nullptr,
		nullptr,
		TEGRA_GM20B_CE_NO_SYNCPT,
		0,
		TEGRA_GM20B_CE_NO_SYNCPT};
}

void expectRows(const std::vector<uint8_t> &d, uint64_t dstOffset, uint32_t dstPitch,
		const uint8_t *want, uint32_t wantPitch, uint32_t rowBytes, uint32_t rows)
{
	for (uint64_t i = 0; i < d.size(); i++) {
		const bool inside = i >= dstOffset && (i - dstOffset) % dstPitch < rowBytes &&
				    (i - dstOffset) / dstPitch < rows;
		if (!inside) {
			ASSERT_EQ(d[i], kUntouched) << "byte " << i << " outside the plane";
			continue;
		}
		const uint64_t y = (i - dstOffset) / dstPitch, x = (i - dstOffset) % dstPitch;
		ASSERT_EQ(d[i], want[y * wantPitch + x]) << "row " << y << " byte " << x;
	}
}

// CoreVideo's 64-byte row alignment (and 16, 192, and an unaligned one) into VIC's 256-byte pitch, and back
TEST(Copy, RowsLandAtTheNewPitch)
{
	struct Shape {
		uint32_t rowBytes, srcPitch, rows;
	};

	for (const Shape c :
	     {Shape{720, 768, 9}, Shape{1280, 1280, 4}, Shape{1920 * 4, 1920 * 4 + 64, 3},
	      Shape{360, 384, 7}, Shape{100, 112, 5}, Shape{48, 64, 1}, Shape{4, 16, 6}}) {
		for (const bool toVic : {true, false}) {
			const uint32_t vicPitch = (c.rowBytes + 255) & ~255u;
			const uint32_t from = toVic ? c.srcPitch : vicPitch,
				       to = toVic ? vicPitch : c.srcPitch;
			const uint64_t srcOffset = 48, dstOffset = 4096;
			const std::vector<uint8_t> src =
				pattern(srcOffset + uint64_t(from) * c.rows, c.rowBytes);
			const std::vector<uint8_t> d =
				copy(rowsJob(from, to, c.rowBytes, c.rows), src, srcOffset,
				     dstOffset + uint64_t(to) * c.rows + 512, dstOffset);
			SCOPED_TRACE(testing::Message() << c.rowBytes << " bytes x " << c.rows
							<< ", " << from << " -> " << to);
			expectRows(d, dstOffset, to, src.data() + srcOffset, from, c.rowBytes,
				   c.rows);
		}
	}
}

// NV12: the luma plane and the half-height chroma plane after it, each its own copy into the bounce's two planes
TEST(Copy, BothPlanesOfATwoPlanePicture)
{
	const uint32_t w = 720, h = 480, srcPitch = 768, dstPitch = 768;
	const uint64_t chroma = uint64_t(srcPitch) * h, dstChroma = uint64_t(dstPitch) * h;
	const std::vector<uint8_t> src = pattern(chroma + uint64_t(srcPitch) * h / 2, 7);
	const std::vector<uint8_t> luma =
		copy(rowsJob(srcPitch, dstPitch, w, h), src, 0, dstChroma, 0);
	expectRows(luma, 0, dstPitch, src.data(), srcPitch, w, h);
	const std::vector<uint8_t> cbcr = copy(rowsJob(srcPitch, dstPitch, w, h / 2), src, chroma,
					       uint64_t(dstPitch) * h / 2, 0);
	expectRows(cbcr, 0, dstPitch, src.data() + chroma, srcPitch, w, h / 2);
}

// A block-linear surface (the kernel tiled it) is read as the rows a CPU lock sees
TEST(Copy, ABlockLinearSurfaceIsReadAsItsRows)
{
	struct Shape {
		uint32_t w, h, bpp;
	};

	for (const Shape c : {Shape{720, 1280, 4}, Shape{64, 64, 4}, Shape{100, 60, 2},
			      Shape{33, 43, 1}, Shape{86, 86, 8}}) {
		tgm_layout l;
		ASSERT_TRUE(tgm_layout_tiled(c.w, c.h, c.bpp, &l));
		const uint32_t rowBytes = c.w * c.bpp, dstPitch = (rowBytes + 255) & ~255u;
		const std::vector<uint8_t> rows = pattern(uint64_t(rowBytes) * c.h, c.w);
		std::vector<uint8_t> texels(l.bytes, 0);
		tgm_layout_write(&l, texels.data(), 0, 0, c.w, c.h, rows.data(), rowBytes);
		tgm_copy_job job = rowsJob(0, dstPitch, rowBytes, c.h);
		job.src_tiled = &l;
		const std::vector<uint8_t> d = copy(job, texels, 0, uint64_t(dstPitch) * c.h, 0);
		SCOPED_TRACE(testing::Message() << c.w << "x" << c.h << " bpp " << c.bpp);
		expectRows(d, 0, dstPitch, rows.data(), rowBytes, rowBytes, c.h);
	}
}

// VIC's rows into a block-linear destination surface: its texels are what a CPU lock's rows would write back
TEST(Copy, ABlockLinearDestinationGetsTheRowsAsItsTexels)
{
	struct Shape {
		uint32_t w, h, bpp;
	};

	for (const Shape c : {Shape{720, 1280, 4}, Shape{64, 64, 4}, Shape{100, 60, 2},
			      Shape{33, 43, 1}, Shape{86, 86, 8}}) {
		tgm_layout l;
		ASSERT_TRUE(tgm_layout_tiled(c.w, c.h, c.bpp, &l));
		const uint32_t rowBytes = c.w * c.bpp, srcPitch = (rowBytes + 255) & ~255u;
		const std::vector<uint8_t> src = pattern(uint64_t(srcPitch) * c.h, c.h);
		tgm_copy_job job = rowsJob(srcPitch, 0, rowBytes, c.h);
		job.dst_tiled = &l;
		const std::vector<uint8_t> d = copy(job, src, 0, l.bytes, 0);
		std::vector<uint8_t> want(l.bytes, kUntouched);
		tgm_layout_write(&l, want.data(), 0, 0, c.w, c.h, src.data(), srcPitch);
		SCOPED_TRACE(testing::Message() << c.w << "x" << c.h << " bpp " << c.bpp);
		EXPECT_EQ(d, want);
	}
}

// The source's GPU writers and VIC's syncpoint first, then the copy; the increment after the copy and the L2's write-back
TEST(Copy, WaitsComeFirstAndTheIncrementLast)
{
	tgm_copy_job job = rowsJob(768, 768, 720, 4);
	job.wait_syncpt = 21;
	job.wait_threshold = 1234;
	job.incr_syncpt = 22;
	const std::vector<tgm_relayout_wait> waits = {{0x100000040ull, 7, 5},
						      {0x100000080ull, 9, 6}};
	const std::vector<uint8_t> src = pattern(768 * 4, 3);
	std::vector<CopyEngine::Syncpt> syncpts;
	const std::vector<uint8_t> d = copy(job, src, 0, 768 * 4, 0, waits, &syncpts);
	expectRows(d, 0, 768, src.data(), 768, 720, 4);
	ASSERT_EQ(syncpts.size(), 1u + TGM_SYNCPT_INCRS);
	const CopyEngine::Syncpt first = syncpts.front(), last = syncpts.back();
	EXPECT_FALSE(first.incr);
	EXPECT_EQ(first.id, 21u);
	EXPECT_EQ(first.threshold, 1234u);
	EXPECT_EQ(first.launchesBefore, 0u);
	EXPECT_TRUE(last.incr);
	EXPECT_EQ(last.id, 22u);
	EXPECT_EQ(last.launchesBefore, 1u);
	EXPECT_TRUE(last.flushedBefore);
}

TEST(Copy, NoSyncpointMethodsWhenNoneIsAsked)
{
	const std::vector<uint8_t> src = pattern(64 * 2, 5);
	std::vector<CopyEngine::Syncpt> syncpts;
	copy(rowsJob(64, 256, 48, 2), src, 0, 512, 0, {}, &syncpts);
	EXPECT_TRUE(syncpts.empty());
}
}  // namespace

TEST(Copy, SyncpointReleaseFitsItsExactAdvertisedStorage)
{
	std::vector<uint32_t> words(TGM_SYNCPT_INCR_DWORDS);
	EXPECT_EQ(tgm_syncpt_incr(words.data(), 22), words.size());
	EXPECT_EQ(words[words.size() - 2], tgm_syncpt_header(0x0074));
	EXPECT_EQ(words.back(), (22u << 8) | 1u);
}

namespace
{
void checkLeanChannel(const uint32_t *words, uint32_t count)
{
	for (uint32_t at = 0; at < count;) {
		const uint32_t header = words[at++];
		const uint32_t subchannel = (header >> 13) & 7;
		const uint32_t method = (header & 0x1fff) * 4;
		const bool immediate = header >> 29 == 4;
		const uint32_t arguments = immediate ? 1 : (header >> 16) & 0x1fff;
		ASSERT_TRUE(immediate || header >> 29 == 1u);
		ASSERT_GT(arguments, 0u);
		ASSERT_LE(immediate ? 0 : arguments, count - at);
		if (subchannel == 0)
			EXPECT_LT(method + (arguments - 1) * 4, 0x100u);
		else {
			EXPECT_EQ(subchannel, 4u);
			EXPECT_GE(method, 0x100u);
		}
		at += immediate ? 0 : arguments;
	}
}
}

TEST(Copy, AllCopyAndRelayoutPacketsUseOnlyFifoMethodsAndCeSubchannel)
{
	const tgm_relayout_wait waits[] = {{0x400000, 7, 1}, {0x800000, 9, 2}};
	uint32_t words[TGM_COPY_DWORDS + TGM_RELAYOUT_DWORDS] = {};
	auto job = rowsJob(256, 256, 128, 16);
	job.wait_syncpt = 165;
	job.wait_threshold = 0xf0100000;
	job.incr_syncpt = 166;
	tgm_layout layout = {};
	ASSERT_TRUE(tgm_layout_tiled(64, 32, 4, &layout));
	for (unsigned variant = 0; variant < 3; ++variant) {
		job.src_tiled = variant == 1 ? &layout : nullptr;
		job.dst_tiled = variant == 2 ? &layout : nullptr;
		checkLeanChannel(words, tgm_copy_push(words, &job, waits, 2));
	}
	for (unsigned operation = TGM_RELAYOUT_TO_ROWS; operation <= TGM_RELAYOUT_UNTILE_IN_PLACE;
	     ++operation) {
		const tgm_relayout_move move = {
			0x100000, 0x200000, 0x300000, 256, tgm_relayout_op(operation),
			16384,    0x400000};
		checkLeanChannel(words, tgm_relayout_push(words, &layout, &move, waits, 2));
	}
}
