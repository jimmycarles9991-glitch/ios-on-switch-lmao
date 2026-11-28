// SPDX-License-Identifier: MIT
// A render state the shader compiler made gets what a pack build gives a state of its own, its fragment program's split and
// its program variants: its full form (RequestForm::Full) asked once its primary record is placed, and attached to its
// pipeline while it draws (Device::attachFullForm), or at its creation where the store's primary entry already is that
// full record. A full form of another layout than the placed record attaches nothing, with an error line.
#include "facade_full_form.h"
#include "bazel/testing/fresh_process.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace {

using namespace harness;

// Draws of `state` while its full form may be landing, each committed and waited for, then one more once it has (or 5 s
// passed): RenderEncoder::prepare reads the pipeline's variants and split as the attach writes them. Beside them a thread
// that takes no lock reads them as a draw does, so nothing but their publication orders its reads after the attach's
// writes. Whether every draw completed.
bool drawnUntilFull(id<MTLDevice> device, id<MTLRenderPipelineState> state, gm20b::Pipeline *pl)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::atomic<bool> landed{false};
    std::thread reader([&] {
        gm20b::Device &dev = *gRM->raw;
        while (std::chrono::steady_clock::now() < until) {
            gm20b::Pipeline *chosen = dev.variant(*pl, gm20b::AssumeUniformAligned);
            const gm20b::SplitSource *source = splitOf(pl);
            if (chosen != pl && chosen->rec == pl->rec && source && source->pp == pl->rec)
                return landed.store(true, std::memory_order_release);
            std::this_thread::yield();
        }
    });
    bool completed = true;
    bool last = false;
    do {
        last = landed.load(std::memory_order_acquire);
        @autoreleasepool {
            completed =
                draw(device, state, MTLPixelFormatBGRA8Unorm).status == MTLCommandBufferStatusCompleted && completed;
        }
    } while (!last && std::chrono::steady_clock::now() < until);
    reader.join();
    return completed && landed.load(std::memory_order_acquire);
}

void aFullFormAfterPlacementGivesItsVariantsAndSplit()
{
    gm20b::RuntimeRecord full;
    RenderCompiler compiler;
    compiler.shapes = [&full](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        residualUniform(record);
        if (request.form != gm20b::RequestForm::Full)
            return primaryForm(record);
        addFullForm(record);
        full = record;
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = madeFull(compiler, "late-full", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    EXPECT_TRUE(drawnUntilFull(device, state, pl));
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler),
              (std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary, gm20b::RequestForm::Full}));
    const gm20b::VariantTable *table = tableOf(pl);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->chosen.size(), 2u);
    gm20b::Device &dev = *gRM->raw;
    gm20b::Pipeline *aligned = dev.variant(*pl, gm20b::AssumeUniformAligned);
    EXPECT_NE(aligned, pl);
    EXPECT_EQ(aligned, table->chosen[0]);
    EXPECT_EQ(aligned->rec, pl->rec);
    EXPECT_EQ(dev.variant(*pl, 0), pl);
    const gm20b::SplitSource *source = splitOf(pl);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->pp, pl->rec);
    // a second full form of the state gives it nothing again: a draw may hold the split and variants it loaded
    full.pipeline.keyHash = pl->rec->keyHash;
    EXPECT_TRUE(dev.attachFullForm(full, why)) << why;
    EXPECT_EQ(splitOf(pl), source);
    EXPECT_EQ(tableOf(pl), table);
}

void aStoredFullRecordGivesBothAtCreation()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &, gm20b::RuntimeRecord &record) {
        residualUniform(record);
        addFullForm(record);
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = madeFull(compiler, "stored-full", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_NE(tableOf(pl), nullptr);
    EXPECT_NE(gRM->raw->variant(*pl, gm20b::AssumeUniformAligned), pl);
    EXPECT_NE(splitOf(pl), nullptr);
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(2) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary});
}

