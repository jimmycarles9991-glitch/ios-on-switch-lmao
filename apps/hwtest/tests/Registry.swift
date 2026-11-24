import HWTestCore
import HWTestGPU

public enum Registry {
    /// The GPU scenes come first: the first of them reproduces the process's first command buffer.
    static let gpuScenes: [any HWTest] =
        Scenes.all.map { GPUSceneTest(scene: $0) } + [PackHitCostTest(), CAStateSweepTest(), CAAskedFormsTest()]

    static let metal: [any HWTest] = [
        MetalDeviceTest(), MetalComputeTest(), MetalColorPassTest(), MetalDepthStencilTest(), MetalMSAATest(),
        MetalPixelFormatTest(), MetalHalfRoundingTest(), MetalCompressedTest(), MetalBlitTest(), MetalTextureViewTest(),
        MetalIOSurfaceTest(), SurfaceTiledDefaultTest(), SurfaceCrossProcessTest(), SurfaceSnapshotLayoutTest(),
        PeerUserClientTest(), GM20BReadbackRoutesTest(),
        YcbcrTargetTest(),
        YcbcrPackedTargetTest(),
        PackedPlaneTargetTest(), PackedSpareBitsTest(), XRTwoPlaneTest(),
        MPSBlurTest(), MPSMatrixTest(), MetalRuntimeCompileTest(), CoreImageLibraryTest(),
        MetalBenchmarkTest(), FetchBlendCostTest(), CompressionChurnTest(), RotatedTargetTest(), IntegerTargetTest(),
        BlendKillTest(), WrittenTextureTest(),
        Float32FilterTest(), CompareSamplerTest(), SurfaceBufferTest(), OwnAnswersTest(), ComputeFactsTest(),
        SourceLibraryTest(), StitchedLibraryTest(), SamplerTableTest(), FunctionTagsTest(),
        FunctionArgumentEncoderTest(), ArgumentGridsTest(), PlainStructEncoderTest(), BufferTexture16Test(),
        PlaneRows16Test(), CompressedTargetsTest(), ZcullTest(), MipTwodTest(), MipCandidatesTest(), MipLodTest(),
        MipmapVolumeTest(), ShadowStencilTest(), ShadowStencilDepthTest(),
        VisibilityTest(), FixedBlendRuleTest(), FixedBlendFormatsTest(), PrivateCompressionTest(),
        PackSlotCleanTest(), ZoomTargetTest(),
        AlphaToCoverageTest(),
        IconCornersTest(), TextureBufferTest(), IconSmallSizesTest(), IconPasscodeTest(), IconCalendarTest(),
        CompileSharedTest(), CompileFirstDrawTest(), CompileStageLibraryTest(), CompileAnswerLatencyTest(),
        CompileSharedStagesTest(),
        DrawBandsTest(), DispatchPiecesTest(), ProbeCtaTest(), ProbeMacroWfiTest(), ForeignWaitIsolationTest(),
        PosterTargetsTest(),
        PushScanTargetsTest(), ResolveCostTest(), JobRefusalLinesTest(),
        MsaaFirstUseTest(), StoredDepthTest(), ZetaMoveTest(), ZetaKeptTest(), ZcViewFormatsTest(),
        ZcZetaLayersTest(), ZcZetaShadowTest(), ZcMixedTest(), ZcStencilCriterionTest(),
        ResolveTwodTest(), FillTextureTest(), StorageCompressedTest(), SharedLayoutTest(), PredicationTest(),
        MPSControlFlowTest(),
        EventReleaseTest(), CoherenceRewriteTest(),
        FetchRungTest(id: "gpu.gm20b.fetch_rung2", rung: 2), BlendTwinTest(), ZetaColourTest(), PlanarConstexprTest(),
        TessellationRootTableTest(),
        StripOverCompressedTest(), OpaqueBorderTest(), IndirectDispatchTest(), TileRefusedTest(),
        SeparateDepthStencilTest(), PixelMirrorSamplerTest(), DeferredZetaStoreTest(),
        NanClearDepthTest(), Depth16ClearTest(), SeparateDepth16StencilTest(),
        SamplerUnsupportedTest(), SeparateZetaLayeredTest(), VisibilityAccumulateTest(),
        DepthBiasUnitsTest(), SeparateZetaSliceTest(), TextureViewsTest(), MipmapLayersTest(), DepthArraySampleTest(),
        SeparateZetaResolveTest(), SeparateDepth16StencilMultisampleTest(), PackedPlaneLoadTest(),
        WrittenXrTextureTest(), WrittenShadowedSurfaceTest(), WrittenXrPlanesTest(), YcbcrCornerTest(),
        YcbcrOddSizeTest(), TurnedBoundTest(), TurnedOffSurfaceTest(), NonFiniteDepthBiasTest(),
        UnalignedBufferOffsetsTest(), VertexCubicSamplerTest(), YcbcrLoadTest(), TurnedUnalignedPlanesTest(),
        YcbcrP3MatrixTest(), YcbcrPastRowsTest(), YcbcrOnePlaneTest(), DrawErrorsTest(), DepthClipBoundsTest(),
        TessellationArgumentTextureTest(), AmplificationWideOffsetTest(), TextureShapesTest(), FormatTargetTest(),
        YcbcrBlendTargetTest(), TurnedSeparatePairTest(), IndirectIndexEndTest(), PatchCommandsWideSetTest(),
        LayeredShaderTestsTest(), LayeredShaderStencilTest(), WideSetTest(), FarUniformTest(), WideTexturesTest(),
        TextureShapesXRTest(),
        TextureShapesDepthTest(), TextureShapesSrgbPairTest(), DepthShapesTest(),
        VertexOnlyStateTest(), RenderReflectionTest(), ComputeReflectionTest(), ReflectionKindsTest(),
        FunctionPointerAnswersTest(), ScratchOrderTest(), FencePageRegionsTest(), Depth16MultisampleResolveTest(),
        InvalidRequestsTest(), HeapAliasableUsedTest(), CompressionSettledTest(), TextureTransferFailedTest(),
        HeapAliasedKindsTest(),
        X1ViewportsTest(), X1ThreadsPerThreadgroupTest(), X1Mme2Test(),
        DecodePvrtcTest(), DecodeAstcNarrowTest(), DecodeAstcHdrTest(), DecodeNativeAstcPrecisionTest(),
        DecodeFreshTest(), DecodeReadbackTest(), DecodeViewsTest(), DecodeViewLifetimeTest(), DecodeUploadCostTest(),
    ]

