// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/TegraGM20BPushScan.h on the host: the kernel's stats method-stream reader over streams laid out
// as NVK writes them for GM20B (a Core Animation-like submission: the command buffer prologue, a pass into a pitch-linear
// 720x1280 target, its draws in a push of their own, barriers, NVK's copy-engine WFI, a method continued across two pushes;
// a multisampled pass; a push the kernel cannot read) and the raw path's, checking every counter and each push's profile
// class and target image. A push the
// profile may stamp after must end on a method boundary: the stamp would otherwise land inside a method's data.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../TegraGM20BPushScan.h"

using namespace tgm_scan;

#define CHECK(cond) EXPECT_TRUE(cond) << #cond

// NV906F DMA headers
static uint32_t inc(uint32_t subc, uint32_t mthd, uint32_t count)
{
	return (1u << 29) | (count << 16) | (subc << 13) | (mthd >> 2);
}

static uint32_t one_inc(uint32_t subc, uint32_t mthd, uint32_t count)
{
	return (5u << 29) | (count << 16) | (subc << 13) | (mthd >> 2);
}

static uint32_t immd(uint32_t subc, uint32_t mthd, uint32_t data)
{
	return (4u << 29) | (data << 16) | (subc << 13) | (mthd >> 2);
}

using Push = std::vector<uint32_t>;

// nvk_BeginCommandBuffer's prologue for a primary (begin_compute + begin_graphics)
static void prologue(Push &p)
{
	for (uint32_t w : {0x800020a6u, 0x80002509u, 0x80002091u, 0x20020509u, 0u, 0u, 0x90000369u})
		p.push_back(w);
}

// one color target and the pass's begin: SET_COLOR_TARGET_A..MEMORY(0), SET_CT_SELECT, SET_ZT_SELECT, SET_ANTI_ALIAS
static void begin_pass(Push &p, uint32_t width, uint32_t height, bool pitch, uint32_t samples_log2,
		       bool depth)
{
	p.push_back(inc(0, 0x0800, 6));
	for (uint32_t w : {0x3eu, 0xf7e0c000u, width, height, 0xcfu, pitch ? (1u << 12) : 0u})
		p.push_back(w);
	p.push_back(immd(0, 0x121c, 1));
	p.push_back(immd(0, 0x1538, depth ? 1 : 0));
	p.push_back(one_inc(0, 0x3800 + 10 * 8, 1));
	p.push_back((0xf0u << 16) | (samples_log2 << 4));
}

static void draws(Push &p, int n)
{
	for (int i = 0; i < n; i++) {
		p.push_back(one_inc(0, 0x3800 + 11 * 8, 4));  // NVK_MME_DRAW and its arguments
		for (uint32_t w : {6u, 1u, 0u, 0u})
			p.push_back(w);
	}
}

struct Result {
	PushCounts c = {};
	std::vector<uint8_t> cls;
};

static Result scan(const std::vector<const Push *> &pushes)
{
	Result r;
	PushParse p = {};
	bool whole = true;
	for (const Push *push : pushes)
		r.cls.push_back(scanPush(r.c, p, whole, push ? push->data() : nullptr,
					 push ? push->size() : 0));
	return r;
}

struct CoreAnimation {
	Push p0, p1, p2, p3, p4, p5;

	CoreAnimation()
	{
		prologue(p0);
		begin_pass(p0, 720 * 4, 1280, true, 0, false);  // pitch: WIDTH is bytes
		draws(p1, 3);
		p2 = {immd(0, 0x0110, 0), immd(0, 0x021c, 0x14),
		      immd(0, 0x1288, 0)};                               // WFI, SHINV+flush, texinv
		p3 = {inc(4, 0x0418, 2), 0, 0, immd(4, 0x0300, 0x186)};  // NVK's copy-engine WFI
		p4 = {inc(0, 0x1b00, 4), 0x3e, 0x1000};  // a report: two of four words here
		p5 = {0, 0x10000002, immd(0, 0x0050, 0), immd(0, 0x0034, 0x0e),
		      immd(0, 0x0da4, 0x10)};  // the rest, SET_REFERENCE,
					       // L2 sysmem invalidate, shinv
	}
};

