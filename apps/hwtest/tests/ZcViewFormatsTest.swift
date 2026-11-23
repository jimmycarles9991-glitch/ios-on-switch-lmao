import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Whether PixelFormatView textures can join compressed memory (a probe; LIST #7): a private RGBA8Unorm 256x256 target
/// of pixelFormatView usage, forced onto compressible memory (switchMetalProbeCompressNext), and its R32Uint and
/// BGRA8Unorm views (raw views over the same binding). It is drawn through each in turn (a word per pixel through
/// R32Uint, codes through the colour views), then read by the copy engine (`view_render_wrong`: texels off the bytes
/// the draw stored) and by the texture unit through the other two (`view_sample_wrong`); last a clear to 0 through the
/// R32Uint view, a boot ZBC value, read every way (`view_zbc_wrong`: texels not 0). The same on a plain target is the
/// control (`control_*`). An M4 Max matches the CPU's model of every read (hwtests12 mac/gpu.gm20b.zc_view_formats). It
/// decides, not judges: all 0 means the mask can take PixelFormatView; a count is the finding. FAIL only where it could
/// not run.
struct ZcViewFormatsTest: HWTest {
    let id = "gpu.gm20b.zc_view_formats"
    let name = "PixelFormatView probe: a compressed RGBA8 target drawn and read through R32Uint and BGRA8 views"
    let subsystem = Subsystem.metal

    static let side = 256
    static let compressNext = NSSelectorFromString("switchMetalProbeCompressNext")

    struct Counts {
        var render = 0, sample = 0, zbc = 0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.compressNext) else {
            return .skip(GM20BFacade.missing("switchMetalProbeCompressNext", on: gpu.device))
        }
        var metrics = Metrics()
        for compressed in [true, false] {
            if compressed { _ = facade.perform(Self.compressNext) }
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: Self.side, height: Self.side, mipmapped: false
            )
            d.usage = [.renderTarget, .shaderRead, .pixelFormatView]
            d.storageMode = .private
            guard let texture = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no RGBA8 target made") }
            let comptags = CompressionSettled.comptags(gpu.device, texture) ?? -1
            let prefix = compressed ? "view_" : "control_"
            metrics.set(prefix + "comptags", comptags)
            if compressed && comptags != 1 {
                return .fail("switchMetalProbeCompressNext gave the target no compressible memory", metrics)
            }
            let counts = try await Self.probe(gpu, texture)
            metrics.set(prefix + "render_wrong", counts.render)
            metrics.set(prefix + "sample_wrong", counts.sample)
            metrics.set(prefix + "zbc_wrong", counts.zbc)
        }
        return Outcome.pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }

    static func hash(_ x: Int, _ y: Int, _ seed: UInt32) -> UInt32 {
        var h = (UInt32(x) &* 73_856_093) ^ (UInt32(y) &* 19_349_663) ^ (seed &* 83_492_791)
        h ^= h >> 13
        h = h &* 0x5bd1_e995
        return h ^ (h >> 15)
    }

    /// A texel's memory word read as BGRA codes packed red first: its bytes 2, 1, 0, 3
    static func swapped(_ w: UInt32) -> UInt32 { (w >> 16 & 255) | (w & 0xff00) | (w & 255) << 16 | (w & 0xff00_0000) }

    static func probe(_ gpu: GPU, _ texture: MTLTexture) async throws -> Counts {
        guard let asUint = texture.makeTextureView(pixelFormat: .r32Uint),
              let asBgra = texture.makeTextureView(pixelFormat: .bgra8Unorm)
        else { throw TestFailure("no R32Uint or BGRA8 view made") }
        let renders: [(MTLTexture, String, MTLPixelFormat, UInt32)] = [
            (asUint, "zc_view_uint_fragment", .r32Uint, 1), (asBgra, "zc_view_float_fragment", .bgra8Unorm, 2),
            (texture, "zc_view_float_fragment", .rgba8Unorm, 3),
        ]
        var counts = Counts()
        for (target, fragment, format, seed) in renders {
            let pipeline = try gpu.render(fragment: fragment, format: format)
            try await draw(gpu, target, pipeline, seed: seed)
            let stored: [UInt32] = (0 ..< side * side).map { i in
                let h = hash(i % side, i / side, seed)
                return format == .bgra8Unorm ? swapped(h) : h
            }
            let blit = try await blitWords(gpu, texture)
            let viaUint = try await words(gpu, asUint, "zc_view_read_uint")
            let viaRgba = try await words(gpu, texture, "zc_view_read_codes")
            let viaBgra = try await words(gpu, asBgra, "zc_view_read_codes")
            counts.render += off(blit, stored, { $0 })
            if format != .r32Uint { counts.sample += off(viaUint, stored, { $0 }) }
            if format != .rgba8Unorm { counts.sample += off(viaRgba, stored, { $0 }) }
            if format != .bgra8Unorm { counts.sample += off(viaBgra, stored, swapped) }
        }
        try await draw(gpu, asUint, nil, seed: 0)
        let blit = try await blitWords(gpu, texture)
        let viaUint = try await words(gpu, asUint, "zc_view_read_uint")
        let viaRgba = try await words(gpu, texture, "zc_view_read_codes")
        let viaBgra = try await words(gpu, asBgra, "zc_view_read_codes")
        for read in [blit, viaUint, viaRgba, viaBgra] { counts.zbc += read.filter { $0 != 0 }.count }
        return counts
    }

    static func off(_ got: [UInt32], _ stored: [UInt32], _ model: (UInt32) -> UInt32) -> Int {
        var n = 0
        for i in 0 ..< min(got.count, stored.count) where got[i] != model(stored[i]) { n += 1 }
        return n + abs(got.count - stored.count)
    }

    /// `pipeline`'s word for each pixel over all of `target`, or with none a clear of it to 0
    static func draw(_ gpu: GPU, _ target: MTLTexture, _ pipeline: MTLRenderPipelineState?, seed: UInt32) async throws {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = pipeline == nil ? .clear : .dontCare
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let pipeline {
                encoder.setRenderPipelineState(pipeline)
                var corners = GPU.fullScreen
                var word = seed
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBytes(&word, length: 4, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
    }

    static func words(_ gpu: GPU, _ view: MTLTexture, _ kernel: String) async throws -> [UInt32] {
        let out = try gpu.buffer(side * side * 4)
        let pipeline = try gpu.compute(kernel)
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(view, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: side, height: side, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }
        let read = out.contents().bindMemory(to: UInt32.self, capacity: side * side)
        return Array(UnsafeBufferPointer(start: read, count: side * side))
    }

    static func blitWords(_ gpu: GPU, _ texture: MTLTexture) async throws -> [UInt32] {
        let bytes = try await gpu.read(texture, bytesPerPixel: 4)
        return (0 ..< side * side).map { bytes.loaded(as: UInt32.self, at: $0) }
    }
}
