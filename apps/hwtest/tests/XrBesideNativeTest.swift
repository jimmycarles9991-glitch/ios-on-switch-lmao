import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// BGR10_XR_sRGB (555) beside a native R8Unorm target, as Weather's moon renders it, in one command buffer: pass A
/// clears both and draws their left halves (texel_values_two: XrSrgb.drawn into the 555), pass B samples the 555 into
/// an RGBA32Float target, pass C loads both and draws the 555's right half (XrSrgb.blended). The same passes with the
/// 555 alone must leave the same 555 words and sample the same values, tolerance 0: a neighbour of a native format
/// changes nothing of how the driver fills and stores the 555's stand-in (format_standin.cpp). Apple's values are
/// gpu.gm20b.xr_srgb's to judge. words_off= texels of the 555 whose word differs, sampled_off= texels pass B read
/// differently; every command buffer raw.
struct XrBesideNativeTest: HWTest {
    let id = "gpu.gm20b.xr_beside_native"
    let name = "BGR10_XR_sRGB beside a native target fills, samples and stores as it does alone, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let alone = try await scene(gpu, beside: false)
        let beside = try await scene(gpu, beside: true)
        let wordsOff = Self.texelsApart(alone.words, beside.words, bytes: 4)
        let sampledOff = Self.texelsApart(alone.sampled, beside.sampled, bytes: 16)
        var metrics = Metrics()
        metrics.set("words_off", wordsOff)
        metrics.set("sampled_off", sampledOff)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let checks = [
            ("the 555's words beside an R8Unorm target are its words alone", wordsOff == 0),
            ("the next pass samples the 555 beside an R8Unorm target as it samples it alone", sampledOff == 0),
            ("every command buffer on the driver", gpu.paths.offRaw.isEmpty),
        ]
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The 555's bytes after the three passes, and the RGBA32Float bytes pass B sampled
    func scene(_ gpu: GPU, beside: Bool) async throws -> (words: [UInt8], sampled: [UInt8]) {
        let n = XrSrgb.side
        let xr = try gpu.texture(.bgr10_xr_srgb, n, n, storage: .shared)
        let r8 = try gpu.texture(.r8Unorm, n, n, storage: .shared)
        let sampled = try gpu.texture(.rgba32Float, n, n, storage: .shared)
        let draw = try gpu.render(fragment: beside ? "texel_values_two" : "texel_values", format: .bgr10_xr_srgb) {
            if beside { $0.colorAttachments[1].pixelFormat = .r8Unorm }
        }
        let read = try gpu.render(fragment: "texel_read", format: .rgba32Float)
        let left = try gpu.buffer(XrSrgb.drawn), right = try gpu.buffer(XrSrgb.blended)
        let targets = Targets(xr: xr, neighbour: beside ? r8 : nil, state: draw)
        try await gpu.run { commands in
            Self.pass(commands, targets, .clear, left, across: -1 ... 0)
            let b = MTLRenderPassDescriptor()
            b.colorAttachments[0].texture = sampled
            b.colorAttachments[0].loadAction = .dontCare
            b.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: b)!
            var corners = GPU.fullScreen
            e.setRenderPipelineState(read)
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentTexture(xr, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
            Self.pass(commands, targets, .load, right, across: 0 ... 1)
        }
        return (try await gpu.read(xr, bytesPerPixel: 4), try await gpu.read(sampled, bytesPerPixel: 16))
    }

    /// The 555, `neighbour` at colour 1 when there is one, and the state that draws them
    struct Targets {
        let xr: MTLTexture
        let neighbour: MTLTexture?
        let state: MTLRenderPipelineState
    }

    /// A pass into `t` that `load`s them and draws `values` over `across` of the target, in clip space
    static func pass(
        _ commands: MTLCommandBuffer, _ t: Targets, _ load: MTLLoadAction, _ values: MTLBuffer,
        across: ClosedRange<Float>
    ) {
        let (xr, neighbour, state) = (t.xr, t.neighbour, t.state)
        let p = MTLRenderPassDescriptor()
        p.colorAttachments[0].texture = xr
        p.colorAttachments[0].loadAction = load
        p.colorAttachments[0].clearColor = XrSrgb.clears[1]
        p.colorAttachments[0].storeAction = .store
        if let neighbour {
            p.colorAttachments[1].texture = neighbour
            p.colorAttachments[1].loadAction = load
            p.colorAttachments[1].clearColor = MTLClearColor(red: 0.3, green: 0, blue: 0, alpha: 1)
            p.colorAttachments[1].storeAction = .store
        }
        let e = commands.makeRenderCommandEncoder(descriptor: p)!
        var corners = GPU.quad(x0: across.lowerBound, y0: -1, x1: across.upperBound, y1: 1, z: 0.5)
        var width = UInt32(xr.width)
        e.setRenderPipelineState(state)
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        e.setFragmentBuffer(values, offset: 0, index: 0)
        e.setFragmentBytes(&width, length: 4, index: 1)
        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        e.endEncoding()
    }

    static func texelsApart(_ a: [UInt8], _ b: [UInt8], bytes: Int) -> Int {
        guard a.count == b.count else { return max(a.count, b.count) / bytes }
        return stride(from: 0, to: a.count, by: bytes).filter { a[$0 ..< $0 + bytes] != b[$0 ..< $0 + bytes] }.count
    }
}
