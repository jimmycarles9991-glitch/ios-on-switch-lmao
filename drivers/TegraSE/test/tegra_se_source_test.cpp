// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the device run showed and a host model cannot: over the AHB both engines ran at about 120 MB/s, a tenth of the
// CPU, and boot sent XNU's large hashes to them anyway. SE1 moves data through its own memory-controller clients, work
// reaches the engines from the bulk size down to whatever a timing run measures no slower, and an engine is proven
// (its failures panic) once bring-up has run every kind of operation on it into the driver's own buffers.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraSESource, EveryOperationTakesItsEnginesMemoryInterface)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_EQ(Count(source, "CRYPTO_CONFIG,"), 1u);
    EXPECT_THAT(Body(source, "void crypto(const Engine &e, uint32_t config)\n"),
                HasSubstr("wr(e, CRYPTO_CONFIG, config | kMemif[&e - gEngines]);"));
    EXPECT_THAT(source, HasSubstr("constexpr uint32_t kMemif[2] = {kCryptoMccif, 0};"));
}

TEST(TegraSESource, BulkWorkGoesToTheEnginesUntilATimingRunMeasuresLower)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_THAT(source, HasSubstr("constexpr size_t kBulk = 2u << 20;"));
    EXPECT_THAT(source, HasSubstr("volatile size_t gCutover[ALGS] = {kBulk, kBulk, kBulk, kBulk, kBulk, kBulk, kBulk, "
                                  "kBulk, kBulk, kBulk};"));
    const std::string timing = Body(source, "void timing()\n");
    EXPECT_THAT(timing, HasSubstr("gCutover[alg] = cutover[alg] && cutover[alg] < kBulk ? cutover[alg] : kBulk;"));
}

// Proven before gReady lets callers in, in every kernel: from then on an operation may be writing a caller's pages.
TEST(TegraSESource, AnEngineIsProvenBeforeCallersPagesReachIt)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_THAT(Body(source, "bool run(Engine &e, uint32_t in, uint32_t out)\n"), Not(HasSubstr("e.proven = true")));
    const std::string bringUp = Body(source, "void seThread(void *, wait_result_t)\n");
    const size_t proven = bringUp.find("    prove();\n    gReady = true;");
    ASSERT_NE(proven, std::string::npos);
    EXPECT_LT(bringUp.find("rng(*got, seed)"), proven);
    EXPECT_EQ(Count(source, "prove();"), 1u);
}

// An engine turned off with an operation in flight may still write the outputs it was given.
TEST(TegraSESource, CallersResultsAreNotDoneAgainOnTheCpu)
{
    const std::string se = SourceText("../TegraSE.cpp");
    EXPECT_THAT(se, Not(HasSubstr("shouldCheck"))) << "no sampled CPU compare, in any build";
    EXPECT_THAT(se, Not(HasSubstr("differs from the CPU's")));
    EXPECT_THAT(Body(se, "\nint compressHook("), HasSubstr("memcpy(s, next, static_cast<size_t>(words) * 4);\n"
                                                           "    return 0;"))
        << "the engine's state goes to the caller as it is";
}

TEST(TegraSESource, KnownAnswersWriteBuffersThatOutliveTheirEngine)
{
    const std::string source = SourceText("../TegraSE.cpp");
    const std::string answers = Body(source, "uint32_t knownAnswers(const Engine &only)\n");
    EXPECT_THAT(answers, Not(HasSubstr("uint8_t buf[64]")));
    EXPECT_THAT(answers, Not(HasSubstr("uint8_t out[64]")));
    EXPECT_THAT(answers, HasSubstr("gAnswers[&only - gEngines].out"));
}

// SE1's MCCIF addresses reach only held pages (TegraSESmmu.h): each operation's lists held before the start and released
// after it in one batched SMMU hold/release, on every path; the engine attached before its first DMA (the known answers).
TEST(TegraSESource, AnMccifOperationsPagesAreHeldOnlyWhileItRuns)
{
    const std::string source = SourceText("../TegraSE.cpp");
    const std::string run = Body(source, "bool run(Engine &e, uint32_t in, uint32_t out)\n");
    const size_t hold = run.find("tegra_se_smmu_hold_lists(which, inList, outList)");
    const size_t start = run.find("start(e, in, out, &started)");
    const size_t release = run.rfind("tegra_se_smmu_release_lists(which, inList, outList)");
    ASSERT_NE(hold, std::string::npos);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(release, std::string::npos);
    EXPECT_LT(hold, start);
    EXPECT_LT(start, release);
    const size_t engineOff = run.find("if (!e.up && started)\n        return ran;");
    ASSERT_NE(engineOff, std::string::npos) << "an engine turned off mid-operation keeps its holds";
    EXPECT_LT(start, engineOff);
    EXPECT_LT(engineOff, release);
    EXPECT_EQ(Count(source, "start(e, "), 1u) << "every operation starts through run()";
    EXPECT_THAT(source, HasSubstr("if (!engineUp(e) || !confine(e, which)) {"));
    EXPECT_THAT(source, Not(HasSubstr("kSmmuAsid")));
    EXPECT_THAT(Body(source, "Engine *acquire()\n"), Not(HasSubstr("IOLockSleepDeadline")));
    EXPECT_THAT(Body(source, "void forceOnly(const Engine *only)\n"), Not(HasSubstr("IOLockSleepDeadline")));
    EXPECT_THAT(Body(source, "extern \"C\" int tegra_se_aes("), Not(HasSubstr("IOMalloc")));
}

TEST(TegraSESource, DirectIrqUnlockedSmmuAndNoBootRamWaste)
{
    const std::string se = SourceText("../TegraSE.cpp");
    const std::string smmu = SourceText("../TegraSESmmu.cpp");
    EXPECT_THAT(se, Not(HasSubstr("IOWorkLoop")));
    EXPECT_THAT(se, Not(HasSubstr("IOFilterInterruptEventSource")));
    EXPECT_THAT(se, Not(HasSubstr("__ATOMIC_SEQ_CST")));
    EXPECT_THAT(se, Not(HasSubstr("__sync_bool_compare_and_swap")));
    EXPECT_THAT(Body(se, "bool drain(Engine &e)\n"), HasSubstr("IOSleep(1)"));
    EXPECT_THAT(Body(se, "bool engineUp(Engine &e)\n"), Not(HasSubstr("kBounceBytes")));
    EXPECT_THAT(Body(se, "void seThread(void *, wait_result_t)\n"), Not(HasSubstr("switch_digest_compress_hook")));
    EXPECT_THAT(smmu, Not(HasSubstr("reserveTables")));
    EXPECT_THAT(smmu, Not(HasSubstr(".locked(")));
    EXPECT_THAT(smmu, HasSubstr(".unlocked("));
}
