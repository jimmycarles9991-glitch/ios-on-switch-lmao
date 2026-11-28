// SPDX-License-Identifier: MIT
// The HW Test probes' privileged ops, the paths op that hands out host1x syncpoints and the facade's raw 3D-class
// methods (which can increment any syncpoint), exist in debug builds alone and answer only a process holding
// SWITCH_GPU_PROBES_ENTITLEMENT, which HW Test's two apps hold.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::Configured;
using switch_ios::test::SourceText;

namespace
{
const std::string &dispatch()
{
	static const std::string source = SourceText("../TegraGM20BUserClientDispatch.cpp");
	return source;
}

const std::string &entitlement()
{
	static const std::string key = Between(SourceText("../TegraGM20BPathsABI.h"),
					       "#define SWITCH_GPU_PROBES_ENTITLEMENT \"", "\"");
	return key;
}

const std::string &facade()
{
	static const std::string source =
		SourceText("../../TegraGM20BMetal/TegraGM20BMetalProbe.mm");
	return source;
}
}  // namespace

TEST(ProbeGate, TheSyncpointOpIsInDebugKernelsAlone)
{
	const CodeText release(Configured(dispatch(), "TGM_DEBUG_PROBES", false));
	EXPECT_EQ(release.count("TegraGM20BPathsOpProbeSyncpts"), 0u);
	EXPECT_EQ(release.count("probeSyncptsLine"), 0u);
	EXPECT_EQ(release.count("tegra_host1x_syncpt_alloc"), 0u);
}

TEST(ProbeGate, TheSyncpointOpAnswersOnlyTheProbesEntitlement)
{
	const CodeText debug(Configured(dispatch(), "TGM_DEBUG_PROBES", true));
	EXPECT_TRUE(
		debug.has("else if (op == TegraGM20BPathsOpProbeSyncpts) {"
			  " if (!switch_gpu_task_entitled(task_, SWITCH_GPU_PROBES_ENTITLEMENT))"
			  " return kIOReturnNotPrivileged;"
			  " probeSyncptsLine(reply->line, sizeof(reply->line)); }"));
	EXPECT_EQ(debug.count("probeSyncptsLine("), 2u);
}

TEST(ProbeGate, TheFacadeProbesAreInDebugBuildsAlone)
{
	const std::string &source = facade();
	const size_t guard = source.find("#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG\n");
	ASSERT_NE(guard, std::string::npos);
	EXPECT_EQ(source.find("#if"), guard);
	EXPECT_LT(guard, source.find("static bool probesEntitled()"));
	EXPECT_LT(guard, source.find("@implementation"));
	EXPECT_EQ(source.substr(source.rfind("@end")), "@end\n#endif\n");
}

TEST(ProbeGate, EveryRawMethodRouteAsksForTheProbesEntitlement)
{
	const std::string &source = facade();
	EXPECT_TRUE(Block(source, "static bool probesEntitled()")
			    .has("xpc_copy_entitlement_for_self(SWITCH_GPU_PROBES_ENTITLEMENT)"));
	const CodeText arm = Block(
		source,
		"- (NSString *)switchMetalProbeBefore:(NSData *)before after:(NSData *)after");
	EXPECT_TRUE(
		arm.before("if (!probesEntitled()) return kProbesUnentitled;", "appendMethods("));
	EXPECT_EQ(CodeText(source).count("d.probeWords = "), 1u);
	const CodeText engine =
		Block(source, "- (NSString *)switchMetalProbeEngine:(NSDictionary *)op");
	EXPECT_TRUE(engine.before("if (method && !probesEntitled()) return kProbesUnentitled;",
				  "ProbeOpKind::Method"));
	EXPECT_EQ(CodeText(source).count("ProbeOpKind::Method"), 1u);
}

TEST(ProbeGate, HWTestHoldsTheProbesEntitlement)
{
	ASSERT_EQ(entitlement().rfind("com.brunostjohn.switch.", 0), 0u);
	const std::string key = "<key>" + entitlement() + "</key><true/>";
	for (const char *app : {"SwitchHWTest.entitlements", "SwitchHWTest27.entitlements"})
		EXPECT_TRUE(
			CodeText(SourceText(std::string("../../../apps/hwtest/") + app)).has(key))
			<< app;
}
