import HWTestCommon
import HWTestCore
import HWTestKit

/// Metal calls the facade answers itself where they were answered zero before: each runs on the driver, or its
/// feature answers NO wherever Metal lets a caller ask.
enum NativeCallTests {
    static let all: [any HWTest] = [
        NativeUnsupportedTest(), NativeVertexSamplerTest(), NativeParallelEncoderTest(), NativeIndirectCommandsTest(),
        NativeComputeCommandsTest(), NativeSharedTextureTest(), NativeSharedTexturePeerTest(), NativeCountersTest(),
        NativeCounterObjectsTest(), NativeTessellationTest(), NativeAmplificationTest(), NativeAmplificationCountTest(),
        NativeAmplificationFetchTest(), NativeAmplificationMappingsTest(), NativeDynamicStrideTest(),
        NativeStageInTest(), NativeVertexStreamsTest(), NativeWideStrideTest(), NativeTargetPartsTest(),
        NativeLayeredClearTest(),
        NativeLayeredMemorylessTest(), NativeLayeredVertexTest(), NativeLayeredFetchTest(), NativeViewportsTest(),
        NativeViewportIndexTest(), NativeBlockTexturesTest(), NativeDepthSlicesTest(), NativeWideTargetsTest(),
        NativeInterpolantsTest(), NativeThreadgroupMemoryTest(), NativeArgumentTier2Test(), NativeErrorBufferCopyTest(),
        NativeErrorIndexRangeTest(), NativeErrorColourFormatTest(), NativeErrorFormatSkipTest(),
        NativeErrorSamplesTest(),
        NativeErrorPositionsTest(), NativeLinearRowTest(), NativeHeapBlockTest(),
        NativeDepthCpuTest(), NativeMemorylessCpuTest(),
        NativeRateMapAbsentTest(),
    ]
}
