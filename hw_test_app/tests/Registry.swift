import HWTestCore
import HWTestGPU

public enum Registry {
    /// The GPU scenes come first: the first of them reproduces the process's first command buffer.
    static let gpuScenes: [any HWTest] =
        Scenes.all.map { GPUSceneTest(scene: $0) } + [CAStateSweepTest(), CAAskedFormsTest()]

    static let metal: [any HWTest] = [
        MetalDeviceTest(), MetalComputeTest(), MetalColorPassTest(), MetalDepthStencilTest(), MetalMSAATest(),
        MetalPixelFormatTest(), MetalHalfRoundingTest(), MetalCompressedTest(), MetalBlitTest(), MetalTextureViewTest(),
        MetalIOSurfaceTest(), SurfaceTiledDefaultTest(), YcbcrTargetTest(), YcbcrPackedTargetTest(),
        PackedPlaneTargetTest(), PackedSpareBitsTest(), XRTwoPlaneTest(),
        MPSBlurTest(), MPSMatrixTest(), MetalRuntimeCompileTest(), CoreImageLibraryTest(),
        MetalBenchmarkTest(), FetchBlendCostTest(), CompressionChurnTest(), RotatedTargetTest(), IntegerTargetTest(),
        BlendKillTest(), WrittenTextureTest(),
        Float32FilterTest(), CompareSamplerTest(), SurfaceBufferTest(), CompressedTargetsTest(), ZcullTest(),
        MipTwodTest(), MipCandidatesTest(),
        VisibilityTest(), FixedBlendRuleTest(), PrivateCompressionTest(), AlphaToCoverageTest(),
        CompileSharedTest(), CompileFirstDrawTest(), DrawBandsTest(), DispatchPiecesTest(), PosterTargetsTest(),
        ResolveCostTest(),
        MsaaFirstUseTest(), StoredDepthTest(),
        ResolveTwodTest(), FillTextureTest(), StorageCompressedTest(), PredicationTest(), MPSControlFlowTest(),
        EventReleaseTest(), CoherenceRewriteTest(),
        FetchRungTest(id: "gpu.raw.fetch_rung2", rung: 2), BlendTwinTest(), ZetaColourTest(),
    ]

    static let video: [any HWTest] =
        VideoDecodeTest.all + [
            NativeExportTest.h264, NativeExportTest.main10, NativeExportTest.main10To8, HEVCAlphaTest(),
            DecodeRouteTest(),
        ]
        + PlaybackTest.all + VideoEncodeTest.all + VideoEncodeIdleTest.all
        + [VideoEncodeBFramesTest(), NVENCInputLayoutTest(), VideoTranscodeTest()]

    static let images: [any HWTest] =
        ImageDecodeTest.all + [
            JPEGAfterIdleTest(), HEICEncodeTest(), CoreImageTest(), PixelTransferTest(), PixelTransfer422Test(),
        ]

    static let audio: [any HWTest] = [
        AudioPlaybackTest(rate: 44_100), AudioPlaybackTest(rate: 48_000), AudioLatencyTest(), AudioRouteTest(),
        HeadsetTest(), MicrophoneTest(),
    ]

    static let display: [any HWTest] =
        [
            DisplayModeTest(), DisplayPacingTest(), CompletionFlipTest(), MediaCompletionFlipTest(), BrightnessTest(),
            ColourPipelineTest(), PlanesTest(), SmallDrawableTest(), PresentAtTimeTest(), PlaneInUseTest(),
            ScanoutRoutesTest(), PlainSurfacesTest(), DisplaySnapshotTest(), WakeTest(),
        ] + DisplayWindowProbeTest.all + [LandscapeVideoTest()]

    static let sensorsAndInput: [any HWTest] = [
        AccelerometerTest(), GyroscopeTest(), DeviceMotionTest(), AmbientLightTest(), ControllersTest(), RailTest(),
        JoyConIdleTest(), JoyConProbeTest(), JoyConInputTest(), FocusIdleTest(), FocusSearchTest(),
        GameControllerTest(), KeyboardTest(), TouchTest(),
    ]

    static let connectivityAndStorage: [any HWTest] = [
        NetworkPathTest(), ReachabilityTest(), BluetoothTest(), USBStorageTest(), SequentialStorageTest(),
        RandomStorageTest(), WiFiLinkTest(),
    ]

    static let cpuAndSystem: [any HWTest] = [
        CPUSingleTest(), CPUMultiTest(), A64XSelfTest(), CryptoKitTest(), CommonCryptoTest(), KeychainTest(),
        LocalAuthenticationTest(), IdentityTest(), PowerStateTest(), ThermalTest(), MemoryTest(), UnionDirectoryTest(),
        OverlayAppsTest(), OverlayAppRecordsTest(), CPUClockTest(), CPUSustainedTest(), CPUSchedTest(),
        DispatchSyncTest(), DispatchSyncWorkloopTest(), WorkloopSyncTest(), WidgetsRenderTest(), IconRenderTest(),
    ]

    static let compositor: [any HWTest] = UISceneTests.all + [VideoAlphaTest()] + PacingTests.all

    public static let all: [any HWTest] =
        [CPUAsyncTimersTest(), RegDumpQuietTest()] + gpuScenes + metal + GPUPathTests.all + NativeObjectTests.all
        + NativeCallTests.all + video + images + audio
        + display + compositor + sensorsAndInput + connectivityAndStorage
        + cpuAndSystem + [RawShareTest()]
        // Drives an NVDEC output mode nothing else uses: after every shared test.
        + [DisplayBufferTest()]
        // The shader compiler's generation covers every test before it.
        + [CompilerAliveTest()]
        // May stop NVJPG's host1x channel for 50 ms: last.
        + [DcHost1xClassTest()]
        // Leaves this app for Settings and opens it again: last, so no other test runs while it is away.
        + [LaunchHitchTest()]

    /// Programs a unit no other driver here drives, which kills the channel when its setup is wrong: each app's last.
    public static let last: [any HWTest] = [FetchRungTest(id: "gpu.raw.fetch_rung3", rung: 3)]
    /// Reset the copy engine under the present, then kill the app's raw channel: each app runs them after all its other
    /// tests, so a failed recovery spoils none of them.
    public static let recovery: [any HWTest] = [
        PresentCopyResetTest(), RecoverKillOneTest(), RecoverKillUnderCompositorTest(), RecoverBusyEngineTest(),
        RecoverKernelStallTest(),
    ]
}
