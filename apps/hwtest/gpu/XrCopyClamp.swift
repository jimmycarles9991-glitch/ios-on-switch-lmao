import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// GPU copies into and out of a Shared BGRA10_XR (552) or BGRA10_XR_sRGB (553) texture, usage ShaderRead, of 16-bit
/// fields whose low 6 bits are set (gpu.gm20b.xr_copy_clamp): field i's B (500 + i % 7) << 6 | i % 64, G (300 + i % 5) << 6 |
/// 3i % 64, R (900 + i % 3) << 6 | 5i % 64, A (i % 1024) << 6 | 7i % 64.
public enum XrCopyClamp {
    public enum Route: String, Sendable, CaseIterable {
        /// a buffer blitted into the texture, read back with getBytes
        case bufferIn = "buffer_in"
        /// replaceRegion into the texture, blitted out into a buffer
        case blitOut = "blit_out"
    }

    public struct Arm: Sendable {
        public let format: UInt
        public let optimized: Bool
        public let size: Int
        public let route: Route

        public var key: String {
            "\(format)_\(route.rawValue)_\(optimized ? "opt" : "plain")\(size == 64 ? "" : "_\(size)")" }

        public init(format: UInt, optimized: Bool, size: Int, route: Route) {
            (self.format, self.optimized, self.size, self.route) = (format, optimized, size, route)
        }
    }

    public struct Reading: Sendable {
        /// Fields not as a GPU-optimized texture keeps them (codes, the alpha code clamped to 384..894, low bits 0)
        public var offClamped = 0
        /// Fields not kept bit for bit
        public var offExact = 0
        /// blit_out: the texture's own fields (getBytes) not kept bit for bit afterwards
        public var sourceChanged = 0
        public var path = "-"
    }

    public static func fields(_ count: Int) -> [UInt16] {
        (0 ..< count).flatMap { i -> [UInt16] in
            [
                UInt16((500 + i % 7) << 6 | i % 64), UInt16((300 + i % 5) << 6 | 3 * i % 64),
                UInt16((900 + i % 3) << 6 | 5 * i % 64), UInt16((i % 1024) << 6 | 7 * i % 64),
            ]
        }
    }

    static func clamped(_ field: UInt16, channel: Int) -> UInt16 {
        let code = Int(field >> 6)
        return UInt16(channel == 3 ? min(max(code, 384), 894) : code) << 6
    }

    public static func observe(_ gpu: GPU, _ arm: Arm) async throws -> Reading {
        let n = arm.size * arm.size, row = arm.size * 8
        let input = fields(n)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: arm.format)!, width: arm.size, height: arm.size, mipmapped: false
        )
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        descriptor.allowGPUOptimizedContents = arm.optimized
        guard let texture = gpu.device.makeTexture(descriptor: descriptor)
        else { throw TestFailure("no \(arm.key) texture") }
        let source = try gpu.buffer(input), out = try gpu.buffer(n * 8)
        let region = MTLRegionMake2D(0, 0, arm.size, arm.size), size = MTLSize(
            width: arm.size,
            height: arm.size,
            depth: 1
        )
        if arm.route == .blitOut { input.withUnsafeBytes { texture.replace(
            region: region,
            mipmapLevel: 0,
            withBytes: $0.baseAddress!,
            bytesPerRow: row
        ) } }
        let timing = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            if arm.route == .bufferIn {
                blit.copy(
                    from: source, sourceOffset: 0, sourceBytesPerRow: row, sourceBytesPerImage: n * 8, sourceSize: size,
                    to: texture, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin()
                )
            } else {
                blit.copy(
                    from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(), sourceSize: size, to: out,
                    destinationOffset: 0, destinationBytesPerRow: row, destinationBytesPerImage: n * 8
                )
            }
            blit.endEncoding()
        }
        var reading = Reading(path: timing.facadePath)
        guard !timing.path.hasPrefix("error:") else { throw TestFailure("\(arm.key): failed \(timing.facadePath)") }
        var kept = [UInt16](repeating: 0, count: 4 * n)
        kept
            .withUnsafeMutableBytes { texture.getBytes($0.baseAddress!, bytesPerRow: row, from: region, mipmapLevel: 0)
            }
        let got = arm.route == .bufferIn
            ? kept : Array(UnsafeBufferPointer(
                start: out.contents().bindMemory(to: UInt16.self, capacity: 4 * n),
                count: 4 * n
            ))
        for i in 0 ..< (4 * n) {
            if got[i] != clamped(input[i], channel: i % 4) { reading.offClamped += 1 }
            if got[i] != input[i] { reading.offExact += 1 }
            if arm.route == .blitOut && kept[i] != input[i] { reading.sourceChanged += 1 }
        }
        return reading
    }
}