TEST(GM20BPushScan, CoreAnimationSubmission)
{
	const CoreAnimation ca;
	Result a = scan({&ca.p0, &ca.p1, &ca.p2, &ca.p3, &ca.p4, &ca.p5});
	CHECK(a.c.passes == 1 && a.c.depthPasses == 0 && a.c.msaaPasses == 0);
	CHECK(a.c.targets == 1 && a.c.linearTargets == 1);
	CHECK(a.c.draws == 3 && a.c.clears == 0);
	CHECK(a.c.wfi == 1 && a.c.shaderInval == 1 && a.c.texInval == 1);
	CHECK(a.c.shaderInvalNoWfi == 2);  // the prologue's constant invalidate and p5's
	CHECK(a.c.ceWfi == 1 && a.c.ceCopies == 0 && a.c.ceBytes == 0);
	CHECK(a.c.setRef == 1 && a.c.l2Inval == 1 && a.c.hostWfi == 0);
	CHECK(a.c.unparsed == 0);
	CHECK(a.cls[0] == TGM_PC_PASS);
	CHECK(a.cls[1] == TGM_PC_DRAW + 0 * 4 + 2 + 0);  // full (921600 pixels), pitch, one sample
	CHECK(a.cls[2] == TGM_PC_SYNC);
	CHECK(a.cls[3] == TGM_PC_SYNC);
	CHECK(a.cls[4] ==
	      TGM_PC_SKIP);  // its method continues into p5: no stamp may go between them
	CHECK(a.cls[5] == TGM_PC_SYNC);
}

TEST(GM20BPushScan, MultisampledPassCopyClearDispatch)
{
	Push q0, q1, q2, q3;
	prologue(q0);
	begin_pass(q0, 1280, 720, false, 2, true);
	draws(q0, 2);  // inline draws: the push is a draw push
	q1 = {inc(4, 0x0418, 2), 4096, 64,
	      immd(4, 0x0300, (1u << 9) | 0x186)};  // 64 lines of 4 KiB
	q2 = {};
	prologue(q2);
	begin_pass(q2, 128, 64, false, 0, false);
	q2.push_back(one_inc(0, 0x3800 + 2 * 8, 2));  // NVK_MME_CLEAR
	q2.push_back(0x3c);
	q2.push_back(0);
	q3 = {immd(1, 0x02b4, 0)};  // a dispatch
	Result b = scan({&q0, &q1, &q2, &q3});
	CHECK(b.c.passes == 2 && b.c.depthPasses == 1 && b.c.msaaPasses == 1);
	CHECK(b.c.draws == 2 && b.c.clears == 1 && b.c.dispatches == 1);
	CHECK(b.c.ceCopies == 1 && b.c.ceBytes == 4096 * 64 && b.c.ceWfi == 0);
	CHECK(b.cls[0] == TGM_PC_DRAW + 0 * 4 + 0 + 1);  // full, block-linear, multisampled
	CHECK(b.cls[1] == TGM_PC_COPY);
	CHECK(b.cls[2] == TGM_PC_CLEAR + 2 * 4 + 0 + 0);  // small, block-linear, one sample
	CHECK(b.cls[3] == TGM_PC_COMPUTE);
}

// A framebuffer fetch's barrier as NVIDIA orders it (PIXEL_SHADER_BARRIER, then the texture invalidate 0x0f74 it
// orders), and a state that turns the pixel interlock on (pixel conflicts, ordered) and one that turns it off.
// The raw path's 4x pass as it writes one: no NVK macro, SET_ANTI_ALIAS 2X2 itself in the pass's prologue and 1X1 again
// after its last draw, in that draw's push; the target is the 720x1280 image's 1440x2560 sample grid. Its resolve pass
// draws the 720x1280 one-sample image.
static void raw_target(Push &p, uint32_t width, uint32_t height)
{
	p.push_back(inc(0, 0x0800, 6));
	for (uint32_t w : {0x3eu, 0xf7e0c000u, width, height, 0xcfu, 0u})
		p.push_back(w);
	p.push_back(immd(0, 0x121c, 1));
	p.push_back(immd(0, 0x1538, 1));
}

static void raw_draw(Push &p)
{
	p.push_back(immd(0, 0x1618, 0));
	p.push_back(immd(0, 0x1614, 0));
}

