import Foundation
import HWTestCore
import IOSurface
import Metal

/// Texture buffers of two-plane YCbCr (500, 503, 540 over '420v') and packed 10-bit (588, 589 over 'p420') formats over
/// a surface's plane 0 (gpu.gm20b.ycbcr_texel_buffers, gpu.gm20b.ycbcr_texel_buffer_wide), each against the 2D texture of
/// its format over the same plane: an M4 Max makes them at any width a plane's rows hold, and a kernel reads buffer
/// texel i as the 2D texture's texel (i, 0).
public enum YcbcrTexelBuffers {
    typealias P = SurfaceReads.Plane
    public static let formats: [UInt] = [500, 503, 540, 588, 589]
    /// Bytes after which every plane of the wide scene repeats (a whole number of packed words, no divisor of 16384)
    static let period = 12000

    public struct Reading: Sendable {
        public var made = false
        /// The width a kernel sees
        public var width = -1
        /// Texels off the 2D texture's: texel i against its (i % 16384, i / 16384)
        public var off = -1
        /// Past texel 16383 (wide only, recorded): texels that read the texel a period back, 0, the texel 16384 back,
        /// texel 16383, the 2D texture's second row
        public var fits: (period: Int, zero: Int, wrap: Int, clamp: Int, row: Int) = (-1, -1, -1, -1, -1)
    }

    static func packed(_ format: UInt) -> Bool { format == 588 || format == 589 }

    /// Texels a period of `period` bytes of plane 0 spans in `format`
    static func periodTexels(_ format: UInt) -> Int {
        format == 588 ? period / 4 * 3 : format == 589 ? period / 4 * 3 / 2 : period
    }

    /// A '420v' ('p420' for the packed formats) of `width` x `height` texels, plane 0 rows `rows` at 0 and plane 1 rows
    /// `rows` from the next 4 KiB on
    static func surface(
        _ format: UInt, width: Int, height: Int, rows: Int, fill: (Int, Int) -> UInt8
    ) -> IOSurfaceRef? {
        let chroma = (rows * height + 4095) / 4096 * 4096
        let allocation = (chroma + rows * height / 2 + 16383) / 16384 * 16384
        let planes = [
            P(at: 0, width, height, bpe: 1, rows: rows), P(at: chroma, width / 2, height / 2, bpe: 2, rows: rows),
        ]
        return SurfaceReads.surface(
            packed(format) ? "p420" : "420v", width, height, planes: planes, allocation: allocation
        ) { i in i < chroma ? fill(0, i) : fill(1, i - chroma) }
    }

    /// Plane 0's rows for a buffer of `width` texels: a packed word holds three codes
    static func rows(_ format: UInt, _ width: Int) -> Int {
        let bytes = format == 588 ? (width + 2) / 3 * 4 : format == 589 ? (2 * width + 2) / 3 * 4 : width
        return (bytes + 255) / 256 * 256
    }

