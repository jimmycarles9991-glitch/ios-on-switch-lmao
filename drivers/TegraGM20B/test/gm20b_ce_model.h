// SPDX-License-Identifier: MIT
// A model of MAXWELL_DMA_COPY_A and the host methods the kernel's copy pushes use (TegraGM20BRelayout.h, TegraGM20BCopyPush.h)
// over modelled GPU memory: remapped component copies and plain byte copies, pitch and block-linear sides (a block-linear side
// addressed by TegraGM20BLayout.h's offsets), constant fills, NON_PIPELINED launches, semaphore acquires and host1x syncpoint
// waits and increments as the host reads them.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BLayout.h"
#include "gm20b_gpu_memory.h"

constexpr uint32_t kCopySubchannel = 4;  // MAXWELL_DMA_COPY_A, as the kernel's channels bind it

struct Side {
	uint32_t blockSize = 0, width = 0, height = 0, depth = 0, layer = 0, origin = 0;
};

// MAXWELL_DMA_COPY_A, the methods the moves use; acquires recorded as they come
struct CopyEngine {
	Memory &memory;
	std::map<uint32_t, uint32_t> m;  // method -> value
	Side src, dst;
	std::vector<std::pair<uint64_t, uint32_t>> acquires;

	struct Syncpt {
		uint32_t id, threshold;
		bool incr;
		uint32_t launchesBefore;
		bool flushedBefore;  // the L2's dirty lines written back since the last launch
	};

	std::vector<Syncpt> syncpts;  // host1x syncpoint waits and increments, in order
	bool flushed = false;
	uint32_t launches = 0;
	bool fault = false;
	uint32_t failLaunch = 0, failAfterBytes = 0;

	explicit CopyEngine(Memory &mem) : memory(mem)
	{
	}

	// `texelBytes`: the remap's texel, bpp()
	uint64_t blockLinear(const Side &s, uint32_t texelBytes, uint32_t xB, uint32_t y) const
	{
		const uint32_t yLog2 = (s.blockSize >> 4) & 0xf;
		return tgm_layout_offset_of(s.width * texelBytes, uint8_t(yLog2), xB, y);
	}

	uint32_t bpp() const
	{
		const uint32_t c = m.at(0x0708);
		return (((c >> 16) & 3) + 1) * (((c >> 20) & 3) + 1);
	}

	void launch(uint32_t flags)
	{
		launches++;
		flushed = false;
		ASSERT_EQ(flags & 3u, 2u) << "NON_PIPELINED";
		ASSERT_TRUE(flags & (1u << 2)) << "FLUSH_ENABLE";
		const bool remap = flags & (1u << 10);
		const uint64_t in = uint64_t(m[0x0400]) << 32 | m[0x0404],
			       out = uint64_t(m[0x0408]) << 32 | m[0x040c];
		const bool srcPitch = flags & (1u << 7), dstPitch = flags & (1u << 8);
		const uint32_t b = remap ? bpp() : 1, w = m[0x0418], h = m[0x041c];
		ASSERT_TRUE(remap || (srcPitch && dstPitch))
			<< "a block-linear side is addressed in texels: remapped";
		if (remap &&
		    (m.at(0x0708) & 7) == 4) {  // DST_X CONST_A: a fill of the constant, no source
			ASSERT_TRUE(dstPitch);
			for (uint32_t y = 0; y < h; y++)
				for (uint32_t x = 0; x < w * b; x++) {
					uint8_t *pd =
						memory.at(out + uint64_t(y) * m[0x0414] + x, true);
					if (!pd) {
						fault = true;
						return;
					}
					*pd = uint8_t(m[0x0700] >> (8 * (x % 4)));
				}
			return;
		}
		uint32_t copied = 0;
		const uint32_t pitchIn = m[0x0410], pitchOut = m[0x0414];
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w * b; x++) {
				const uint64_t s = srcPitch ? in + uint64_t(y) * pitchIn + x
							    : in + blockLinear(src, b, x, y);
				const uint64_t d = dstPitch ? out + uint64_t(y) * pitchOut + x
							    : out + blockLinear(dst, b, x, y);
				uint8_t *ps = memory.at(s), *pd = memory.at(d, true);
				if (!ps || !pd) {
					fault = true;
					return;
				}
				*pd = *ps;
				if (launches == failLaunch && ++copied == failAfterBytes) {
					fault = true;
					return;
				}
			}
	}

	void method(uint32_t subc, uint32_t mthd, uint32_t data)
	{
		if (mthd < 0x100) {
			m[mthd] = data;
			if (mthd == 0x0034 && data == 0x10u << 27)  // MEM_OP_D L2_FLUSH_DIRTY
				flushed = true;
			if (mthd ==
			    0x0074) {  // SYNCPOINTB: INDEX 15:8, WAIT_SWITCH 4, OPERATION 0 (wait) or 1 (incr)
				const uint32_t id = (data >> 8) & 0xff;
				if (data & 1)
					syncpts.push_back({id, 0, true, launches, flushed});
				else
					syncpts.push_back(
						{id, m[0x0070], false, launches, flushed});
			}
			if (mthd == 0x001c) {
				if (data == (2u | 1u << 24)) {
					uint8_t *target = memory.at(
						uint64_t(m[0x0010]) << 32 | m[0x0014], true);
					if (!target) {
						fault = true;
						return;
					}
					ASSERT_TRUE(flushed);
					const uint32_t value = m[0x0018];
					__builtin_memcpy(target, &value, sizeof(value));
					return;
				}
				ASSERT_EQ(data, 0x4u | 1u << 12) << "ACQ_GEQ, ACQUIRE_SWITCH";
				acquires.emplace_back(uint64_t(m[0x0010]) << 32 | m[0x0014],
						      m[0x0018]);
			}
			return;
		}
		ASSERT_EQ(subc, kCopySubchannel);
		m[mthd] = data;
		if (mthd >= 0x0728 && mthd < 0x0740)
			setSide(src, mthd - 0x0728, data);
		else if (mthd >= 0x070c && mthd < 0x0724)
			setSide(dst, mthd - 0x070c, data);
		else if (mthd == 0x0300)
			launch(data);
	}

	static void setSide(Side &s, uint32_t at, uint32_t data)
	{
		uint32_t *f[] = {&s.blockSize, &s.width, &s.height, &s.depth, &s.layer, &s.origin};
		*f[at / 4] = data;
	}

	// The pushbuffer at GPU address `va`, fetched through the page table: an unmapped push faults.
	void runAt(uint64_t va, uint32_t dwords)
	{
		std::vector<uint32_t> words;
		if (!memory.fetch(va, dwords, words)) {
			fault = true;
			return;
		}
		run(words.data(), dwords);
	}

	void run(const uint32_t *w, uint32_t n)
	{
		for (uint32_t i = 0; i < n;) {
			const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff,
				       subc = (h >> 13) & 7, mthd = (h & 0xfff) << 2;
			if (op == 4) {  // IMMD_DATA_METHOD: the count field is the value
				method(subc, mthd, count);
				if (fault)
					return;
				continue;
			}
			ASSERT_EQ(op, 1u) << "INC and immediate headers only: the host raises "
					     "PBENTRY on header "
					  << std::hex << h << " at word " << std::dec << i - 1;
			for (uint32_t k = 0; k < count; k++) {
				method(subc, mthd + 4 * k, w[i++]);
				if (fault)
					return;
			}
		}
	}
};