TEST(GM20BPushScan, RawMultisampledPassAndItsResolve)
{
	Push t0, t1, t2, t3;
	raw_target(t0, 1440, 2560);
	t0.push_back(immd(0, 0x15d0, 2));
	t0.push_back(immd(0, 0x19d0, 0x3c));
	raw_draw(t1);
	raw_draw(t2);
	t2.push_back(immd(0, 0x15d0, 0));
	raw_target(t3, 720, 1280);
	raw_draw(t3);
	PushParse p = {};
	PushCounts c = {};
	bool whole = true;
	CHECK(scanPush(c, p, whole, t0.data(), t0.size()) == TGM_PC_CLEAR + 0 * 4 + 0 + 1);
	CHECK(targetPixels(p) == 1440 * 2560);
	CHECK(scanPush(c, p, whole, t1.data(), t1.size()) == TGM_PC_DRAW + 0 * 4 + 0 + 1);
	CHECK(scanPush(c, p, whole, t2.data(), t2.size()) == TGM_PC_DRAW + 0 * 4 + 0 + 1);
	const TargetImage drawn = targetImage(p);
	CHECK(drawn.width == 720 && drawn.height == 1280 && drawn.samples == 4);
	CHECK(scanPush(c, p, whole, t3.data(), t3.size()) == TGM_PC_DRAW + 0 * 4 + 0 + 0);
	const TargetImage resolved = targetImage(p);
	CHECK(resolved.width == 720 && resolved.height == 1280 && resolved.samples == 1);
	CHECK(c.passes == 2 && c.msaaPasses == 1 && c.draws == 3 && c.clears == 1);
}

// Each SET_ANTI_ALIAS mode's sample grid, NVK's macro (log2 samples in bits 7:4) and the method alike: a target's WIDTH and
// HEIGHT over the grid are its image's.
TEST(GM20BPushScan, SampleGridOfEachMode)
{
	struct Mode {
		uint32_t mode, across, down;
	};

	for (const Mode m : {Mode{0, 1, 1}, Mode{1, 2, 1}, Mode{2, 2, 2}, Mode{3, 4, 2},
			     Mode{4, 4, 2}, Mode{5, 2, 1}, Mode{6, 4, 4}, Mode{8, 2, 2},
			     Mode{9, 2, 2}, Mode{10, 4, 2}, Mode{11, 4, 2}}) {
		Push q;
		raw_target(q, 64 * m.across, 32 * m.down);
		q.push_back(immd(0, 0x15d0, m.mode));
		raw_draw(q);
		PushParse p = {};
		PushCounts c = {};
		bool whole = true;
		scanPush(c, p, whole, q.data(), q.size());
		const TargetImage image = targetImage(p);
		EXPECT_EQ(image.width, 64u) << m.mode;
		EXPECT_EQ(image.height, 32u) << m.mode;
		EXPECT_EQ(image.samples, m.across * m.down) << m.mode;
	}
	for (uint32_t log2 = 0; log2 <= 3; log2++) {
		Push q;
		begin_pass(q, 128 << ((log2 + 1) / 2), 64 << (log2 / 2), false, log2, false);
		draws(q, 1);
		PushParse p = {};
		PushCounts c = {};
		bool whole = true;
		scanPush(c, p, whole, q.data(), q.size());
		const TargetImage image = targetImage(p);
		EXPECT_EQ(image.width, 128u) << log2;
		EXPECT_EQ(image.height, 64u) << log2;
		EXPECT_EQ(image.samples, 1u << log2) << log2;
	}
}

TEST(GM20BPushScan, PixelShaderBarrierAndInterlock)
{
	Push s0 = {immd(0, 0x0de0, 0), immd(0, 0x0f74, 0)};
	Push s1 = {immd(0, 0x1224, 2), immd(0, 0x1224, 0)};
	Result e = scan({&s0, &s1});
	CHECK(e.c.psBarriers == 1 && e.c.texInval == 1 && e.c.wfi == 0);
	CHECK(e.c.interlocks == 1);
	CHECK(e.cls[0] == TGM_PC_SYNC && e.cls[1] == TGM_PC_STATE);
}

// A hang's line names the last engine method of the hung submission: F-v1's twod_clear ended on the 2D engine's
// RENDER_SOLID_PRIM_POINT_Y (subchannel 3, 0x060c); host methods (a semaphore after it) do not replace it.
TEST(GM20BPushScan, TheLastEngineMethodSurvivesHostMethodsAfterIt)
{
	Push fill = {inc(3, 0x0600, 4), 1, 1, 39, 23, immd(0, 0x0078, 0)};
	PushCounts c = {};
	PushParse p = {};
	CHECK(p.lastEngine == 0);
	pushParse(c, p, fill.data(), fill.size());
	CHECK(p.lastEngine == (3u << 16 | 0x060cu));
}

TEST(GM20BPushScan, UnreadablePushEndsTheStamps)
{
	const CoreAnimation ca;
	Result c = scan({&ca.p0, nullptr, &ca.p1, &ca.p2});
	CHECK(c.cls[0] == TGM_PC_PASS);
	CHECK(c.cls[1] == TGM_PC_SKIP && c.cls[2] == TGM_PC_SKIP && c.cls[3] == TGM_PC_SKIP);
	CHECK(c.c.draws == 3);  // still counted
}