    /// Routes the m2v removal moved onto the driver, each against Apple's GPU
    static let gm20bRoutes: [any HWTest] =
        FetchShapeTest.all + [
            DepthFormatMismatchTest.attached, DepthFormatMismatchTest.unattached, HiddenDepthTest(),
            BorderTableFullTest(), XrSrgbTest.bgr, XrSrgbTest.bgra, ComputeInvalidTest(), FillShapesTest(),
            WordMipmapsTest(), StencilByteCopiesTest(), KernelYcbcrOnePlaneTest(), KernelConvertedWriteTest(),
            DynamicStridesTest(), ReachShapesTest(), ResolveCodesTest(), ResolveOneSampleSwapTest(),
            SeparateStencilResolveTest(), P3YcbcrStoreTest(), Depth16ShaderTestsTest(), PlanesBlitsTest(),
            IcbComputeFormsTest(), StandInResolveTest(), StandInLayeredTest(), ResolveOtherCountTest(),
            ResolveCodesPartsTest(), RtalPastLayersTest(), XrPlanesImagesTest(), Bgra10XrResolveTest(),
            ViewSliceTargetTest(), NoRenderUsageTest(), P3YcbcrStoreSubsampledTest(), PlaneInBlocksTest(),
            Rg8SrgbTargetTest(), TextureBufferTargetTest(), ZetaApartTest(), ZetaResolvePartsTest(),
            YcbcrNoRenderUsageTest(), MixedSamplesTest(), MixedEndRowsTest(),
            SameBitsTest(), Depth16LayeredTestsTest(), LayersPastTest(), LayersPastDepthTest(), TexelBufferReachTest(),
            FetchTessellatedTest(), PlanarRenderThenSampleTest(), XrTexelBufferStoresTest(),
            XrTexelBufferSurfaceTest(), TurnedStandInTargetsTest(), Bgra10XrSrgbEmptyPassTest(), StorageFormsTest(),
            XrBlendRoutesTest(), XrStandInTest(),
            PlanarSampledTargetTest(),
            YcbcrTargetShapesTest(),
            XrTwoPlanesAnyElementsTest(),
            XrImagesManyTest(),
            SrgbEncodeTest(),
            ExactRoundTest(),
            X1GpuVicSyncTest(),
            FrameWeightsTest(),
            YcbcrOnePlaneTargetsTest(),
            YcbcrRowsPastPageTest(),
            SM53LayoutTest(),
            FloatTestsTest(),
            YcbcrOverlapRowsTest(), YcbcrOverlapWordsTest(), XrCopyClampTest(),
            XrPlanesRowsOff32Test(), XrWrittenOnePlaneTest(),
            XrNextPlaneTest(), YcbcrAnySurfaceTest(), XrPlanesOff16Test(), XrAlphaSinkTest(),
            XrTiledSurfaceTest(), YcbcrRowsUnder16Test(),
            XrTexelBufferWrittenTest(), YcbcrTurnedLoadTest(), XrWordsIntoAlphaTest(), XrIntoAlphaTieTest(),
            XrAlonePastLastPageTest(),
            PlanesPlaneOneRowsTest(), PlanesAcrossFormatsTest(), XrRowsUnder16Test(), YcbcrRowsUnder16StoresTest(),
            PlanesCopiesOpenTest(), XrTexelBufferTiledTest(), PackedMipsTest(), XrTurnedRowsUnder16Test(),
            XrOverlapRowsTest(), PackedPlaneAnySurfaceTest(), PlanarTurnedTest(), YcbcrTurnedOverlapOrderTest(),
            YcbcrPlanesMeetTest(),
            YcbcrCpuCopiesTest(), YcbcrTexelBuffersTest(), YcbcrTexelBufferWideTest(), PackedLevelsTest(),
            PackedLevelsOpenTest(),
            XrDescriptorCopiesTest(), XrDescriptorPaddingTest(), TexelRowsTest(), XrStandInRefillTest(),
            TurnedOverlapTileOrderTest(), XrTurnedClearOrderTest(),
            PlainRowsReadsTest(), PlainRowsStoreOrderTest(), PlainRowsLoadedTest(), PlainRowsCpuTest(),
            PlainRowsUnder16Test(), PlainRowsBlitTilesTest(), PlainRowsQuarterTurnsTest(), MsBufferCopyTest(),
            IntegerMipmapsTest(),
        ]

