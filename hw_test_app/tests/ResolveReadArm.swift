import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.resolve_twod's read arm: a resolve whose target a blit copies out later in the same command buffer, which the
/// rules see when the tape is encoded and send through the complement (resolve.exact, raw_resolve_rule.h), against
/// Metal's rule computed here from the samples resolve_twod_fragment shades: (sum + n/2) / n per channel.
extension ResolveTwodTest {
    static func sampleHash(_ x: Int, _ y: Int, _ sample: Int) -> UInt32 {
        var h = (UInt32(x) &* 73_856_093) ^ (UInt32(y) &* 19_349_663) ^ (UInt32(sample + 1) &* 83_492_791)
        h ^= h >> 13
        h = h &* 0x5bd1_e995
        h ^= h >> 15
        return h
    }

    /// Metal's resolve of the pattern, BGRA8 rows (the fragment's r, g, b, a are the hash's bytes 0, 1, 2, 3)
    static func reference(samples: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: width * height * 4)
        let n = UInt32(samples), half = UInt32(samples / 2)
        out.withUnsafeMutableBufferPointer { o in
            for y in 0..<height {
                for x in 0..<width {
                    var b: UInt32 = 0, g: UInt32 = 0, r: UInt32 = 0, a: UInt32 = 0
                    for s in 0..<samples {
                        let h = sampleHash(x, y, s)
                        r += h & 255
                        g += (h >> 8) & 255
                        b += (h >> 16) & 255
                        a += h >> 24
                    }
                    let at = (y * width + x) * 4
                    o[at] = UInt8((b + half) / n)
                    o[at + 1] = UInt8((g + half) / n)
                    o[at + 2] = UInt8((r + half) / n)
                    o[at + 3] = UInt8((a + half) / n)
                }
            }
        }
        return out
    }

    /// The pass and its resolve, then a blit of the resolve target into a buffer, in one command buffer
    func readInCommandBuffer(_ gpu: GPU, _ state: MTLRenderPipelineState, samples: Int) async throws -> Resolved {
        let ms = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget], samples: samples)
        let into = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        let row = Self.width * 4
        let out = try gpu.buffer(row * Self.height)
        let before = RawFacade.counters(of: gpu.device)
        let timing = try await gpu.run { commands in
            draw(commands, pass(ms, into: into, load: .clear, store: .multisampleResolve), state)
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: into, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: Self.width, height: Self.height, depth: 1), to: out,
                destinationOffset: 0, destinationBytesPerRow: row, destinationBytesPerImage: row * Self.height)
            blit.endEncoding()
        }
        let after = RawFacade.counters(of: gpu.device)
        var read = Resolved()
        read.gpuMs = timing.gpuMilliseconds
        read.path = timing.path
        read.twod = (after["resolve_twod"] ?? 0) - (before["resolve_twod"] ?? 0)
        read.exact = (after["resolve_exact"] ?? 0) - (before["resolve_exact"] ?? 0)
        read.bytes = Array(UnsafeRawBufferPointer(start: out.contents(), count: row * Self.height))
        return read
    }
}
