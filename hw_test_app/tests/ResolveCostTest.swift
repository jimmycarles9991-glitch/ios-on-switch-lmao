import HWTestCore
import HWTestGPU
import Metal

/// What a 720x1280 multisampled resolve costs the GPU: a cleared pass stored with and without its resolve, the
/// difference the resolve pass. The lock screen's Celosia poster resolves a 4x BGRA8 memoryless target into its
/// drawable every frame, and that pass took 11.5-12.1 ms on device, 12.5-13.1 ns a pixel; MapKit's 4x BGRA8 sRGB
/// resolves, another program over the same fetch, took 12.6-13.7 ns, where reading 14.7 MB and writing 3.7 MB takes
/// under 1 ms. The variants split the cost between the resolve programs (rgba8: a loop over the sample count, rgba16f:
/// its four reads in line) and the multisampled fetch, and between memoryless and stored sources; ResolveArms splits the
/// fetch further (four multisampled fetches against one, against a plain texture of the same bytes tiled and pitch-linear).
struct ResolveCostTest: HWTest {
    let id = "gpu.msaa.resolve_cost"
    let name = "A 720x1280 4x resolve costs the GPU what its bytes do"
    let subsystem = Subsystem.metal

    static let width = 720
    static let height = 1280
    static let runs = 6
    static let budgetMs = 2.0
    static let clear = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)

    struct Variant {
        let label: String
        let format: MTLPixelFormat
        let samples: Int
        let storage: MTLStorageMode
        /// The clear colour as the resolved texture holds it
        let texel: [UInt8]
    }

    static let rgba8: [UInt8] = [64, 128, 191, 255]
    static let rgba16f: [UInt8] = [0x00, 0x34, 0x00, 0x38, 0x00, 0x3a, 0x00, 0x3c]
    static let variants = [
        Variant(label: "rgba8_4x_memoryless", format: .rgba8Unorm, samples: 4, storage: .memoryless, texel: rgba8),
        Variant(label: "rgba8_4x_private", format: .rgba8Unorm, samples: 4, storage: .private, texel: rgba8),
        Variant(label: "rgba8_2x_memoryless", format: .rgba8Unorm, samples: 2, storage: .memoryless, texel: rgba8),
        Variant(
            label: "rgba16f_4x_memoryless", format: .rgba16Float, samples: 4, storage: .memoryless, texel: rgba16f),
    ]

    struct Runs {
        var ms = 0.0
        var paths = Set<String>()
        var clocks = Set<String>()
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard gpu.device.supportsTextureSampleCount(4) else { return .fail("4x MSAA unsupported") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for variant in Self.variants {
            let resolved = try gpu.texture(variant.format, Self.width, Self.height)
            let target = try gpu.texture(
                variant.format, Self.width, Self.height, usage: [.renderTarget], samples: variant.samples,
                storage: variant.storage)
            let with = try await median(gpu, target, resolve: resolved)
            let without = try await median(gpu, target, resolve: nil)
            let texel = Array(
                try await gpu.read(resolved, bytesPerPixel: variant.texel.count).prefix(variant.texel.count))
            let resolveMs = max(0, with.ms - without.ms)
            let raw = with.paths == ["raw"] && without.paths == ["raw"]
            let gpuClocked = with.clocks == ["ns"] && without.clocks == ["ns"]
            metrics.set("\(variant.label)_pass_ms", with.ms, digits: 3)
            metrics.set("\(variant.label)_bare_ms", without.ms, digits: 3)
            metrics.set("\(variant.label)_ms", resolveMs, digits: 3)
            metrics.set("\(variant.label)_ns_px", resolveMs * 1e6 / Double(Self.width * Self.height), digits: 2)
            metrics.set("\(variant.label)_clock", with.clocks.union(without.clocks).sorted().joined(separator: ","))
            metrics.set("\(variant.label)_paths", with.paths.union(without.paths).sorted().joined(separator: ","))
            checks.append(("\(variant.label) resolved the clear colour", texel == variant.texel))
            if variant.label == "rgba8_4x_memoryless" && raw && gpuClocked {
                checks.append(
                    ("the poster's 720x1280 4x resolve within \(Self.budgetMs) ms", resolveMs <= Self.budgetMs))
            }
        }
        try await ResolveArms.measure(gpu, into: &metrics)
        if checks.count == Self.variants.count && checks.allSatisfy(\.1) {
            return .skip("the poster's form did not run on the raw path with GPU stamps", metrics)
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The median GPU time of a cleared pass over `target`, resolved into `resolve` or not stored.
    func median(_ gpu: GPU, _ target: MTLTexture, resolve: MTLTexture?) async throws -> Runs {
        var times: [Double] = []
        var runs = Runs()
        for _ in 0..<Self.runs {
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].resolveTexture = resolve
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = Self.clear
                pass.colorAttachments[0].storeAction = resolve == nil ? .dontCare : .multisampleResolve
                commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
            }
            times.append(timing.gpuMilliseconds)
            runs.paths.insert(timing.path)
            runs.clocks.insert(timing.gpuClock ?? "device")
        }
        runs.ms = times.sorted()[times.count / 2]
        return runs
    }
}