    static let video: [any HWTest] =
        VideoDecodeTest.all + [
            NativeExportTest.h264, NativeExportTest.main10, NativeExportTest.main10To8, HEVCAlphaTest(),
            DecodeRouteTest(),
        ]
        + PlaybackTest.all + VideoEncodeTest.all + VideoEncodeIdleTest.all
        + [VideoEncodeBFramesTest(), NVENCInputLayoutTest(), VideoTranscodeTest(), VideoGPUSourceTest()]

    static let images: [any HWTest] =
        ImageDecodeTest.all + [
            JPEGAfterIdleTest(), JPEGMappingLifetimeTest(), HEICEncodeTest(), CoreImageTest(), PixelTransferTest(),
            PixelTransfer422Test(),
            PixelTransferRouteTest(),
            VICLumaTest(),
            VICGamutTest(), VICTagsTest(),
        ]

    static let audio: [any HWTest] = [
        AudioPlaybackTest(rate: 44_100), AudioPlaybackTest(rate: 48_000), AudioLatencyTest(), AudioRouteTest(),
        HeadsetTest(), MicrophoneTest(),
    ]

    static let display: [any HWTest] =
        [
            DisplayModeTest(), DisplayPacingTest(), CompletionFlipTest(), MediaCompletionFlipTest(),
            MediaCompletionFlipGatedTest(), ScanReturnedTest(), BrightnessTest(),
            ColourPipelineTest(), PlanesTest(), SmallDrawableTest(), PresentAtTimeTest(), DisplayRefreshTest(),
            PlaneInUseTest(),
            ScanoutRoutesTest(), PlainSurfacesTest(), DisplaySnapshotTest(), WakeTest(),
            PlanesCroppedTest(), PlaneTagsTest(),
        ] + DisplayWindowProbeTest.all + [LandscapeVideoTest(), LandscapeDownscaleTest()]
        // Holds the swaps for a second each; the forced producer failure last.
        + [Host1xFlipTest.trial, Host1xFlipTest.teardown]

