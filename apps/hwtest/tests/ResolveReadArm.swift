import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.resolve_twod's read arm: a resolve whose target a blit copies out later in the same command buffer, which
/// the rules see when the tape is encoded and send to the pack's resolve pass (resolve_rule.h), against Metal's
/// rule computed here from the samples resolve_twod_fragment shades: (sum + n/2) / n per channel.
extension ResolveTwodTest {
    static func sampleHash(_ x: Int, _ y: Int, _ sample: Int) -> UInt32 {
        ResolveRef.sampleHash(x, y, sample)
    }

    /// Metal's resolve of the pattern, BGRA8 rows (the fragment's r, g, b, a are the hash's bytes 0, 1, 2, 3)
    static func reference(samples: Int) -> [UInt8] {
        ResolveRef.reference(width: width, height: height, samples: samples)
    }

    /// The 2D engine's 2x box of the pattern and its complement as resolve_rule.h reads the chip, BGRA8 rows
    static func engineReading() -> (box: [UInt8], complement: [UInt8]) {
        ResolveRef.engineReading(width: width, height: height)
    }

    /// The pass and its resolve, then a blit of the resolve target into a buffer, in one command buffer
    func readInCommandBuffer(
        _ gpu: GPU, _ state: MTLRenderPipelineState, samples: Int, format: MTLPixelFormat = .bgra8Unorm,
        bytesPerTexel: Int = 4
    ) async throws -> Resolved {
        let ms = try gpu.texture(format, Self.width, Self.height, usage: [.renderTarget], samples: samples)
        let into = try gpu.texture(format, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        let row = Self.width * bytesPerTexel
        let out = try gpu.buffer(row * Self.height)
        let before = GM20BFacade.counters(of: gpu.device)
        let timing = try await gpu.run { commands in
            draw(commands, pass(ms, into: into, load: .clear, store: .multisampleResolve), state)
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: into, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: Self.width, height: Self.height, depth: 1), to: out,
                destinationOffset: 0, destinationBytesPerRow: row, destinationBytesPerImage: row * Self.height
            )
            blit.endEncoding()
        }
        let after = GM20BFacade.counters(of: gpu.device)
        var read = Resolved()
        read.gpuMs = timing.gpuMilliseconds
        read.path = timing.path
        read.twod = (after["resolve_twod"] ?? 0) - (before["resolve_twod"] ?? 0)
        read.exact = (after["resolve_exact"] ?? 0) - (before["resolve_exact"] ?? 0)
        read.bytes = Array(UnsafeRawBufferPointer(start: out.contents(), count: row * Self.height))
        return read
    }
}