TEST(GM20BPushScan, UnknownOpcodeEndsTheStamps)
{
	const CoreAnimation ca;
	Push bad = {0x00000001u};  // GRP0 use-tert: not one of NVK's
	Result d = scan({&ca.p2, &bad, &ca.p2});
	CHECK(d.cls[0] == TGM_PC_SYNC && d.cls[1] == TGM_PC_SKIP && d.cls[2] == TGM_PC_SKIP &&
	      d.c.unparsed == 1);
}

// 5. Draws shaded per sample: NVK's SET_ANTI_ALIAS macro keeps samples (bits 7:4) and log2(1 / minSampleShading) (bits
// 3:0) by masked writes (nvk_mme_set_anti_alias); a draw runs log2 samples - that passes of its fragment shader. A 4x pass
// with sample shading off (minSampleShading 0: 1 / 16, log2 4) shades once a pixel; with minSampleShading 1 (log2 0) four
// times; before this submission wrote the shading half, a draw is not counted either way.
static void min_sample_shading(Push &p, uint32_t rcp_log2)
{
	p.push_back(one_inc(0, 0x3800 + 10 * 8, 1));
	p.push_back((0x0fu << 16) | rcp_log2);
}

TEST(GM20BPushScan, PerSampleDraws)
{
	Push r0, r1, r2, r3;
	prologue(r0);
	begin_pass(r0, 1280, 720, false, 2, true);
	draws(r0, 2);  // shading half not written in this submission
	min_sample_shading(r1, 4);
	draws(r1, 3);  // one pass
	min_sample_shading(r2, 0);
	draws(r2, 5);  // four passes
	min_sample_shading(r3, 1);
	draws(r3, 1);  // two passes
	Result e = scan({&r0, &r1, &r2, &r3});
	CHECK(e.c.draws == 11);
	CHECK(e.c.sampleDraws == 6);
	CHECK(e.c.msaaPasses == 1);
	for (uint8_t cls : e.cls)
		CHECK(cls ==
		      TGM_PC_DRAW + 0 * 4 + 0 + 1);  // the shading writes keep the pass's samples
	// a one-sample pass never shades per sample
	Push s0;
	prologue(s0);
	begin_pass(s0, 1280, 720, false, 0, false);
	min_sample_shading(s0, 0);
	draws(s0, 4);
	Result f = scan({&s0});
	CHECK(f.c.draws == 4 && f.c.sampleDraws == 0);
}

// An indirect command buffer's command as the raw path writes it (raw/runtime/raw_icb.h): its topology, then the gated
// draw macro with its index and Metal's indirect draw arguments
static void icb_command(Push &p, uint32_t index, bool indexed)
{
	p.push_back(immd(0, 0x1970, 5));  // SET_PRIMITIVE_TOPOLOGY
	p.push_back(one_inc(0,
			    0x3800 + 8 * (indexed ? SWITCH_GM20B_MACRO_ICB_DRAW_INDEXED
						  : SWITCH_GM20B_MACRO_ICB_DRAW),
			    5));
	for (uint32_t w : {index, 4u, 1u, 0u, 0u})
		p.push_back(w);
}

static PushCounts scan_raw(const std::vector<const Push *> &pushes, bool raw)
{
	PushParse p = {};
	p.rawMacros = raw;
	PushCounts c = {};
	bool whole = true;
	for (const Push *push : pushes)
		scanPush(c, p, whole, push->data(), push->size());
	return c;
}

// A raw job's indirect command buffer draws count where their gate admits them: inside the range the execute wrote to the
// shadow scratch, every one where the GPU reads the range. NVK's macros at the same slots are no draws.
TEST(GM20BPushScan, RawIndirectCommandBufferDrawsInTheirRange)
{
	Push range, slots;
	range.push_back(inc(0, 0x3400 + 4 * SWITCH_GM20B_SCRATCH_ICB_LOCATION, 2));
	range.push_back(3);
	range.push_back(2);
	icb_command(slots, 3, false);
	icb_command(slots, 4, true);
	icb_command(slots, 5, false);
	const PushCounts c = scan_raw({&range, &slots}, true);
	CHECK(c.draws == 2);

	Push read, words;
	read.push_back(one_inc(0, 0x3800 + 8 * SWITCH_GM20B_MACRO_ICB_RANGE, 2));
	words.push_back(3);
	words.push_back(2);
	const PushCounts g = scan_raw({&read, &words, &slots}, true);
	CHECK(g.draws == 3);

	CHECK(scan_raw({&range, &slots}, false).draws == 0);
}

