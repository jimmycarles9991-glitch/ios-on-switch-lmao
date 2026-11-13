import Foundation

/// Every GPU scene in the order the app runs them: the first Core Animation surface before anything else touches the
/// GPU, since it is the process's first command buffer that it reproduces.
public enum Scenes {
    public static let firstSurface: any GPUScene = FirstTiledSurfaceScene()

    public static let all: [any GPUScene] = [
        firstSurface, CABlurChainScene(), CAVariableBlurChainScene(), CADrawThenCopyScene(), CAHotStatesScene(),
        CAGenericStateScene(), FramebufferFetchScene(), FramebufferFetchDepthScene(), FetchBlendOrderScene(),
        PassFusionScene(),
        GlyphAtlasScene(), ReplaceRegionScene(), BlitUploadScene(), SurfaceLockScene(), ClearFormatsScene(),
        ClearThenDrawScene(), InstancedDrawScene(), DepthBiasScene(), RenderThenSampleScene(), UnormBlendScene(),
        ExtendedBlendScene(), MipmapScene(), MSAAResolveScene(), MPSBlurScene(), CoreImageBlurScene(),
        HalfArithmeticScene(), SM53ProbeScene(), TexsF16Scene(), XRBlendScene(), TextureKindsScene(),
        AstcInPlaceCopyScene(), AstcLayoutScene(),
        LatencyScene(),
        LinkedFunctionScene(),
        StorageOffsetScene(),
        PixelSamplerScene(),
        TraceIdScene(),
        CubicSamplerScene(),
        KernelOffsetScene(),
        MemorylessBurstScene(),
        KernelDirectScene(),
        KernelViewScene(),
        SharedPointerScene(),
        HeapTwinScene(),
        PlacementHeapScene(),
        HeapKindsScene(),
        ArchiveMissScene(),
        StoredMSAAScene(),
        FreshTextureScene(),
    ]

    /// QuartzCore 27's Liquid Glass (scenes27), which only iOS 27's app runs.
    public static let glass: any GPUScene = CAGlassScene()
    public static let glassSplit: any GPUScene = CAGlassSplitScene()
}
