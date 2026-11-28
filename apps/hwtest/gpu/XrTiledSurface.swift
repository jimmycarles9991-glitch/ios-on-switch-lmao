import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A 550 over a planeless 'w30r' 64x64 (rows 256) that a BGR10_XR (554) render target over it was cleared in first (the
/// device's kernel lays such a surface out block-linear for that target) (gpu.gm20b.xr_tiled_surface), against an M4 Max
public enum XrTiledSurface {
    public struct Reading: Sendable {
        public var made = false
        public var readOff = -1, storeOff = -1, cpuOff = -1, blitOff = -1
        /// The first texel the kernel read: zeros name a pool texture never filled from the blocks
        public var first = "-"
    }

    static let size = 64

    /// The surface, its 554 cleared to (0.25, 0.5, 0.75, 1), and a 550 over it
    static func tiled(_ gpu: GPU) async throws -> (MTLTexture, MTLTexture?) {
        guard let surface = SurfaceReads.surface(
            "w30r",
            size,
            size,
            rows: 256,
            allocation: 256 * size,
            fill: { _ in 0 }
        ),
              let target = SurfaceReads.texture(
                  gpu,
                  554,
                  size,
                  size,
                  over: surface,
                  usage: [.renderTarget, .shaderRead]
              )
        else { throw TestFailure("no 554 over a 'w30r'") }
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        guard !timing.path.hasPrefix("error:") else { throw TestFailure("the 554 clear failed: \(timing.facadePath)") }
        return (
            target,
            SurfaceReads.texture(gpu, 550, size, size, over: surface, usage: [.shaderRead, .shaderWrite, .renderTarget])
        )
    }

    static func close(_ a: SIMD4<Float>,
                      _ b: SIMD4<Float>) -> Bool { (0 ..< 4).allSatisfy { abs(a[$0] - b[$0]) <= 1e-6 } }

    public static func observe(_ gpu: GPU) async throws -> Reading {
        var reading = Reading()
        let (target, made) = try await tiled(gpu)
        guard let xr = made else { return reading }
        reading.made = true
        let cleared = try await SurfaceReads.read(gpu, xr)
        let clearWant = SIMD4<Float>(128.0 / 510, 0.5, 382.0 / 510, 0)
        reading.readOff = cleared.filter { !close($0, clearWant) }.count
        if let texel = cleared.first { reading.first = "\(texel.x),\(texel.y),\(texel.z),\(texel.w)" }
        let path = try await SurfaceReads.fill(gpu, xr, SIMD4(0.5, 0.25, -0.25, 0.75))
        guard !path.hasPrefix("error:") else { throw TestFailure("the 550 store failed: \(path)") }
        let stored = try await SurfaceReads.read(gpu, target)
        reading.storeOff = stored.filter { !close($0, SurfaceReads.xr(0x27f8_0100, alpha: 1)) }.count
        let region = MTLRegionMake2D(5, 3, 1, 1)
        var word: UInt32 = 0
        xr.getBytes(&word, bytesPerRow: 4, from: region, mipmapLevel: 0)
        var replaced: UInt32 = 0x2abc_def1, back: UInt32 = 0
        xr.replace(region: region, mipmapLevel: 0, withBytes: &replaced, bytesPerRow: 4)
        xr.getBytes(&back, bytesPerRow: 4, from: region, mipmapLevel: 0)
        let after = try await SurfaceReads.read(gpu, target)
        reading.cpuOff = (word == 0x27f8_0100 ? 0 : 1) + (back == replaced ? 0 : 1)
            + (close(after[3 * size + 5], SurfaceReads.xr(replaced, alpha: 1)) ? 0 : 1)
        let (secondTarget, secondMade) = try await tiled(gpu)
        guard let second = secondMade else { throw TestFailure("no second 550") }
        let blit = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeBlitCommandEncoder()!
            encoder.copy(
                from: xr, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 8, y: 0, z: 0),
                sourceSize: MTLSize(width: 16, height: 8, depth: 1), to: second, destinationSlice: 0,
                destinationLevel: 0,
                destinationOrigin: MTLOrigin()
            )
            encoder.endEncoding()
        }
        guard !blit.path.hasPrefix("error:") else { throw TestFailure("the blit failed: \(blit.facadePath)") }
        let copied = try await SurfaceReads.read(gpu, secondTarget)
        reading.blitOff = (0 ..< 8).flatMap { y in (0 ..< 16).map { (y, $0) } }
            .filter { !close(copied[$0.0 * size + $0.1], after[$0.0 * size + 8 + $0.1]) }.count
        return reading
    }
}