// A call of macro `slot` with its arguments, as the raw path writes one (raw/runtime/raw_mme.h)
static Push macro_call(uint32_t slot, const std::vector<uint32_t> &arguments)
{
	Push p = {one_inc(0, 0x3800 + 8 * slot, uint32_t(arguments.size()))};
	p.insert(p.end(), arguments.begin(), arguments.end());
	return p;
}

// One raw job per kind of draw and clear the raw path writes, into a 1280x720 one-sample block-linear target, an indirect
// command buffer's commands inside their execute's range: each counts once as a draw or a clear and latches that target,
// which its push's profile class names. An Exec's stream reads the raw path's own slots as NVK's macros there: no draws.
TEST(GM20BPushScan, EachRawDrawAndClearCountsOnceAndLatchesItsTarget)
{
	constexpr uint32_t kIndex = 7;
	std::vector<uint32_t> patch(3 + 3 * SWITCH_GM20B_ICB_PATCH_BUFFERS +
					    SWITCH_GM20B_ICB_PATCH_CONTROL_WORDS + 2,
				    0);
	patch[0] = kIndex;

	struct Kind {
		const char *name;
		Push call;
		bool clear, rawSlot;
	};

	const Kind kinds[] = {
		{"draw", macro_call(SWITCH_GM20B_MACRO_DRAW, {1, 0, 6}), false, false},
		{"indexed draw", macro_call(SWITCH_GM20B_MACRO_DRAW_INDEXED, {1, 0, 6}), false,
		 false},
		{"ICB draw", macro_call(SWITCH_GM20B_MACRO_ICB_DRAW, {kIndex, 6, 1, 0, 0}), false,
		 true},
		{"ICB indexed draw",
		 macro_call(SWITCH_GM20B_MACRO_ICB_DRAW_INDEXED, {kIndex, 6, 1, 0, 0, 0}), false,
		 true},
		{"patch draw", macro_call(SWITCH_GM20B_MACRO_ICB_PATCHES, patch), false, true},
		{"amplified draw", macro_call(SWITCH_GM20B_MACRO_AMPLIFY, {6, 1, 0, 0}), false,
		 true},
		{"amplified indexed draw",
		 macro_call(SWITCH_GM20B_MACRO_AMPLIFY_INDEXED, {6, 1, 0, 0, 0}), false, true},
		{"clear", {immd(0, 0x19d0, 0x3c)}, true, false},  // CLEAR_SURFACE
		{"layered clear", macro_call(SWITCH_GM20B_MACRO_CLEAR_LAYERS, {0x3c, 2}), true,
		 true},
	};
	Push pass;
	raw_target(pass, 1280, 720);
	pass.push_back(inc(0, 0x3400 + 4 * SWITCH_GM20B_SCRATCH_ICB_LOCATION, 2));
	pass.push_back(kIndex);
	pass.push_back(1);
	for (const Kind &k : kinds) {
		for (const bool raw : {true, false}) {
			PushParse p = {};
			p.rawMacros = raw;
			PushCounts c = {};
			bool whole = true;
			scanPush(c, p, whole, pass.data(), pass.size());
			const uint8_t cls = scanPush(c, p, whole, k.call.data(), k.call.size());
			const bool counted = raw || !k.rawSlot;
			EXPECT_EQ(c.draws, counted && !k.clear ? 1u : 0u)
				<< k.name << (raw ? " (job)" : " (Exec)");
			EXPECT_EQ(c.clears, counted && k.clear ? 1u : 0u)
				<< k.name << (raw ? " (job)" : " (Exec)");
			const TargetImage drawn = targetImage(p);
			if (!counted) {
				EXPECT_TRUE(cls == TGM_PC_STATE) << k.name;
				EXPECT_EQ(drawn.width, 0u) << k.name;
				continue;
			}
			// full, block-linear, one sample
			EXPECT_TRUE(cls == (k.clear ? TGM_PC_CLEAR : TGM_PC_DRAW) + 0 * 4 + 0 + 0)
				<< k.name;
			EXPECT_EQ(drawn.width, 1280u) << k.name;
			EXPECT_EQ(drawn.height, 720u) << k.name;
			EXPECT_EQ(drawn.samples, 1u) << k.name;
		}
	}
}
