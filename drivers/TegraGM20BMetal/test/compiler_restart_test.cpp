// SPDX-License-Identifier: MIT
// compiler/compiler_restart.h: a compiler that cannot serve declines only for a structural cause, or when nothing starts it
// again or the kernel stopped waiting; a transient one exits 1 for its launcher. A compile context that did not open fails
// that compile alone (Failed), and the next compile opens afresh.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../compiler/compiler_restart.h"
#include "bazel/testing/source_text.h"

#include <mach/kern_return.h>

namespace {

using switch_compiler::connectFailure;
using switch_compiler::Ending;
using switch_compiler::ending;
using switch_compiler::registrationFailure;
using switch_compiler::StartFailure;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(CompilerRestart, OnlyAStructuralCauseOrALastStartDeclines)
{
    EXPECT_EQ(ending(StartFailure::Structural, true, false), Ending::Decline);
    EXPECT_EQ(ending(StartFailure::Transient, true, false), Ending::Restart);
    EXPECT_EQ(ending(StartFailure::Transient, false, false), Ending::Decline) << "nothing starts the last start again";
    EXPECT_EQ(ending(StartFailure::Transient, true, true), Ending::Decline) << "the kernel stopped waiting: no loop";
    EXPECT_EQ(ending(StartFailure::AnotherServes, true, false), Ending::Leave);
    EXPECT_EQ(ending(StartFailure::AnotherServes, false, true), Ending::Leave);
}

TEST(CompilerRestart, TheKernelsAnswersAreSortedByWhetherAnotherStartMeetsThemAgain)
{
    EXPECT_EQ(registrationFailure(kIOReturnExclusiveAccess), StartFailure::AnotherServes);
    for (const int structural : {kIOReturnNotPrivileged, kIOReturnBadArgument, kIOReturnUnsupported})
        EXPECT_EQ(registrationFailure(structural), StartFailure::Structural) << std::hex << structural;
    for (const int transient : {kIOReturnVMError, kIOReturnNoMemory, kIOReturnError})
        EXPECT_EQ(registrationFailure(transient), StartFailure::Transient) << std::hex << transient;
    for (const int structural : {kIOReturnNotPrivileged, kIOReturnNotPermitted, kIOReturnUnsupported})
        EXPECT_EQ(connectFailure(structural), StartFailure::Structural) << std::hex << structural;
    for (const int transient : {kIOReturnNoMemory, kIOReturnNoResources, int(KERN_FAILURE), kIOReturnNotReady})
        EXPECT_EQ(connectFailure(transient), StartFailure::Transient) << std::hex << transient;
}

TEST(CompilerRestart, TheServiceDeclinesOnlyWhereTheRulesSay)
{
    const std::string service = SourceText("../compiler/compiler_service.mm");
    const std::string open = Body(service, "void Service::open(uint64_t stack)");
    EXPECT_THAT(open, HasSubstr("if (!connect(why, failure))\n        notServing(why, failure);"));
    EXPECT_THAT(open, HasSubstr("notServing(why, StartFailure::Structural);"));
    EXPECT_THAT(open, HasSubstr("notServing(why, StartFailure::Transient);"));
    EXPECT_THAT(open, HasSubstr("if (!registerStore(stack, why, &failure))\n        notServing(why, failure);"));
    EXPECT_THAT(open, Not(HasSubstr("decline()")));
    const std::string ends = Body(service, "[[noreturn]] void Service::notServing(");
    EXPECT_THAT(ends, HasSubstr("ending(failure, restarts() != 0, kernelGaveUp())"));
    EXPECT_THAT(ends, HasSubstr("if (end == Ending::Decline)\n        decline();"));
    EXPECT_THAT(ends, HasSubstr("exit(end == Ending::Restart ? 1 : 0);"));
    EXPECT_THAT(service, Not(HasSubstr("compiles its states itself")));
}

// A context that did not open fails its compile alone: no count, no latch, the next compile opens afresh
TEST(CompilerRestart, EachCompileOpensAContextThatDidNotOpenAfresh)
{
    const std::string engine = SourceText("../compiler/compiler_engine.mm");
    const std::string open = Body(engine, "bool Engine::openCompiler(std::string &why)");
    EXPECT_THAT(open, HasSubstr("if (s.direct)\n        return true;"));
    EXPECT_THAT(open, HasSubstr("native_bridge_metal_compile_open(s.inputs.nvk.c_str())"));
    EXPECT_THAT(open, Not(HasSubstr("opens")));
    EXPECT_THAT(open, Not(HasSubstr("openRefusal")));
    const std::string compile = Body(engine, "int32_t Engine::compile(const Prepared &p,");
    EXPECT_THAT(compile, HasSubstr("if (!openCompiler(why))\n        return TegraGM20BCompileFailed;"));
}

// An exit for a fresh compiler would register again, zero the kernel's losses and could loop for the boot
TEST(CompilerRestart, ACompilerWhoseOpensAreSpentKeepsServingItsStore)
{
    const std::string run = Body(SourceText("../compiler/compiler_service.mm"), "int32_t Service::run(Work &work)");
    EXPECT_THAT(run, HasSubstr("engine_.compile(p, entry, why)"));
    EXPECT_THAT(run, Not(HasSubstr("exit(")));
}

}  // namespace