    static let sensorsAndInput: [any HWTest] = [
        AccelerometerTest(), GyroscopeTest(), DeviceMotionTest(), AmbientLightTest(), ControllersTest(), RailTest(),
        JoyConIdleTest(), JoyConProbeTest(), JoyConInputTest(), FocusIdleTest(), FocusSearchTest(),
        GameControllerTest(), KeyboardTest(), TouchTest(),
    ]

    static let connectivityAndStorage: [any HWTest] = [
        NetworkPathTest(), ReachabilityTest(), BluetoothTest(), USBStorageTest(), SequentialStorageTest(),
        RandomStorageTest(), StorageIOTest(), WiFiLinkTest(),
    ]

    static let cpuAndSystem: [any HWTest] = [
        CPUSingleTest(), CPUMultiTest(), A64XSelfTest(), CryptoKitTest(), CommonCryptoTest(), KeychainTest(),
        CPUAtomicsTest(),
        LocalAuthenticationTest(), IdentityTest(), PowerStateTest(), ThermalTest(), SocThermThrottleTest(),
        MemoryTest(), UnionDirectoryTest(), OverlayAppsTest(), OverlayAppRecordsTest(), IconCacheTest(),
        IconCacheResetTest(), CPUClockTest(), CPUDfllLandingTest(), CPUSustainedTest(), CPUSchedTest(),
        CPUIPIPingPongTest(), HogPacingTest(),
        DispatchSyncTest(), DispatchSyncWorkloopTest(), WorkloopSyncTest(), WidgetsRenderTest(), IconRenderTest(),
        DispatchSyncHandoffTest(),
        SnapshotPurgeableTest(), RecorderHangDumpTest(),
    ]

    static let compositor: [any HWTest] =
        UISceneTests.all + [VideoAlphaTest()] + PacingTests.all
            + [ScalePacingTest(), TitleScrollTest(), AnimTimingTest()]

    public static let all: [any HWTest] =
        [CPUAsyncTimersTest(), RegDumpQuietTest()] + gpuScenes + metal + gm20bRoutes + GPUPathTests.all
            + [FrameRecordCostTest()]
            + NativeObjectTests.all
            + NativeCallTests.all + video + images + audio
            + display + compositor + sensorsAndInput + connectivityAndStorage
            + cpuAndSystem
            // Drives an NVDEC output mode nothing else uses: after every shared test.
            + [DisplayBufferTest()]
            // The shader compiler's generation covers every test before it.
            + [CompilerAliveTest()]
            // May stop the display's host1x channel for 50 ms: last.
            + [DcHost1xClassTest()]
            // Every client channel's RAMFC timeslice once the shared tests have run jobs on them.
            + [RamfcTimesliceTest(), RamfcCreationTest()]
            // Rail-gates the GPU under every other client four times: after the shared tests.
            + [GpuRailCycleTest()]
            // Leaves this app for Settings and opens it again: last, so no other test runs while it is away.
            + [LaunchHitchTest()]

    /// A 1.6 GB cube first, where the process has the memory; then fills the GPU pool to its compressed floor and gives
    /// it back (gpu.gm20b.compression_giveback), then takes it down to that floor again, which refuses every other
    /// process's compressed asks while each runs; then fills the GPU's ZBC colour table for the rest of the boot
    /// (gpu.gm20b.zbc_full: no test after it counts ZBC hits); then programs a unit no other driver here drives (across
    /// pass boundaries, then each program forced), which kills the channel when its setup is wrong: each app's last.
    public static let last: [any HWTest] = [
        X1TextureExtentsTest(), CompressionGivebackTest(), CompressedFloorTest(), ZbcFullTest(),
        IteratedBlendPassesTest(), FetchRungTest(
            id: "gpu.gm20b.fetch_rung3",
            rung: 3
        ),
    ]
    /// Reset the copy engine under the present, then kill the app's raw channel: each app runs them after all its other
    /// tests, so a failed recovery spoils none of them.
    public static let recovery: [any HWTest] = [
        PresentCopyResetTest(), RecoverKillOneTest(), RecoverKillUnderCompositorTest(), RecoverBusyEngineTest(),
        RecoverKernelStallTest(),
        UncheckedBufferLoadsTest(), KernelSparseMapTest(), ChannelEpochTest(), EndedEncoderTest(), AheadFailuresTest(),
    ]
}
