// SPDX-License-Identifier: MIT
// SwitchGpuRefusalSites.h, and how the GPU job calls use it: every refusal of SubmitJob and ResourceSetCreate names its
// site and values in one line from the call's single exit, but the two flow-control answers, and a connection's
// refusals at one site are rate-bound. Boot 21 and 22 refused metal.native.counters' job (a GpuAddress use of the
// kernel's clock-anchor page, which no binding holds) and iconservicesagent's at 13:34:22.551 with no line at all.
#include <IOKit/IOReturn.h>
#include <gtest/gtest.h>

#include <set>
#include <string>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20B/SwitchGpuRefusalSites.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace
{
constexpr int32_t kBadArgument = kIOReturnBadArgument, kNotReady = kIOReturnNotReady,
		  kCannotLock = kIOReturnCannotLock, kNotResponding = kIOReturnNotResponding;

std::string line(const SwitchGpuRefusalNote &note, int32_t ret, uint32_t channel = 2,
		 uint32_t nth = 0)
{
	char text[320];
	if (!switch_gpu_refusal_line(text, sizeof(text), "iconservicesagen", 209, "SubmitJob",
				     channel, ret, note, nth))
		return "(silent)";
	return text;
}

SwitchGpuRefusalNote note(SwitchGpuRefusal site, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0)
{
	SwitchGpuRefusalNote n;
	switch_gpu_refuse(n, site, 0, a, b, c);
	return n;
}
}  // namespace

TEST(SwitchGpuRefusalSites, EverySiteHasItsOwnTextAndNamesWhatItPrints)
{
	std::set<std::string> texts;
	for (size_t i = 0; i < size_t(SwitchGpuRefusal::Count); ++i) {
		const SwitchGpuRefusalSite &s = switch_gpu_refusal::kSites[i];
		ASSERT_NE(s.what, nullptr) << i;
		EXPECT_TRUE(texts.insert(s.what).second) << s.what;
		for (int v = 0; v < 3; ++v)
			EXPECT_EQ(s.kinds[v] != SwitchGpuRefusalValue::None, s.names[v] != nullptr)
				<< s.what << " value " << v;
	}
}

TEST(SwitchGpuRefusalSites, OnlyTheTwoFlowControlAnswersAreSilent)
{
	std::set<SwitchGpuRefusal> silent;
	for (size_t i = 1; i < size_t(SwitchGpuRefusal::Count); ++i)
		if (switch_gpu_refusal::kSites[i].silent)
			silent.insert(SwitchGpuRefusal(i));
	EXPECT_EQ(silent,
		  (std::set<SwitchGpuRefusal>{SwitchGpuRefusal::StaleSet, SwitchGpuRefusal::Busy}));
	EXPECT_EQ(line(note(SwitchGpuRefusal::StaleSet, 1, 7), kNotReady), "(silent)");
	EXPECT_EQ(line(note(SwitchGpuRefusal::Busy), kCannotLock), "(silent)");
}

// The compositor's (a critical client's) refused job loses its frame, which panics at the refusal: every site but the
// two the runtime resubmits, and neither Aborted (a -5 at admission) nor NotOpen (the process closing). No other
// client's refusal does.
TEST(SwitchGpuRefusalSites, OnlyTheCompositorsRefusalsLoseItsFrame)
{
	constexpr int32_t kAborted = kIOReturnAborted, kNotOpen = kIOReturnNotOpen;
	for (size_t i = 0; i < size_t(SwitchGpuRefusal::Count); ++i) {
		const auto site = SwitchGpuRefusal(i);
		const bool resubmitted =
			site == SwitchGpuRefusal::StaleSet || site == SwitchGpuRefusal::Busy;
		for (const int32_t ret : {kBadArgument, kNotReady, kCannotLock, kNotResponding}) {
			EXPECT_EQ(switch_gpu_refusal_loses_frame(true, site, ret), !resubmitted)
				<< "site " << i << " ret " << std::hex << ret;
			EXPECT_FALSE(switch_gpu_refusal_loses_frame(false, site, ret))
				<< "site " << i;
		}
		EXPECT_FALSE(switch_gpu_refusal_loses_frame(true, site, kAborted)) << "site " << i;
		EXPECT_FALSE(switch_gpu_refusal_loses_frame(true, site, kNotOpen)) << "site " << i;
	}
}

// An app that asks for priority 0 is scheduled high but is not the compositor (tgm_compositor): its refused job loses
// no frame; the compositor's, which also holds graphics-restart-no-kill, does
TEST(SwitchGpuRefusalSites, AnAppAtTheTopPriorityLosesNoFrame)
{
	const enum tgm_level top = tgm_priority_level(uint32_t(tgm_priority_granted(0)), 4, 0);
	EXPECT_FALSE(switch_gpu_refusal_loses_frame(tgm_compositor(top, 0) != 0,
						    SwitchGpuRefusal::Push, kBadArgument));
	EXPECT_TRUE(switch_gpu_refusal_loses_frame(tgm_compositor(top, 1) != 0,
						   SwitchGpuRefusal::Push, kBadArgument));
}

