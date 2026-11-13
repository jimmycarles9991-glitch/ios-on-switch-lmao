import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Compression and zero-bandwidth clears for the targets no CPU or other engine reaches
/// (drivers/TegraGM20BMetal/raw/runtime/raw_comp.h, raw_zbc.h): 4x multisampled memoryless RGBA16Float, BGRA8 and RGBA8
/// colour over 4x Depth16Unorm, and one-sample BGRA8 over a memoryless Depth32Float_Stencil8, each cleared again and
/// again to a value the boot ZBC table lacks, a quad drawn over the left half and the colour resolved or stored. The
/// pixels must be the quad's and the clear's exactly: each added entry's texel, in its format's channel order, read back
/// through every later clear that hits it. The RGBA16Float target cleared to the BGRA8 one's floats must get its own halves,
/// not the BGRA8 texel (the GPU matches an entry by format and value). The facade must make compressed memory, add the
/// entries and hit them. Then the shadow cases (CompressedShadowCases: the copy engine through compressed memory), and a
/// 4x BGRA8 target cleared to a value the table holds and to 0.5 (a ROP tie, never added): the GPU time of each.
struct CompressedTargetsTest: HWTest {
    let id = "gpu.raw.compressed_targets"
    let name = "Multisampled and depth memoryless targets compress; repeated clears become zero-bandwidth"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let width = 640
    static let height = 360
    static let quadColour = SIMD4<Float>(1, 0, 0, 1)
    static let rounds = 12

    struct Case {
        let label: String
        let colour: MTLPixelFormat
        let depth: MTLPixelFormat
        let samples: Int
        let clear: MTLClearColor
        let clearDepth: Double
    }

    static let exactCodes = MTLClearColor(red: 51.0 / 255, green: 102.0 / 255, blue: 204.0 / 255, alpha: 1)

