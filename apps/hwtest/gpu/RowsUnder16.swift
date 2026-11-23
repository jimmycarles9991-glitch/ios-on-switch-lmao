import Foundation
import HWTestCore
import IOSurface
import Metal

/// Planes of rows under 16 bytes in surfaces of 8 MiB + 16 KiB, as an M4 Max reads and stores them: reads a mebibyte a
/// row apart, stores 16 bytes a row apart. The surfaces are filled with one byte and compared by range, so only the
/// head where the stores land is compared byte by byte.
public enum RowsUnder16 {
    public typealias P = SurfaceReads.Plane
    public static let mebibyte = 1 << 20
    public static let allocation = 8 * mebibyte + 16384
    static let head = 16384

    /// A surface of `planes`, every byte `fill`
    public static func surface(
        _ fourcc: String, _ width: Int, _ height: Int, planes: [P], fill: UInt8
    ) -> IOSurfaceRef? {
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: width, kIOSurfaceHeight: height, kIOSurfaceAllocSize: allocation,
            kIOSurfacePixelFormat: fourcc.utf8.reduce(UInt32(0)) { $0 << 8 | UInt32($1) },
            kIOSurfacePlaneInfo: planes.map { p -> [CFString: Any] in
                [
                    kIOSurfacePlaneWidth: p.width, kIOSurfacePlaneHeight: p.height,
                    kIOSurfacePlaneBytesPerElement: p.bytesPerElement, kIOSurfacePlaneBytesPerRow: p.pitch,
                    kIOSurfacePlaneOffset: p.offset, kIOSurfacePlaneSize: p.pitch * p.height,
                ]
            },
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { return nil }
        edit(surface) { memset($0, Int32(fill), IOSurfaceGetAllocSize(surface)) }
        return surface
    }

    public static func edit(_ surface: IOSurfaceRef, _ body: (UnsafeMutablePointer<UInt8>) -> Void) {
        IOSurfaceLock(surface, [], nil)
        body(IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self))
        IOSurfaceUnlock(surface, [], nil)
    }

    /// The bytes at `offsets`
    public static func bytes(_ surface: IOSurfaceRef, at offsets: [Int]) -> [UInt8] {
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        return offsets.map { base[$0] }
    }

    /// Bytes of the first 16 KiB off `want` but those `ignoring`, plus bytes past it neither `fill` nor `except`'s
    public static func off(
        _ surface: IOSurfaceRef, _ want: [UInt8], fill: UInt8, ignoring: Range<Int> = 0 ..< 0,
        except: [Int: UInt8] = [:]
    ) -> Int {
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        var off = (0 ..< head).filter { !ignoring.contains($0) && base[$0] != want[$0] }.count
        off += except.filter { base[$0.key] != $0.value }.count
        let filled = [UInt8](repeating: fill, count: head)
        var at = head
        while at < allocation {
            let n = min(head, allocation - at)
            if memcmp(base + at, filled, n) != 0 {
                off += (at ..< at + n).filter { base[$0] != fill && except[$0] == nil }.count
            }
            at += n
        }
        return off
    }

    /// Writes `value` at `at` little-endian into `bytes`
    static func put(_ value: UInt32, _ bytes: inout [UInt8], at: Int) {
        for k in 0 ..< 4 { bytes[at + k] = UInt8(truncatingIfNeeded: value >> (8 * k)) }
    }

    /// One pass into `target`: cleared to `clear`, then the whole target drawn by `fragment` when one is named
    public static func pass(
        _ gpu: GPU, _ target: MTLTexture, clear: MTLClearColor, fragment: String?, load: Bool = false
    ) async throws -> String {
        let pipeline = try fragment.map { try gpu.render(fragment: $0, format: target.pixelFormat) }
        return try await gpu.run(tolerateError: true) { commands in
            let descriptor = MTLRenderPassDescriptor()
            descriptor.colorAttachments[0].texture = target
            descriptor.colorAttachments[0].loadAction = load ? .load : .clear
            descriptor.colorAttachments[0].clearColor = clear
            descriptor.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
            if let pipeline {
                var corners = GPU.fullScreen
                encoder.setRenderPipelineState(pipeline)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }.facadePath
    }

    /// A kernel `name` over every texel of `target`
    public static func dispatch(_ gpu: GPU, _ name: String, _ target: MTLTexture) async throws -> String {
        let pipeline = try gpu.compute(name)
        return try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(target, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: target.width, height: target.height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }.facadePath
    }

    /// One blit encoder's `encode` in a command buffer of its own; its facade path
    public static func blitPath(_ gpu: GPU, _ encode: (MTLBlitCommandEncoder) -> Void) async throws -> String {
        try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            encode(blit)
            blit.endEncoding()
        }.facadePath
    }
}
