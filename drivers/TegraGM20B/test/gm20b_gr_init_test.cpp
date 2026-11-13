// GR's floorsweep at every init, as nvgpu's power-on setup rewrites it (gk20a_init_gr_setup_hw, init_fs_state), and the
// debug kernels' one line per GR context.
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string Text(const char *path)
{
	std::ifstream file(path, std::ios::binary);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

std::string Body(const std::string &source, const std::string &head, const char *close = "\n}\n")
{
	const size_t at = source.find(head);
	if (at == std::string::npos)
		return "";
	return source.substr(at, source.find(close, at) - at);
}

TEST(Gm20bGrInit, FloorsweepsAtEveryInitBeforeFecsStarts)
{
	const std::string init = Body(Text(GK20A_C), "gk20a_gr_init(struct gf100_gr *gr)\n{");
	const size_t rop = init.find("gr->func->init_rop_active_fbps(gr);");
	const size_t sweep = init.find("gf100_grctx_generate_floorsweep(gr);");
	const size_t fecs = init.find("return gf100_gr_init_ctxctl(gr);");
	ASSERT_NE(sweep, std::string::npos);
	EXPECT_LT(rop, sweep);
	EXPECT_LT(sweep, fecs);
	EXPECT_NE(Text(GM20B_C).find(".init = gk20a_gr_init,"), std::string::npos)
		<< "GM20B's GR init";
}

TEST(Gm20bGrInit, TheFloorsweepWritesPdAndDsTpcCountsOnGm20b)
{
	const std::string sweep =
		Body(Text(CTXGF100_C), "gf100_grctx_generate_floorsweep(struct gf100_gr *gr)\n{");
	EXPECT_NE(sweep.find("gf100_gr_init_num_tpc_per_gpc(gr, false, true);"), std::string::npos);
	EXPECT_NE(sweep.find("if "
			     "(!func->skip_pd_num_tpc_per_gpc)\n\t\tgf100_gr_init_num_tpc_per_gpc("
			     "gr, true, false);"),
		  std::string::npos);
	const std::string grctx = Body(Text(CTXGM20B_C), "gm20b_grctx = {", "\n};");
	ASSERT_FALSE(grctx.empty());
	EXPECT_EQ(grctx.find("skip_pd_num_tpc_per_gpc"), std::string::npos);
}

TEST(Gm20bGrInit, EveryContextIsLookedForAtEveryFence)
{
	const std::string chan = Text(GPU_CHAN_C);
	EXPECT_NE(Body(chan, "static int chan_new(").find("tgm_stats_gr_bind(chan);"),
		  std::string::npos);
	EXPECT_NE(Body(chan, "static void chan_free(struct tgm_chan *chan)\n{")
			  .find("tgm_stats_gr_unbind(chan);"),
		  std::string::npos);
	EXPECT_NE(Body(Text(GPU_FENCED_H),
		       "static inline void stats_fences(struct tgm_chan *chan)\n{")
			  .find("tgm_stats_gr_context();"),
		  std::string::npos);
}

}  // namespace
