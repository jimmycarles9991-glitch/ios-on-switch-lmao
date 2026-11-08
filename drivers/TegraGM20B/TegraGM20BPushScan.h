// SPDX-License-Identifier: MIT
// The stats' method-stream reader (TegraGM20BUserClient.cpp scanPushes): counts what a submission's pushes ask
// of the GM20B and names each push for the push profile (gpu.c "push profile"). Plain C++ with no kernel dependency, so
// the host test (test/gm20b_push_scan_test.cpp) runs the same code over NVK's streams.
// Encoding: NV906F DMA headers (opcode 31:29, count or immediate 28:16, subchannel 15:13, dword method 11:0); NVK's
// subchannels (Mesa nv_push.h: 0 3D, 1 compute, 3 2D, 4 copy) and MME macros (nvk_mme.h: 2 CLEAR, 10 SET_ANTI_ALIAS,
// 11..16 the draws); method numbers from Mesa's clb197.h, clb1c0.h, clb06f.h, cl902d.h and cla0b5.h.
#pragma once
#include <stdint.h>
#include "TegraGM20BGpu.h"

namespace tgm_scan
{
struct PushCounts {
	uint64_t submits, scanned, bytes, readBytes, unparsed;
	uint64_t passes, depthPasses, msaaPasses, targets, linearTargets, draws, clears;
	uint64_t
		sampleDraws;  // draws shaded once per sample (or per sample pair): NVK's min sample shading above 1 pass
	uint64_t wfi, shaderInval, shaderInvalNoWfi, texInval, setRef, l2Inval, hostWfi;
	uint64_t ceCopies, ceBytes, ceWfi, blits, dispatches, acquires;
	uint64_t
		psBarriers;  // PIXEL_SHADER_BARRIER: later pixel shading waits for earlier pixels, not the whole pipe
	uint64_t interlocks;  // SET_PIXEL_SHADER_INTERLOCK_CONTROL with conflict detection on
};

struct PushParse {
	uint32_t op, subc, mthd,
		left;  // the method in progress (it may continue in the next push of the submission)
	uint32_t ceLine, ceCount;  // the copy engine's LINE_LENGTH_IN / LINE_COUNT
	// the push profile's pass state (scanPushes): color target 0's width (bytes when pitch) and height, its layout, and
	// the sample count of the last SET_ANTI_ALIAS
	uint32_t rtWidth, rtHeight;
	bool rtPitch, msaa;
	// NVK's ANTI_ALIAS scratch as the SET_ANTI_ALIAS macro keeps it (nvk_mme_set_anti_alias: masked writes; bits 7:4
	// log2 samples, 3:0 log2 of 1 / minSampleShading), and whether this submission wrote its shading half yet
	uint32_t antiAlias;
	bool shadingKnown;
};

// Fragment shading passes per pixel, log2, as the macro derives them: log2 samples - log2(1 / minSampleShading), at least 0
inline uint32_t shadingPassesLog2(const PushParse &p)
{
	const int32_t passes = static_cast<int32_t>((p.antiAlias >> 4) & 0xf) -
			       static_cast<int32_t>(p.antiAlias & 0xf);
	return passes > 0 ? static_cast<uint32_t>(passes) : 0;
}

inline void pushMethod(PushCounts &c, PushParse &p, uint32_t subc, uint32_t mthd, uint32_t data)
{
	if (mthd < 0x100) {                            // host methods, any subchannel
		if (mthd == 0x001c && (data & 0xd)) {  // SEMAPHORED: ACQUIRE, ACQ_GEQ, ACQ_AND
			c.acquires++;
		} else if (mthd == 0x0050) {  // SET_REFERENCE (NVK's host barrier)
			c.setRef++;
		} else if (mthd == 0x0034 &&
			   (data & 0x1f) == 0x0e) {  // MEM_OP_D L2_SYSMEM_INVALIDATE
			c.l2Inval++;
		} else if (mthd == 0x0078) {  // WFI
			c.hostWfi++;
		}
		return;
	}
	const bool target =
		mthd >= 0x0800 && mthd < 0x0a00;  // SET_COLOR_TARGET_*(j), 64 bytes apart
	switch (subc) {
	case 0:  // MAXWELL_B
		if (mthd == 0x0110) {
			c.wfi++;  // WAIT_FOR_IDLE
		} else if (mthd == 0x1288 || mthd == 0x1338 || mthd == 0x0f74) {
			c.texInval++;  // INVALIDATE_TEXTURE_DATA_CACHE(_NO_WFI), 0x0f74 the one PIXEL_SHADER_BARRIER orders
		} else if (mthd == 0x0de0) {
			c.psBarriers++;  // PIXEL_SHADER_BARRIER
		} else if (mthd == 0x1224 && (data & 3)) {
			c.interlocks++;  // SET_PIXEL_SHADER_INTERLOCK_CONTROL: TILE_COALESCER_MODE not NO_CONFLICT_DETECT
		} else if (mthd == 0x021c) {
			c.shaderInval++;  // INVALIDATE_SHADER_CACHES (waits for idle)
		} else if (mthd == 0x0da4) {
			c.shaderInvalNoWfi++;  // INVALIDATE_SHADER_CACHES_NO_WFI
		} else if (mthd == 0x1538) {
			c.passes++;  // SET_ZT_SELECT: once per vkCmdBeginRendering
			c.depthPasses += data & 1;
		} else if (mthd == 0x19d0) {
			c.clears++;  // CLEAR_SURFACE
		} else if (mthd == 0x1618) {
			c.draws++;  // BEGIN outside an MME macro
			c.sampleDraws +=
				static_cast<uint64_t>(p.shadingKnown && shadingPassesLog2(p) != 0u);
		} else if (mthd == 0x0808) {
			p.rtWidth = data;  // SET_COLOR_TARGET_WIDTH(0)
		} else if (mthd == 0x080c) {
			p.rtHeight = data;  // SET_COLOR_TARGET_HEIGHT(0)
		} else if (target && (mthd & 0x3f) == 0x10 && data) {
			c.targets++;  // SET_COLOR_TARGET_FORMAT, not DISABLED
		} else if (target && (mthd & 0x3f) == 0x14) {
			const bool pitch =
				(data & (1u << 12)) != 0u;  // SET_COLOR_TARGET_MEMORY LAYOUT_PITCH
			c.linearTargets += static_cast<uint64_t>(pitch);
			if (mthd == 0x0814)
				p.rtPitch = pitch;
		} else if (mthd >= 0x3800 && mthd < 0x3c00 && !(mthd & 7)) {  // CALL_MME_MACRO(j)
			const uint32_t j = (mthd - 0x3800) / 8;
			if (j == 10) {
				const uint32_t mask = data >> 16;
				p.antiAlias = (p.antiAlias & ~mask) | (data & mask);
				p.shadingKnown = p.shadingKnown || ((mask & 0xf) != 0u);
			}
			c.draws += static_cast<uint64_t>(j >= 11 && j <= 16);
			c.sampleDraws += static_cast<uint64_t>(
				j >= 11 && j <= 16 && p.shadingKnown && shadingPassesLog2(p) != 0u);
			c.clears += static_cast<uint64_t>(j == 2);
			// SET_ANTI_ALIAS (once per vkCmdBeginRendering), its argument the macro's data: a sample count
			// (log2 in bits 7:4, under mask 0xf0 in bits 23:16) above one
			c.msaaPasses += static_cast<uint64_t>(
				j == 10 && ((data & 0x00f00000) != 0u) && (data & 0xf0) != 0u);
			if (j == 10 && (data & 0x00f00000))
				p.msaa = ((data & 0xf0) != 0u);
		}
		break;
	case 1:                                                         // MAXWELL_COMPUTE_B
		c.dispatches += static_cast<uint64_t>(mthd == 0x02b4);  // SEND_PCAS_A
		c.wfi += static_cast<uint64_t>(mthd == 0x0110);
		c.shaderInval += static_cast<uint64_t>(mthd == 0x021c);
		c.shaderInvalNoWfi += static_cast<uint64_t>(mthd == 0x0da4);
		c.texInval += static_cast<uint64_t>(mthd == 0x1288 || mthd == 0x1338);
		break;
	case 3:                                                    // FERMI_TWOD_A
		c.blits += static_cast<uint64_t>(mthd == 0x08dc);  // PIXELS_FROM_MEMORY_SRC_Y0_INT
		break;
	case 4:  // MAXWELL_DMA_COPY_A
		if (mthd == 0x0418) {
			p.ceLine = data;
		} else if (mthd == 0x041c) {
			p.ceCount = data;
		} else if (mthd == 0x0300 &&
			   !p.ceLine) {  // LAUNCH_DMA of nothing: NVK's copy-engine WFI
			c.ceWfi++;
		} else if (mthd == 0x0300) {  // LAUNCH_DMA (bit 9 MULTI_LINE)
			c.ceCopies++;
			c.ceBytes += static_cast<uint64_t>(p.ceLine) *
				     ((data & (1u << 9)) ? p.ceCount : 1);
		}
		break;
	default:
		break;
	}
}

inline void pushParse(PushCounts &c, PushParse &p, const uint32_t *w, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		if (p.left) {  // a data word of the method in progress
			pushMethod(c, p, p.subc, p.mthd, w[i]);
			if (p.op == 1) {  // INC: the next word to the next method
				p.mthd += 4;
			} else if (p.op ==
				   5) {  // ONE_INC: every word after the first to the next method
				p.op = 3;
				p.mthd += 4;
			}
			p.left--;
			continue;
		}
		const uint32_t h = w[i];
		const uint32_t op = h >> 29;
		const uint32_t count = (h >> 16) & 0x1fff;
		const uint32_t subc = (h >> 13) & 7;
		const uint32_t mthd = (h & 0xfff) << 2;
		if (!h)
			continue;  // padding
		if (op == 4) {     // IMMD_DATA_METHOD: the data is the count field
			pushMethod(c, p, subc, mthd, count);
		} else if (op == 1 || op == 3 || op == 5) {  // INC, NON_INC, ONE_INC
			p.op = op;
			p.subc = subc;
			p.mthd = mthd;
			p.left = count;
		} else {
			c.unparsed++;
			return;
		}
	}
}