void aFullFormOfAnotherLayoutAttachesNothing()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        residualUniform(record);
        if (request.form != gm20b::RequestForm::Full)
            return primaryForm(record);
        addFullForm(record);
        record.blob = {0x1a00, 0x2222};
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    testing::internal::CaptureStderr();
    gm20b::Pipeline *pl = madeFull(compiler, "other-layout", device, state, why);
    const bool settled = pl && compiler.wait(2) && gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC);
    const std::string lines = testing::internal::GetCapturedStderr();
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(settled);
    EXPECT_EQ(forms(compiler),
              (std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary, gm20b::RequestForm::Full}));
    EXPECT_EQ(tableOf(pl), nullptr);
    EXPECT_EQ(splitOf(pl), nullptr);
    EXPECT_NE(lines.find("SwitchMetal: a full form of state "), std::string::npos) << lines;
    EXPECT_NE(lines.find(" is not attached: it is of another register list than its placed record"), std::string::npos)
        << lines;
}

// A device-made state's turned draws take its turned variant (TurnedSource), never a program variant made unturned
void aTurnedDrawOfADeviceMadeStateKeepsItsOwnPipeline()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &, gm20b::RuntimeRecord &record) {
        residualUniform(record);
        addFullForm(record);
        gm20b::CapturedProgram turned = record.programs[1];
        turned.image[86] = 1;
        turned.id = "fs-turned";
        record.variants.insert(record.variants.begin() + record.pipeline.variantCount[0],
                               {gm20b::AssumeTurned, turned});
        record.pipeline.variantCount[1]++;
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = madeFull(compiler, "turned", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    EXPECT_TRUE(pl->turnedVariant);
    gm20b::Device &dev = *gRM->raw;
    ASSERT_NE(dev.variant(*pl, gm20b::AssumeUniformAligned), pl);
    EXPECT_EQ(dev.variant(*pl, gm20b::AssumeUniformAligned | gm20b::AssumeTurned), pl);
}

// The forms the device asked of a state whose primary record `shapes` makes, once every background ask settled
std::vector<gm20b::RequestForm> askedOf(const char *name, std::function<void(gm20b::RuntimeRecord &)> shapes)
{
    RenderCompiler compiler;
    compiler.shapes = [shapes](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        if (request.form != gm20b::RequestForm::Full)
            shapes(record);
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    EXPECT_NE(madeFull(compiler, name, device, state, why), nullptr) << why;
    EXPECT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    return forms(compiler);
}

void aRecordWhoseSplitWasTriedAndOfNoResidualAsksNoFullForm()
{
    EXPECT_EQ(askedOf("tried", [](gm20b::RuntimeRecord &) {}),
              std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary});
}

void aRecordWhoseSplitWasNotTriedAsksItsFullForm()
{
    EXPECT_EQ(askedOf("untried", primaryForm),
              (std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary, gm20b::RequestForm::Full}));
}

void aRecordOfAResidualUniformAsksItsFullForm()
{
    EXPECT_EQ(askedOf("residual", residualUniform),
              (std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary, gm20b::RequestForm::Full}));
}

}  // namespace

TEST(GM20BFacadeMadeFull, ARecordWhoseSplitWasTriedAndOfNoResidualAsksNoFullForm)
{
    switch_ios::test::ExpectPassesInFreshProcess(aRecordWhoseSplitWasTriedAndOfNoResidualAsksNoFullForm);
}

TEST(GM20BFacadeMadeFull, ARecordWhoseSplitWasNotTriedAsksItsFullForm)
{
    switch_ios::test::ExpectPassesInFreshProcess(aRecordWhoseSplitWasNotTriedAsksItsFullForm);
}

TEST(GM20BFacadeMadeFull, ARecordOfAResidualUniformAsksItsFullForm)
{
    switch_ios::test::ExpectPassesInFreshProcess(aRecordOfAResidualUniformAsksItsFullForm);
}

TEST(GM20BFacadeMadeFull, AFullFormAfterPlacementGivesItsVariantsAndSplit)
{
    switch_ios::test::ExpectPassesInFreshProcess(aFullFormAfterPlacementGivesItsVariantsAndSplit);
}

TEST(GM20BFacadeMadeFull, AStoredFullRecordGivesBothAtCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(aStoredFullRecordGivesBothAtCreation);
}

TEST(GM20BFacadeMadeFull, AFullFormOfAnotherLayoutAttachesNothing)
{
    switch_ios::test::ExpectPassesInFreshProcess(aFullFormOfAnotherLayoutAttachesNothing);
}

TEST(GM20BFacadeMadeFull, ATurnedDrawOfADeviceMadeStateKeepsItsOwnPipeline)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTurnedDrawOfADeviceMadeStateKeepsItsOwnPipeline);
}