    static let cases = [
        Case(
            label: "ms4_bgra8", colour: .bgra8Unorm, depth: .depth16Unorm, samples: 4, clear: exactCodes,
            clearDepth: 0.75),
        Case(
            label: "ms4_rgba8", colour: .rgba8Unorm, depth: .depth16Unorm, samples: 4, clear: exactCodes,
            clearDepth: 0.75),
        Case(
            label: "ms4_f16", colour: .rgba16Float, depth: .depth16Unorm, samples: 4,
            clear: MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1), clearDepth: 0.75),
        Case(
            label: "ms4_f16_bgra8_floats", colour: .rgba16Float, depth: .depth16Unorm, samples: 4, clear: exactCodes,
            clearDepth: 0.75),
        Case(
            label: "d32s8", colour: .bgra8Unorm, depth: .depth32Float_stencil8, samples: 1,
            clear: MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1), clearDepth: 0.5),
    ]

    static func pass(_ c: Case, colour: MTLTexture, depth: MTLTexture, resolve: MTLTexture?) -> MTLRenderPassDescriptor
    {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colour
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = c.clear
        if let resolve {
            pass.colorAttachments[0].resolveTexture = resolve
            pass.colorAttachments[0].storeAction = .multisampleResolve
        } else {
            pass.colorAttachments[0].storeAction = .store
        }
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = c.clearDepth
        pass.depthAttachment.storeAction = .dontCare
        if c.depth == .depth32Float_stencil8 {
            pass.stencilAttachment.texture = depth
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.storeAction = .dontCare
        }
        return pass
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw", !RawFacade.compression(of: gpu.device).isEmpty else {
            return .skip("the default device is not the raw facade (\(kind))")
        }
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: Set<String> = []
        let start = RawFacade.compression(of: gpu.device)
        let depthState = gpu.device.makeDepthStencilState(
            descriptor: {
                let d = MTLDepthStencilDescriptor()
                d.depthCompareFunction = .less
                d.isDepthWriteEnabled = true
                return d
            }())!
        for c in Self.cases {
            let colour = try gpu.texture(
                c.colour, Self.width, Self.height, usage: [.renderTarget], samples: c.samples, storage: .memoryless)
            let depth = try gpu.texture(
                c.depth, Self.width, Self.height, usage: [.renderTarget], samples: c.samples, storage: .memoryless)
            let out = try gpu.texture(c.colour, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
            let state = try gpu.render(
                fragment: "exact_fragment", format: c.colour, samples: c.samples, depth: c.depth,
                stencil: c.depth == .depth32Float_stencil8 ? c.depth : .invalid)
            var corners = GPU.pixelQuad(
                0, 0, Self.width / 2, Self.height, width: Self.width, height: Self.height, z: 0.25)
            for _ in 0..<Self.rounds {
                paths.insert(
                    try await gpu.run { commands in
                        let encoder = commands.makeRenderCommandEncoder(
                            descriptor: Self.pass(
                                c, colour: c.samples > 1 ? colour : out, depth: depth,
                                resolve: c.samples > 1 ? out : nil))!
                        encoder.setRenderPipelineState(state)
                        encoder.setDepthStencilState(depthState)
                        var value = Self.quadColour
                        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                        encoder.setFragmentBytes(&value, length: 16, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }.path)
                try await Task.sleep(nanoseconds: 20_000_000)
            }
            let half = c.colour == .rgba16Float
            let raster = try await gpu.raster(
                out, half ? .rgba16Float : c.colour == .rgba8Unorm ? .rgba8Unorm : .bgra8Unorm)
            let clear = [c.clear.red, c.clear.green, c.clear.blue, c.clear.alpha].map {
                half ? Double(Float(Float16(Float($0)))) : $0
            }
            var bad = 0
            for y in 0..<Self.height {
                for x in 0..<Self.width {
                    for ch in 0..<4 {
                        let want = x < Self.width / 2 ? Double(Self.quadColour[ch]) : clear[ch]
                        if abs(Double(raster[x, y, ch]) - want) > (half ? 1e-6 : 0.5 / 255) { bad += 1 }
                    }
                }
            }
            metrics.set("\(c.label)_wrong", bad)
            if bad != 0 { wrong.append("\(c.label)_pixels") }
        }
        let after = RawFacade.compression(of: gpu.device)
        let delta = { (key: String) in (after[key] ?? 0) - (start[key] ?? 0) }
        for key in ["made", "refused", "zbc_added", "zbc_full", "zbc_hits", "zbc_misses", "zbc_inexact"] {
            metrics.set(key, delta(key))
        }
        metrics.set("views", after["views"] ?? 0)
        if delta("made") == 0 && (after["views"] ?? 0) == 0 { wrong.append("no_compressed_memory") }
        if delta("zbc_added") + delta("zbc_full") == 0 { wrong.append("no_zbc_entry_asked") }
        if delta("zbc_added") > 0 && delta("zbc_hits") == 0 { wrong.append("no_zbc_hit") }
        try await CompressedShadowCases.run(gpu, &metrics, &wrong)
        let (heldMs, tieMs) = try await Self.clearTimes(gpu)
        metrics.set("zbc_clear_gpu_ms", heldMs, digits: 3)
        metrics.set("tie_clear_gpu_ms", tieMs, digits: 3)
        metrics.set("paths", paths.sorted().joined(separator: ","))
        if paths != ["raw"] { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// GPU milliseconds of 16 clears of a 4x BGRA8 memoryless 1280x720 target to opaque black (the boot table's) and to
    /// 0.5 grey (a tie no entry may hold), after warm-up passes made its compressed memory
    static func clearTimes(_ gpu: GPU) async throws -> (Double, Double) {
        let target = try gpu.texture(.bgra8Unorm, 1280, 720, usage: [.renderTarget], samples: 4, storage: .memoryless)
        func clears(_ colour: MTLClearColor) async throws -> Double {
            var ms = 0.0
            for _ in 0..<16 {
                ms += try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].clearColor = colour
                    pass.colorAttachments[0].storeAction = .dontCare
                    commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
                }.gpuMilliseconds
            }
            return ms
        }
        let black = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        _ = try await clears(black)
        try await Task.sleep(nanoseconds: 100_000_000)
        return (try await clears(black), try await clears(MTLClearColor(red: 0.5, green: 0.5, blue: 0.5, alpha: 1)))
    }
}