    static func buffer(_ gpu: GPU, _ format: UInt, _ width: Int, over surface: IOSurfaceRef) -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.textureBufferDescriptor(
            with: MTLPixelFormat(rawValue: format)!, width: width, resourceOptions: .storageModeShared,
            usage: [.shaderRead]
        )
        return gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
    }

    static func read(_ gpu: GPU, buffer texture: MTLTexture) async throws -> (texels: [SIMD4<Float>], width: Int) {
        let count = texture.width
        let out = try gpu.buffer(count * 16), width = try gpu.buffer(4)
        let read = try gpu.compute("read_texture_buffer"), seen = try gpu.compute("pbuf_width")
        let path = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(read)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.setComputePipelineState(seen)
            encoder.setBuffer(width, offset: 0, index: 0)
            let one = MTLSize(width: 1, height: 1, depth: 1)
            encoder.dispatchThreads(one, threadsPerThreadgroup: one)
            encoder.endEncoding()
        }.facadePath
        guard !path.hasPrefix("error") else { throw TestFailure("the buffer's read failed: \(path)") }
        let texels = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: count)
        return (Array(UnsafeBufferPointer(start: texels, count: count)), Int(width.contents().load(as: UInt32.self)))
    }

    static func tolerance(_ format: UInt) -> Float { packed(format) ? 0.5 / 1023 : 0.5 / 255 }

    static func near(_ a: SIMD4<Float>, _ b: SIMD4<Float>, _ tolerance: Float) -> Bool {
        (0 ..< 4).allSatisfy { abs(a[$0] - b[$0]) <= tolerance }
    }

    /// A buffer of `width` texels (64 over a 64x8 of rows 256, chroma at 4096; 16384 over two rows) against the 2D
    /// texture's row 0, every byte (i * 37 + (i >> 8) * 11 + 5) & 255 of its plane
    public static func row(_ gpu: GPU, _ format: UInt, width: Int) async throws -> Reading {
        let height = width == 64 ? 8 : 2, rows = width == 64 ? 256 : rows(format, width)
        guard let surface = surface(format, width: width, height: height, rows: rows, fill: { _, i in
            UInt8(truncatingIfNeeded: i * 37 + (i >> 8) * 11 + 5)
        })
        else { throw TestFailure("no surface for \(format) \(width) wide") }
        guard let texture = buffer(gpu, format, width, over: surface) else { return Reading() }
        guard let flat = SurfaceReads.texture(gpu, format, width, height, over: surface) else {
            throw TestFailure("no 2D \(format) \(width)x\(height) to compare with")
        }
        let (texels, seen) = try await read(gpu, buffer: texture)
        let reference = try await SurfaceReads.read(gpu, flat)
        let off = (0 ..< width).filter { !near(texels[$0], reference[$0], tolerance(format)) }.count
        return Reading(made: true, width: seen, off: off)
    }

    /// A buffer of 20000 texels over a plane of two rows, against the 2D texture of 16384 x 2 (the widest a 2D texture
    /// is) over the same plane: an M4 Max reads texel i at the 2D texture's (i % 16384, i / 16384), past texel 16383
    /// the plane's second row, not the first row's bytes on (every plane repeats every 12000 bytes, which tells the two
    /// apart)
    public static func wide(_ gpu: GPU, _ format: UInt) async throws -> Reading {
        let width = 20000, rows = rows(format, width)
        let byte = { (j: Int) in UInt8(truncatingIfNeeded: (j % period) * 37 + ((j % period) >> 8) * 11 + 5) }
        guard let surface = surface(format, width: width, height: 2, rows: rows, fill: { _, j in byte(j) }) else {
            throw TestFailure("no surface for \(format) 20000 wide")
        }
        guard let texture = buffer(gpu, format, width, over: surface) else { return Reading() }
        guard let flat = SurfaceReads.texture(gpu, format, 16384, 2, over: surface) else {
            throw TestFailure("no 2D \(format) 16384x2 to compare with")
        }
        let (texels, seen) = try await read(gpu, buffer: texture)
        let reference = try await SurfaceReads.read(gpu, flat)
        let tolerance = tolerance(format), back = periodTexels(format), past = 16384 ..< width
        let off = (0 ..< width).filter { !near(texels[$0], reference[$0], tolerance) }.count
        let fits = (
            period: past.filter { near(texels[$0], texels[$0 - back], tolerance) }.count,
            zero: past.filter { near(texels[$0], .zero, tolerance) }.count,
            wrap: past.filter { near(texels[$0], texels[$0 - 16384], tolerance) }.count,
            clamp: past.filter { near(texels[$0], texels[16383], tolerance) }.count,
            row: past.filter { near(texels[$0], reference[$0], tolerance) }.count
        )
        return Reading(made: true, width: seen, off: off, fits: fits)
    }

    /// Whether a 500 texture buffer of 64 texels of render-target usage is made over a '420v' 64x8 (an M4 Max: nil)
    public static func renderUsageMade(_ gpu: GPU) throws -> Bool {
        guard let surface = surface(500, width: 64, height: 8, rows: 256, fill: { _, _ in 0xee }) else {
            throw TestFailure("no '420v' 64x8")
        }
        let descriptor = MTLTextureDescriptor.textureBufferDescriptor(
            with: MTLPixelFormat(rawValue: 500)!, width: 64, resourceOptions: .storageModeShared,
            usage: [.shaderRead, .renderTarget]
        )
        return gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) != nil
    }
}
