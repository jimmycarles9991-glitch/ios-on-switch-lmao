import CoreVideo
import Foundation
import HWTestCore
import IOSurface
import Metal

/// mediaanalysisd's Core Image conversion on boot 21 (326 command buffers on m2v, compute.image at slot 1): a kernel
/// samples an RGBA8 surface texture and writes the luma and chroma planes of a 4:2:0 surface, every texture's rows 16
/// bytes off 32, which the texture header cannot address. The raw path reaches them through their own 32-aligned
/// images, filled and copied back by the copy engine around the encoder (raw/runtime/raw_kernel_shadow.h). The planes
/// are read back from the surface under its lock: every texel and the rows' padding as the kernel and the CPU left
/// them, and a CPU write between two command buffers seen by the second.
struct KernelShadowScene: GPUScene {
    let id = "gpu.raw.kernel_shadow"
    let name = "Kernels sampling and writing surface textures whose rows are 16 bytes off 32"

    static let width = 332
    static let height = 78
    static let rgbaRow = 1328  // 16 mod 32
    static let planeRow = 336  // 16 mod 32
    static let padding: UInt8 = 0x5a

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (w, h) = (Self.width, Self.height)
        let chromaOffset = Self.planeRow * h
        guard
            let rgbaSurface = IOSurface(properties: [
                .width: w, .height: h, .bytesPerElement: 4, .bytesPerRow: Self.rgbaRow,
                .allocSize: Self.rgbaRow * h, .pixelFormat: UInt32(0x5247_4241),  // 'RGBA'
            ]),
            let planes = IOSurface(properties: [
                .width: w, .height: h, .pixelFormat: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                .allocSize: chromaOffset + Self.planeRow * h / 2,
                .planeInfo: [
                    [
                        IOSurfacePropertyKey.planeWidth: w, .planeHeight: h, .planeBytesPerRow: Self.planeRow,
                        .planeOffset: 0, .planeSize: Self.planeRow * h, .planeBytesPerElement: 1,
                    ],
                    [
                        IOSurfacePropertyKey.planeWidth: w / 2, .planeHeight: h / 2, .planeBytesPerRow: Self.planeRow,
                        .planeOffset: chromaOffset, .planeSize: Self.planeRow * h / 2, .planeBytesPerElement: 2,
                    ],
                ],
            ])
        else { throw TestFailure("IOSurface creation failed") }
        result.metrics.set(
            "rows", "\(rgbaSurface.bytesPerRow),\(planes.bytesPerRowOfPlane(at: 0)),\(planes.bytesPerRowOfPlane(at: 1))"
        )
        result.expect(
            rgbaSurface.bytesPerRow % 32 == 16 && planes.bytesPerRowOfPlane(at: 0) % 32 == 16
                && planes.bytesPerRowOfPlane(at: 1) % 32 == 16, "the surfaces' rows are not 16 bytes off 32")

        var pixels = [UInt8](repeating: 0, count: w * h * 4)
        for i in 0..<pixels.count { pixels[i] = UInt8(truncatingIfNeeded: i * 37 + 11) }
        Self.store(pixels, into: rgbaSurface)
        planes.lock(options: [], seed: nil)
        memset(planes.baseAddress, Int32(Self.padding), planes.allocationSize)
        planes.unlock(options: [], seed: nil)

        let rgba = try Self.texture(gpu, .rgba8Unorm, rgbaSurface, plane: 0, w, h, [.shaderRead])
        let luma = try Self.texture(gpu, .r8Unorm, planes, plane: 0, w, h, [.shaderWrite])
        let chroma = try Self.texture(gpu, .rg8Unorm, planes, plane: 1, w / 2, h / 2, [.shaderWrite])
        let pipeline = try gpu.compute("shadow_planes")
        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.minFilter = .linear
        samplerDescriptor.magFilter = .linear
        guard let sampler = gpu.device.makeSamplerState(descriptor: samplerDescriptor) else {
            throw TestFailure("makeSamplerState returned nil")
        }
        var paths: [String] = []
        for run in 0..<2 {
            if run == 1 {
                for i in stride(from: 0, to: pixels.count, by: 4) { pixels[i] ^= 0xff }
                Self.store(pixels, into: rgbaSurface)
            }
            let timing = try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(pipeline)
                encoder.setTexture(luma, index: 0)
                encoder.setTexture(rgba, index: 1)
                encoder.setTexture(chroma, index: 2)
                encoder.setSamplerState(sampler, index: 0)
                encoder.dispatchThreads(
                    MTLSize(width: w, height: h, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 32, height: 8, depth: 1))
                encoder.endEncoding()
            }
            paths.append(timing.path)
            let wrong = Self.wrongBytes(planes, pixels)
            result.metrics.set("run\(run)_wrong", wrong.count)
            result.expect(
                wrong.isEmpty, "run \(run): \(wrong.count) surface bytes wrong, the first at \(wrong.first ?? -1)")
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(paths.allSatisfy { $0 == "raw" }, "the kernels took \(paths), not raw")
        }
        return result
    }

    private static func texture(
        _ gpu: GPU, _ format: MTLPixelFormat, _ surface: IOSurface, plane: Int, _ width: Int, _ height: Int,
        _ usage: MTLTextureUsage
    ) throws -> MTLTexture {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: false)
        descriptor.usage = usage
        descriptor.storageMode = .shared
        guard
            let texture = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: plane)
        else { throw TestFailure("makeTexture(\(format.rawValue), plane \(plane)) returned nil") }
        return texture
    }

    private static func store(_ pixels: [UInt8], into surface: IOSurface) {
        surface.lock(options: [], seed: nil)
        for y in 0..<height {
            pixels.withUnsafeBytes { source in
                (surface.baseAddress + y * surface.bytesPerRow).copyMemory(
                    from: source.baseAddress! + y * width * 4, byteCount: width * 4)
            }
        }
        surface.unlock(options: [], seed: nil)
    }

    /// Every byte of both planes against the kernel's result: luma the green, chroma (blue, red) at each 2x2 block's
    /// first pixel, the rows' padding untouched
    private static func wrongBytes(_ planes: IOSurface, _ pixels: [UInt8]) -> [Int] {
        planes.lock(options: .readOnly, seed: nil)
        defer { planes.unlock(options: .readOnly, seed: nil) }
        var wrong: [Int] = []
        for plane in 0..<2 {
            let base = planes.baseAddressOfPlane(at: plane).assumingMemoryBound(to: UInt8.self)
            let row = planes.bytesPerRowOfPlane(at: plane)
            let (texels, rows, bytes) = plane == 0 ? (width, height, 1) : (width / 2, height / 2, 2)
            for y in 0..<rows {
                for x in 0..<row {
                    let at = y * row + x
                    var want = padding
                    if x < texels * bytes {
                        let texel = x / bytes, channel = x % bytes
                        let source = plane == 0 ? (y * width + texel) * 4 : (2 * y * width + 2 * texel) * 4
                        want = plane == 0 ? pixels[source + 1] : pixels[source + (channel == 0 ? 2 : 0)]
                    }
                    if base[at] != want { wrong.append(plane << 24 | at) }
                }
            }
        }
        return wrong
    }
}