// metal.native.counters' refusal as the next boot's line reads it, and a dead channel's
TEST(SwitchGpuRefusalSites, ALineNamesTheProcessChannelSiteAndValues)
{
	EXPECT_EQ(line(note(SwitchGpuRefusal::UseNoBinding, 3, 0x1fff00040, 4), kBadArgument),
		  "tegra-gm20b: iconservicesagen[209]: SubmitJob on channel 2 refused, 0xe00002c2: "
		  "a shared "
		  "use's GPU address has no binding use=3 address=0x1fff00040 bytes=4");
	EXPECT_EQ(line(note(SwitchGpuRefusal::Exec, switch_gpu_refusal_errno(-5), 12, 3),
		       kNotResponding),
		  "tegra-gm20b: iconservicesagen[209]: SubmitJob on channel 2 refused, 0xe00002ed: "
		  "the channel "
		  "refused the job error=-5 pushes=12 dependencies=3");
	EXPECT_EQ(line(note(SwitchGpuRefusal::UseNoBacking, 0, 0x2a, 4096), kBadArgument),
		  "tegra-gm20b: iconservicesagen[209]: SubmitJob on channel 2 refused, 0xe00002c2: "
		  "a shared "
		  "use's BO has no GPU backing use=0 handle=0x2a bytes=4096");
	EXPECT_EQ(line(note(SwitchGpuRefusal::JobHeader, 0x16b2c0000), kBadArgument, 0),
		  "tegra-gm20b: iconservicesagen[209]: SubmitJob refused, 0xe00002c2: the job "
		  "header is "
		  "unreadable or names no channel address=0x16b2c0000");
}

TEST(SwitchGpuRefusalSites, AnUnnotedRefusalStillHasALine)
{
	EXPECT_EQ(
		line(SwitchGpuRefusalNote{}, kBadArgument),
		"tegra-gm20b: iconservicesagen[209]: SubmitJob on channel 2 refused, 0xe00002c2: a "
		"refusal no site named");
}

// A client refusing itself every job: four lines, then one in 256
TEST(SwitchGpuRefusalSites, ALoopingClientIsRateBound)
{
	unsigned logged = 0;
	for (uint32_t nth = 0; nth < 100000; ++nth)
		logged += line(note(SwitchGpuRefusal::Push, 0, 0x1000, 0), kBadArgument, 1, nth) !=
			  "(silent)";
	EXPECT_EQ(logged, 4u + (100000 - 1) / 256);
	EXPECT_NE(line(note(SwitchGpuRefusal::Push), kBadArgument, 1, 3)
			  .find("from here every 256th"),
		  std::string::npos);
	EXPECT_NE(line(note(SwitchGpuRefusal::Push), kBadArgument, 1, 512).find("refusal 513 here"),
		  std::string::npos);
}

TEST(SwitchGpuRefusalSites, ATruncatedLineStaysTerminated)
{
	char text[24];
	ASSERT_TRUE(switch_gpu_refusal_line(text, sizeof(text), "iconservicesagen", 209,
					    "SubmitJob", 2, kBadArgument,
					    note(SwitchGpuRefusal::Exec, 1, 2, 3), 0));
	EXPECT_EQ(std::string(text), std::string("tegra-gm20b: iconservic"));
}

// Every refusal of the job calls goes through a site: no bare IOReturn, no bare ioreturn() of a callee's error
TEST(SwitchGpuRefusalSitesSource, EveryJobRefusalNamesItsSite)
{
	const std::string submit = SourceText("../TegraGM20BUserClientJobSubmit.cpp");
	const std::string sets = SourceText("../TegraGM20BUserClientJob.cpp");

	const struct {
		const std::string &source;
		const char *head;
	} bodies[] = {
		{submit, "IOReturn AGXDeviceUserClient::takeJob("},
		{submit, "IOReturn AGXDeviceUserClient::runJob("},
		{submit, "IOReturn AGXDeviceUserClient::runJobUses("},
		{sets, "IOReturn AGXDeviceUserClient::resourceSetCreate("},
		{sets, "IOReturn AGXDeviceUserClient::attachSet("},
	};

	for (const auto &b : bodies) {
		const CodeText body = Block(b.source, b.head);
		EXPECT_EQ(body.count("return kIOReturn"), body.count("return kIOReturnSuccess"))
			<< b.head;
		EXPECT_EQ(body.count("return ioreturn("), 0u) << b.head;
		EXPECT_EQ(body.count("? kIOReturnBadArgument :"), 0u) << b.head;
	}
	EXPECT_TRUE(Block(submit, "IOReturn AGXDeviceUserClient::submitJob(")
			    .before("takeJob(in, out, why, channel)",
				    "logRefusal(\"SubmitJob\", channel, result, why)"));
	EXPECT_TRUE(Block(sets, "IOReturn AGXDeviceUserClient::resourceSetCreate(")
			    .before("IOLockUnlock(lock_);",
				    "logRefusal(\"ResourceSetCreate\", 0, result, why)"));
	EXPECT_EQ(CodeText(submit).count("IOLog("), 0u) << "one line, from the exit";
}
