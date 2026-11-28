import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A kernel sampling a two-plane 4:2:0 YCbCr texture (540, RGB8_420_2P read as codes, over a '420v' surface) beside an
/// RGBA8 texture, across a grid twice the textures' width. Metal samples the YCbCr texture's wrapping modes and clamps to
/// zero as clamp to edge (an M4 Max's reads) and the RGBA texture's as they are. The driver refused every constexpr
/// sampler beside such a texture (compute.image): a kernel's sampler is a word of its own. Now the record names the
/// texture each sampler samples (pack.h kernelSamplerPairs), and the direct compile compiles a kernel under its
/// textures' address maps (NativeMetalDirectCompute.rs): one sampler the maps give two sets of modes is a kernel the
/// compile cannot make, so its command buffer fails MTLCommandBufferErrorInternal with no predicate (error:none, the
/// scene's one failed command buffer; boot C24: "A render pipeline could not be compiled"), where an M4 Max samples it.
/// Every sample is a texel's codes, exact.
struct KernelYcbcrSamplerScene: GPUScene {
    let id = "gpu.gm20b.kernel_ycbcr_sampler"
    let name = "A kernel sampling a YCbCr and an RGBA texture through samplers of their own"
    func run(_ gpu: GPU) async throws -> SceneResult { try await KernelYcbcrSampling(form: .edge).run(gpu) }
}

struct KernelYcbcrMappedSamplerScene: GPUScene {
    let id = "gpu.gm20b.kernel_ycbcr_sampler_mapped"
    let name = "A kernel sampling a YCbCr texture through samplers its address map changes"
    func run(_ gpu: GPU) async throws -> SceneResult { try await KernelYcbcrSampling(form: .mapped).run(gpu) }
}

struct KernelYcbcrSharedSamplerScene: GPUScene {
    let id = "gpu.gm20b.kernel_ycbcr_shared_sampler"
    let name = "A kernel sampling a YCbCr and an RGBA texture through one sampler"
    var expectedFailures: [String: Int] { ["none": 1] }
    func run(_ gpu: GPU) async throws -> SceneResult { try await KernelYcbcrSampling(form: .shared).run(gpu) }
}

struct KernelYcbcrSampling {
    enum Form {
        /// A constexpr clamp-to-edge sampler of the YCbCr texture, a bound repeating one of the RGBA texture
        case edge
        /// Constexpr repeating and clamp-to-zero samplers of the YCbCr texture, which its map changes
        case mapped
        /// One bound repeating sampler of both, which one sampler cannot be: raw fails the command buffer
        case shared
    }

    let form: Form

    static let width = 16
    static let height = 8
    static let codes: UInt = 540
    static let cb: UInt8 = 0x40
    static let cr: UInt8 = 0xC0

    private var kernel: String {
        switch form {
        case .edge: "ycbcr_sampler_pairs"
        case .mapped: "ycbcr_sampler_mapped"
        case .shared: "ycbcr_sampler_shared"
        }
    }

    /// Each sample a thread writes: true for the YCbCr texture's (its edge past its right side), false for the RGBA one's
    /// (repeated)
    private var samples: [Bool] { form == .mapped ? [true, true, false] : [true, false] }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (w, h) = (Self.width, Self.height)
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard
            CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &made)
            == kCVReturnSuccess,
            let buffer = made, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
            let format = MTLPixelFormat(rawValue: Self.codes)
        else { throw TestFailure("no '420v' surface") }
        Self.fill(buffer)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: w, height: h, mipmapped: false
        )
        descriptor.usage = [.shaderRead]
        guard let ycbcr = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("makeTexture(540 over '420v') returned nil")
        }
        let rgba = try gpu.texture(.rgba8Unorm, w, h, usage: [.shaderRead])
        try await gpu.upload((0 ..< w * h).flatMap { Self.rgba($0 % w, $0 / w) }, to: rgba, bytesPerRow: w * 4)
        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.sAddressMode = .repeat
        samplerDescriptor.tAddressMode = .repeat
        guard let sampler = gpu.device.makeSamplerState(descriptor: samplerDescriptor) else {
            throw TestFailure("makeSamplerState returned nil")
        }
        let pipeline = try gpu.compute(kernel)
        let count = 2 * w * h * samples.count
        let out = try gpu.buffer(count * MemoryLayout<SIMD4<Float>>.stride)
        let raw = GM20BFacade.kind(of: gpu.device) == "gm20b"
        let timing = try await gpu.run(tolerateError: form == .shared && raw) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(ycbcr, index: 0)
            encoder.setTexture(rgba, index: 1)
            encoder.setSamplerState(sampler, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: 2 * w, height: h, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }
        result.metrics.set("path", String(timing.path.prefix(80)))
        if form == .shared && raw {
            let internalError = "error:\(MTLCommandBufferErrorDomain)/\(MTLCommandBufferError.internal.rawValue) "
            result.expect(
                timing.path.hasPrefix(internalError) && timing.facadePath == "error:none",
                "the command buffer took \(timing.path) (\(timing.facadePath)), not Internal with no predicate"
            )
            return result
        }
        let got = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: count)
        var wrong = [0, 0]
        var maxError: Float = 0
        for y in 0 ..< h {
            for x in 0 ..< 2 * w {
                for (k, ycbcrSample) in samples.enumerated() {
                    let want = ycbcrSample ? Self.ycbcr(min(x, w - 1), y) : Self.rgba(x % w, y)
                    let sample = got[(y * 2 * w + x) * samples.count + k] * 255
                    let error = (0 ..< 4).map { abs(sample[$0] - Float(want[$0])) }.max() ?? 0
                    maxError = max(maxError, error.isNaN ? .infinity : error)
                    if !(error < 0.01) { wrong[ycbcrSample ? 0 : 1] += 1 }
                }
            }
        }
        result.metrics.set("max_error", String(format: "%.3f", maxError))
        result.metrics.set("wrong_ycbcr", wrong[0])
        result.metrics.set("wrong_rgba", wrong[1])
        result.metrics.set("past_edge", String(format: "%.1f", got[(2 * w - 1) * samples.count][1] * 255))
        result.expect(
            wrong == [0, 0],
            "samples off their texel's codes: \(wrong[0]) YCbCr (clamped to its edge), \(wrong[1]) RGBA (repeated); "
                + "largest error \(maxError) codes"
        )
        if raw {
            result.expect(timing.path == "gm20b", "the kernel took \(timing.path), not raw")
        }
        return result
    }

    /// Luma 32 + 11x + 3y, every chroma pair (Cb, Cr) the same
    private static func fill(_ buffer: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let luma = CVPixelBufferGetBaseAddressOfPlane(buffer, 0)!.assumingMemoryBound(to: UInt8.self)
        let lumaRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        let chroma = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)!.assumingMemoryBound(to: UInt8.self)
        let chromaRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        for y in 0 ..< height {
            for x in 0 ..< width { luma[y * lumaRow + x] = UInt8(32 + 11 * x + 3 * y) }
        }
        for y in 0 ..< height / 2 {
            for x in 0 ..< width / 2 {
                chroma[y * chromaRow + 2 * x] = cb
                chroma[y * chromaRow + 2 * x + 1] = cr
            }
        }
    }

    /// The codes texture's texel: red Cr, green Y, blue Cb, alpha one
    private static func ycbcr(_ x: Int, _ y: Int) -> [UInt8] { [cr, UInt8(32 + 11 * x + 3 * y), cb, 255] }

    private static func rgba(_ x: Int, _ y: Int) -> [UInt8] { [UInt8(13 * x + 1), UInt8(29 * y + 2), 77, 255] }
}
