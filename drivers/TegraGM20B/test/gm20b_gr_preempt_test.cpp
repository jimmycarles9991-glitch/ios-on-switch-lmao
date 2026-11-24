// SPDX-License-Identifier: MIT
#include <cerrno>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "gm20b_zcull_context_fixture.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BGrPreemptContext.h"
#include "drivers/TegraGM20B/TegraGM20BZcullContext.h"
}
#include <gtest/gtest.h>

namespace
{
constexpr u32 kCtxSize = 0x1000;
constexpr u32 kZcullModeWord = 0x1c;
constexpr u32 kZcullPtrWord = 0x20;
constexpr u32 kPreemptWord = 0x60;

struct Context : nvkm_gpuobj {
	std::map<u32, u32> words;
};

void contextWrite(nvkm_gpuobj *context, u32 offset, u32 data)
{
	static_cast<Context *>(context)->words[offset] = data;
}

const nvkm_gpuobj_func kContextFunc = {contextWrite};

Context goldenContext()
{
	Context ctx;
	ctx.ptrs = &kContextFunc;
	ctx.words = {
		{kZcullModeWord, TGM_ZCULL_CTX_NO_CTXSW},
		{kZcullPtrWord, 0},
		{kPreemptWord, TGM_GR_CTX_PREEMPT_WFI},
	};
	return ctx;
}

std::string readText(const char *path)
{
	std::ifstream file(path, std::ios::binary);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

std::string extractBody(const std::string &source, const std::string &head,
			const char *close = "\n}\n")
{
	const size_t at = source.find(head);
	if (at == std::string::npos)
		return "";
	const size_t end = source.find(close, at);
	return end == std::string::npos ? source.substr(at) : source.substr(at, end - at);
}

struct ProbeState {
	Context *ctx = nullptr;
	bool enabled = true;
	bool lock_held = false;
	int preempts = 0;
};

int probeOwn(void *arg)
{
	auto *state = static_cast<ProbeState *>(arg);
	if (state->lock_held)
		return 0;
	state->lock_held = true;
	return 1;
}

void probeRelease(void *arg)
{
	static_cast<ProbeState *>(arg)->lock_held = false;
}

void probeDisable(void *arg)
{
	static_cast<ProbeState *>(arg)->enabled = false;
}

void probeEnable(void *arg)
{
	static_cast<ProbeState *>(arg)->enabled = true;
}

int probePreempt(void *arg)
{
	++static_cast<ProbeState *>(arg)->preempts;
	return 0;
}

u32 probeRead(void *arg, u32 offset)
{
	return static_cast<Context *>(arg)->words[offset];
}

void probeWrite(void *arg, u32 offset, u32 value)
{
	static_cast<Context *>(arg)->words[offset] = value;
}

TEST(Gm20bGrPreemptContext, CtaOptionWritesOffset0x60AndPreservesOtherWords)
{
	Context ctx = goldenContext();
	nvkm_vma zcull{0x200040000ULL, 0x1500, 12};

	tgm_zcull_image_bind(&ctx, &zcull);
	tgm_gr_preempt_image_bind(&ctx, kCtxSize, TGM_GR_CTX_PREEMPT_CTA);

	EXPECT_EQ(ctx.words.size(), 3u);
	EXPECT_EQ(ctx.words[kZcullModeWord], static_cast<u32>(TGM_ZCULL_CTX_SEPARATE_BUFFER));
	EXPECT_EQ(ctx.words[kZcullPtrWord], static_cast<u32>(zcull.addr >> 8));
	EXPECT_EQ(ctx.words[kPreemptWord], TGM_GR_CTX_PREEMPT_CTA);
}

TEST(Gm20bGrPreemptContext, ZeroOptionAndTruncatedImageLeavePreemptWordUnchanged)
{
	Context ctx = goldenContext();

	tgm_gr_preempt_image_bind(&ctx, kCtxSize, TGM_GR_CTX_PREEMPT_WFI);
	EXPECT_EQ(ctx.words[kPreemptWord], TGM_GR_CTX_PREEMPT_WFI);

	tgm_gr_preempt_image_bind(&ctx, TGM_GR_CTX_PREEMPT_MIN_SIZE - 4u, TGM_GR_CTX_PREEMPT_CTA);
	EXPECT_EQ(ctx.words[kPreemptWord], TGM_GR_CTX_PREEMPT_WFI);
}

TEST(Gm20bGrPreemptContext, ProbeP1TogglesCleanlyFromBoundCtaContext)
{
	Context ctx = goldenContext();
	tgm_gr_preempt_image_bind(&ctx, kCtxSize, TGM_GR_CTX_PREEMPT_CTA);
	ASSERT_EQ(ctx.words[kPreemptWord], TGM_GR_CTX_PREEMPT_CTA);

	ProbeState state{&ctx};
	tgm_ramfc_steps steps{};
	steps.arg = &state;
	steps.own = probeOwn;
	steps.release = probeRelease;
	steps.disable = probeDisable;
	steps.enable = probeEnable;
	steps.preempt = probePreempt;
	const tgm_gr_preempt_io io = {&ctx, probeRead, probeWrite};
	u32 old_mode = 99;
	u32 readback = 99;

	ASSERT_EQ(tgm_gr_preempt_write_quiesced(&steps, &io, TGM_GR_CTX_PREEMPT_WFI, &old_mode,
						&readback),
		  0);
	EXPECT_EQ(old_mode, TGM_GR_CTX_PREEMPT_CTA);
	EXPECT_EQ(readback, TGM_GR_CTX_PREEMPT_WFI);
	EXPECT_EQ(ctx.words[kPreemptWord], TGM_GR_CTX_PREEMPT_WFI);

	ASSERT_EQ(tgm_gr_preempt_write_quiesced(&steps, &io, TGM_GR_CTX_PREEMPT_CTA, &old_mode,
						&readback),
		  0);
	EXPECT_EQ(old_mode, TGM_GR_CTX_PREEMPT_WFI);
	EXPECT_EQ(readback, TGM_GR_CTX_PREEMPT_CTA);
	EXPECT_EQ(ctx.words[kPreemptWord], TGM_GR_CTX_PREEMPT_CTA);
	EXPECT_EQ(state.preempts, 2);
	EXPECT_TRUE(state.enabled);
	EXPECT_FALSE(state.lock_held);
}

TEST(Gm20bGrPreemptContext, Gm20bGrctxAndChanBindWireCtaPreemption)
{
	const std::string ctxgm20b = readText(CTXGM20B_C);
	const std::string grctx = extractBody(ctxgm20b, "gm20b_grctx = {", "\n};");
	ASSERT_FALSE(grctx.empty());
	EXPECT_NE(grctx.find(".preempt_options = TGM_GR_CTX_PREEMPT_CTA,"), std::string::npos);

	const std::string ctxgf100 = readText(CTXGF100_H);
	const std::string func = extractBody(ctxgf100, "struct gf100_grctx_func {", "\n};");
	ASSERT_FALSE(func.empty());
	EXPECT_NE(func.find("u32 preempt_options;"), std::string::npos);

	const std::string gf100 = readText(GF100_C);
	const std::string bind = extractBody(gf100, "gf100_gr_chan_bind(");
	ASSERT_FALSE(bind.empty());
	const size_t fw = bind.find("if (!gr->firmware)");
	const size_t zcull = bind.find("tgm_zcull_image_bind(*pgpuobj, chan->zcull_vma);");
	const size_t preempt = bind.find("tgm_gr_preempt_image_bind(*pgpuobj, gr->size,");
	ASSERT_NE(fw, std::string::npos);
	ASSERT_NE(zcull, std::string::npos);
	ASSERT_NE(preempt, std::string::npos);
	EXPECT_LT(fw, zcull);
	EXPECT_LT(zcull, preempt);
}
}  // namespace
