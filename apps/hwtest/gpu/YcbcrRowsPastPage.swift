import Foundation
import HWTestCore
import IOSurface
import Metal

/// A 500 (two-plane 4:2:0, video range) texture of 64x512 over plane 0 of a 64x8 '420v' made by IOSurfaceCreate (pitch
/// 64, plane 1 at 512, 768 bytes, every byte i * 31 + 7 first), every texel read by a kernel, against a 64x512 '420v'
/// whose planes hold in their own rows the bytes the first one's rows reach: the surface's own bytes, then 0 to the end
/// of its last page and past it (gpu.gm20b.ycbcr_rows_past_page).
public enum YcbcrRowsPastPage {
    public static let width = 64, height = 512, rows = 8, chromaAt = 512, allocation = 768

    public struct Reading: Sendable {
        public var made = false
        public var rowsMatched = 0
        /// The first row not alike and its texel 0 both ways, "-" for none
        public var first = "-"
        public var paths: [String] = []
    }

    static func surface(width: Int, height: Int, chromaAt: Int, allocation: Int,
                        bytes: [UInt8]?) throws -> IOSurfaceRef {
        let planes: [[CFString: Any]] = [
            [
                kIOSurfacePlaneWidth: width, kIOSurfacePlaneHeight: height, kIOSurfacePlaneBytesPerElement: 1,
                kIOSurfacePlaneBytesPerRow: width, kIOSurfacePlaneOffset: 0, kIOSurfacePlaneSize: width * height,
            ],
            [
                kIOSurfacePlaneWidth: width / 2, kIOSurfacePlaneHeight: height / 2,
                kIOSurfacePlaneBytesPerElement: 2, kIOSurfacePlaneBytesPerRow: width, kIOSurfacePlaneOffset: chromaAt,
                kIOSurfacePlaneSize: width * height / 2,
            ],
        ]
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: width, kIOSurfaceHeight: height, kIOSurfacePixelFormat: OSType(0x3432_3076),
            kIOSurfaceAllocSize: allocation, kIOSurfacePlaneInfo: planes,
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { throw TestFailure("no surface") }
        IOSurfaceLock(surface, [], nil)
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        for i in 0 ..< IOSurfaceGetAllocSize(surface) {
            base[i] = bytes.map { i < $0.count ? $0[i] : 0 } ?? UInt8(truncatingIfNeeded: i * 31 + 7)
        }
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    /// The reference's bytes: luma row r is the source's bytes [64r, 64r + 64), chroma row j its [512 + 64j, ...)
    static func reference(_ source: [UInt8]) -> [UInt8] {
        let at = { (i: Int) in i < source.count ? source[i] : 0 }
        let luma = (0 ..< (width * height)).map(at)
        let chroma = (0 ..< (width * height / 2)).map { at(chromaAt + $0) }
        return luma + chroma
    }

    static func texture(_ gpu: GPU, over surface: IOSurfaceRef) -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 500)!, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        return gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
    }

    static func read(_ gpu: GPU, _ texture: MTLTexture) async throws -> ([SIMD4<Float>], String) {
        let out = try gpu.buffer(width * height * 16)
        let pipeline = try gpu.compute("read_float_texture")
        let timing = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: width, height: height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }
        let texels = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: width * height)
        return (Array(UnsafeBufferPointer(start: texels, count: width * height)), timing.facadePath)
    }

    public static func observe(_ gpu: GPU) async throws -> Reading {
        var reading = Reading()
        let source = try surface(width: width, height: rows, chromaAt: chromaAt, allocation: allocation, bytes: nil)
        guard let past = texture(gpu, over: source) else { return reading }
        reading.made = true
        let sourceBytes = YcbcrOnePlaneTargets.bytes(source)
        let mirror = try surface(
            width: width, height: height, chromaAt: width * height, allocation: width * height * 3 / 2,
            bytes: reference(sourceBytes)
        )
        guard let alike = texture(gpu, over: mirror) else { throw TestFailure("no texture over the reference surface") }
        let (got, path) = try await read(gpu, past)
        let (want, referencePath) = try await read(gpu, alike)
        reading.paths = [path, referencePath]
        for row in 0 ..< height {
            let range = (row * width) ..< ((row + 1) * width)
            if range.allSatisfy({ got[$0] == want[$0] }) {
                reading.rowsMatched += 1
            } else if reading.first == "-" {
                let show = { (v: SIMD4<Float>) in "\(v.x)/\(v.y)/\(v.z)" }
                reading.first = "row\(row):\(show(got[range.lowerBound]))_not_\(show(want[range.lowerBound]))"
            }
        }
        return reading
    }
}
