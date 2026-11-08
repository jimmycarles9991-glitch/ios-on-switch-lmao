import HWTestCore
import HWTestGPU

enum Registry {
    /// The GPU scenes come first: the first of them reproduces the process's first command buffer.
    static let gpuScenes: [any HWTest] = Scenes.all.map { GPUSceneTest(scene: $0) }

    static let metal: [any HWTest] = [
        MetalDeviceTest(), MetalComputeTest(), MetalColorPassTest(), MetalDepthStencilTest(), MetalMSAATest(),
        MetalPixelFormatTest(), MetalHalfRoundingTest(), MetalCompressedTest(), MetalBlitTest(), MetalTextureViewTest(),
        MetalIOSurfaceTest(), MPSBlurTest(), MPSMatrixTest(), MetalRuntimeCompileTest(), MetalBenchmarkTest(),
    ]

    static let video: [any HWTest] = VideoDecodeTest.all + PlaybackTest.all + VideoEncodeTest.all

    static let images: [any HWTest] = ImageDecodeTest.all + [HEICEncodeTest(), CoreImageTest(), PixelTransferTest()]

    static let audio: [any HWTest] = [
        AudioPlaybackTest(rate: 44_100), AudioPlaybackTest(rate: 48_000), AudioLatencyTest(), AudioRouteTest(),
        HeadsetTest(), MicrophoneTest(),
    ]

    static let display: [any HWTest] = [
        DisplayModeTest(), DisplayPacingTest(), BrightnessTest(), ColourPipelineTest(), WakeTest(),
    ]

    static let sensorsAndInput: [any HWTest] = [
        AccelerometerTest(), GyroscopeTest(), DeviceMotionTest(), AmbientLightTest(), GameControllerTest(),
        KeyboardTest(), TouchTest(),
    ]

    static let connectivityAndStorage: [any HWTest] = [
        NetworkPathTest(), ReachabilityTest(), BluetoothTest(), USBStorageTest(), SequentialStorageTest(),
        RandomStorageTest(),
    ]

    static let cpuAndSystem: [any HWTest] = [
        CPUSingleTest(), CPUMultiTest(), CryptoKitTest(), CommonCryptoTest(), KeychainTest(), LocalAuthenticationTest(),
        IdentityTest(), PowerStateTest(),
    ]

    static let compositor: [any HWTest] = UISceneTests.all + PacingTests.all

    static let all: [any HWTest] =
        gpuScenes + metal + video + images + audio + display + compositor + sensorsAndInput + connectivityAndStorage
        + cpuAndSystem
}