// The push profile's class of one push (TegraGM20BGpu.h TGM_PC_*): what its methods asked of the GPU (before -> after),
// draws and clears by the pass state the parser holds.
inline uint8_t pushClass(const PushCounts &before, const PushCounts &after, const PushParse &p)
{
	const uint64_t pixels =
		static_cast<uint64_t>(p.rtPitch ? p.rtWidth / 4 : p.rtWidth) * p.rtHeight;
	uint8_t area = 2;
	if (pixels >= 600000) {
		area = 0;
	} else if (pixels >= 100000) {
		area = 1;
	}
	const uint8_t variant = area * 4 + (p.rtPitch ? 2 : 0) + (p.msaa ? 1 : 0);
	if (after.draws > before.draws)
		return TGM_PC_DRAW + variant;
	if (after.clears > before.clears)
		return TGM_PC_CLEAR + variant;
	if (after.passes > before.passes)
		return TGM_PC_PASS;
	if (after.dispatches > before.dispatches)
		return TGM_PC_COMPUTE;
	if (after.ceCopies > before.ceCopies)
		return TGM_PC_COPY;
	if (after.blits > before.blits)
		return TGM_PC_2D;
	const uint64_t syncBefore = before.wfi + before.shaderInval + before.shaderInvalNoWfi +
				    before.texInval + before.setRef + before.l2Inval +
				    before.hostWfi + before.ceWfi + before.psBarriers;
	const uint64_t syncAfter = after.wfi + after.shaderInval + after.shaderInvalNoWfi +
				   after.texInval + after.setRef + after.l2Inval + after.hostWfi +
				   after.ceWfi + after.psBarriers;
	return syncAfter > syncBefore ? TGM_PC_SYNC : TGM_PC_STATE;
}

// Read one push of a submission (`w`: its `n` dwords, or null where it cannot be read) into `c`, the parser's state `p`
// carried from the push before, and answer its push-profile class: TGM_PC_SKIP unless the stream read so far is whole
// (every push read, every opcode one NVK writes: `whole`, cleared for good otherwise) and no method continues past its
// end. A stamp between two pushes lands in the method stream between them, so only there may the profile put one.
inline uint8_t scanPush(PushCounts &c, PushParse &p, bool &whole, const uint32_t *w, uint64_t n)
{
	if (!w) {
		p.left = 0;  // a method straddling into a push not read is lost, not misread
		whole = false;
		return TGM_PC_SKIP;
	}
	const PushCounts before = c;
	pushParse(c, p, w, n);
	whole = whole && c.unparsed == before.unparsed;
	return whole && !p.left ? pushClass(before, c, p) : TGM_PC_SKIP;
}
}  // namespace tgm_scan
