// SPDX-License-Identifier: MIT
// pack/kernel_shapes.h over this Mac's Metal: a D line's stage-in segment, as the journal writes a state's stage input
// (StageInText.h), makes the descriptor it names and keys the kernel as the runtime keys it (TegraGM20BMetalComputeState.mm);
// one the reader cannot make a descriptor of names why.
#import <Metal/Metal.h>
#include <gtest/gtest.h>
#include <string>
#include "../pack/kernel_shapes.h"
#include "StageInText.h"

namespace {

const std::string kHead =
    "D ok=1 tg=8,1,1 edge=0,0,0 threads=0 | form=d opt=0 max=0 tgm=0 | k kernel_main lib=/a.metallib";

MTLStageInputOutputDescriptor *stageInput()
{
    MTLStageInputOutputDescriptor *in = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
    in.attributes[0].format = MTLAttributeFormatFloat2;
    in.attributes[0].bufferIndex = 1;
    in.layouts[1].stride = MTLBufferLayoutStrideDynamic;
    in.layouts[1].stepFunction = MTLStepFunctionThreadPositionInGridX;
    in.attributes[3].format = MTLAttributeFormatUChar4Normalized;
    in.attributes[3].offset = 8;
    in.attributes[3].bufferIndex = 2;
    in.layouts[2].stride = 12;
    in.layouts[2].stepFunction = MTLStepFunctionThreadPositionInGridXIndexed;
    in.indexType = MTLIndexTypeUInt16;
    in.indexBufferIndex = 4;
    return in;
}

TEST(KernelShapes, AStageInSegmentMakesTheDescriptorItNamesAndKeysTheKernel)
{
    const std::string segment = stageInText(stageInput());
    ASSERT_EQ(segment, " | in a0=29:0:1:4294967295:5:1 a3=9:8:2:12:7:1 i=0:4");
    probe::Shape s;
    std::string why;
    ASSERT_TRUE(probe::parse(kHead + segment + " | ", s, why)) << why;
    EXPECT_EQ(s.function, "kernel_main");
    EXPECT_EQ(s.stageIn, segment);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    ASSERT_NE(dev, nil);
    NSError *error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@"kernel void kernel_main() {}" options:nil error:&error];
    ASSERT_NE(library, nil);
    MTLComputePipelineDescriptor *d = probe::descriptor(s, [library newFunctionWithName:@"kernel_main"]);
    EXPECT_EQ(stageInText(d.stageInputDescriptor), segment);
    EXPECT_NE(probe::key(s, {}).find(segment), std::string::npos);
    probe::Shape none;
    ASSERT_TRUE(probe::parse(kHead + " | ", none, why)) << why;
    EXPECT_EQ(probe::key(none, {}).find(" | in "), std::string::npos);
}

// A frontend journal's line names no address map: a converted texture's item is keyed with its format's map, as the
// device keys the dispatch (the pack's record otherwise matches no device key); an item that states one keeps it
TEST(KernelShapes, ATextureItemThatStatesNoMapIsKeyedWithItsFormats)
{
    const std::string line = "D ok=1 tg=32,8,1 edge=0,0,0 threads=0 tex=0:500:2:1:1:02030405,1:70:2:1:1:02030405,"
                             "2:500:2:1:1:02030405:am3 | form=d opt=0 max=0 tgm=0 | k kernel_main lib=/a.metallib | ";
    probe::Shape s;
    std::string why;
    ASSERT_TRUE(probe::parse(line, s, why)) << why;
    probe::stateFormatMaps(s, [](uint64_t format) -> uint8_t {
        return format == 500 ? 1 : 0;
    });
    const std::string key = probe::key(s, {});
    EXPECT_NE(key.find("tex=0:500:2:1:1:02030405:am1,1:70:2:1:1:02030405,2:500:2:1:1:02030405:am3 |"),
              std::string::npos)
        << key;
}

TEST(KernelShapes, AStageInSegmentItCannotReadOrOfAFunctionsStateIsRefused)
{
    probe::Shape s;
    std::string why;
    EXPECT_FALSE(probe::parse(kHead + " | in a0=29:0 i=0:4 | ", s, why));
    EXPECT_EQ(why, "a stage input it cannot read");
    const std::string ofFunction =
        "D ok=1 tg=8,1,1 edge=0,0,0 threads=0 | form=f opt=0 | k kernel_main lib=/a.metallib";
    EXPECT_FALSE(probe::parse(ofFunction + stageInText(stageInput()) + " | ", s, why));
}

}  // namespace
