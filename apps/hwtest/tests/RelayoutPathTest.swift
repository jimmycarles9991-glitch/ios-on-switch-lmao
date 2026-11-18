import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A render target nothing wrote is made block-linear without moving its bytes, and when its texture goes it stays
/// block-linear until a CPU lock asks for rows (drivers/TegraGM20B/TegraGM20BTiled.cpp decide,
/// TegraGM20BCoherency.cpp drop, TegraGM20BViews.cpp): the GPU blits a pattern into a fresh IOSurface, the texture goes,
/// and a read-only lock must read the pattern as rows. A surface the CPU wrote first must still be tiled by a move: the
/// GPU blits over its top half only, and its lock must read the GPU's rows above the CPU's. A pattern that is not uniform
/// shows a skipped tiling move (the CPU's rows read back scrambled) and a skipped untile (texels read as rows).
struct RelayoutPathTest: GPUPathTest {
    let id = "gpu.path.relayout_history"
    let name = "A fresh render target is tiled without a move and untiled at its first CPU lock; a written one moves"
    let counters = ["relayout_fresh", "relayout_deferred", "relayout_lazy"]

    static let width = 128, height = 128, rowBytes = 512

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        var failures: [String] = []
        let drawn = Self.pattern(1)
        let fresh = try Self.surface()
        try await Self.blit(gpu, drawn, into: fresh, rows: Self.height)
        let freshWrong = Self.wrong(Self.read(fresh), drawn)
        metrics.set("fresh_wrong_bytes", freshWrong)
        if freshWrong != 0 { failures.append("\(freshWrong) bytes of the fresh target read back wrong after it went") }

        let connection = GPUConnection()
        let before = GPUPaths.read(connection)
        let cpu = Self.pattern(2)
        let written = try Self.surface()
        written.lock(options: [], seed: nil)
        cpu.withUnsafeBytes { written.baseAddress.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
        written.unlock(options: [], seed: nil)
        let top = Self.pattern(3)
        try await Self.blit(gpu, top, into: written, rows: Self.height / 2)
        let half = Self.height / 2 * Self.rowBytes
        let want = Array(top[..<half]) + Array(cpu[half...])
        let writtenWrong = Self.wrong(Self.read(written), want)
        metrics.set("written_wrong_bytes", writtenWrong)
        if writtenWrong != 0 { failures.append("\(writtenWrong) bytes of the CPU-written target read back wrong") }
        if case .counts(let start) = before, case .counts(let end) = GPUPaths.read(connection) {
            let skipped = end.taken("relayout_fresh", since: start)
            metrics.set("written_fresh", skipped)
            if skipped != 0 { failures.append("a surface the CPU wrote first was tiled without a move") }
        }
        return failures
    }

    static func pattern(_ seed: Int) -> [UInt8] {
        (0..<(rowBytes * height)).map { UInt8(truncatingIfNeeded: ($0 &* 7) ^ ($0 >> 9) &+ seed &* 61) }
    }

    static func surface() throws -> IOSurface {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .bytesPerElement: 4, .bytesPerRow: rowBytes,
            .allocSize: rowBytes * height, .pixelFormat: kCVPixelFormatType_32BGRA,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface not made") }
        return surface
    }

    /// The bytes' first `rows` rows into the surface's texture, which goes before this returns: its import's release.
    static func blit(_ gpu: GPU, _ bytes: [UInt8], into surface: IOSurface, rows: Int) async throws {
        let source = try gpu.buffer(bytes)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard
            let target = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { throw TestFailure("makeTexture(iosurface:) returned nil") }
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: source, sourceOffset: 0, sourceBytesPerRow: rowBytes, sourceBytesPerImage: rowBytes * rows,
                sourceSize: MTLSize(width: width, height: rows, depth: 1), to: target, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            blit.endEncoding()
        }
    }

    static func read(_ surface: IOSurface) -> [UInt8] {
        surface.lock(options: .readOnly, seed: nil)
        let bytes = Array(UnsafeRawBufferPointer(start: surface.baseAddress, count: rowBytes * height))
        surface.unlock(options: .readOnly, seed: nil)
        return bytes
    }
}
