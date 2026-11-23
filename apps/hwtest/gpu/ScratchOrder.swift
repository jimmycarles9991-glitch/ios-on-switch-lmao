import Foundation
import HWTestCore
import Metal

/// gpu.gm20b.scratch_order's passes: two passes of different sizes over a memoryless colour 1 of one format, which the
/// raw path backs with the same scratch memory and opens one after the other with no drain between them. Pass A
/// (722x1282) writes a seed's pattern into colour 1, then a second draw fetches colour 1 into its stored colour 0;
/// pass B (1792x1472) clears colour 1, and its first draw fetches it into its stored colour 0.
public enum ScratchOrder {
    public static let a = (width: 722, height: 1282), b = (width: 1792, height: 1472)
    public static let clear = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)

    public struct Read: Sendable {
        /// Each round's colour 0 bytes of pass A and of pass B (BGRA8)
        public var a: [[UInt8]] = [], b: [[UInt8]] = []
        public var paths: [String] = []
    }

    /// `rounds` command buffers of A then B, the seed the round's
    public static func run(_ gpu: GPU, scratch format: MTLPixelFormat, rounds: Int) async throws -> Read {
        func state(_ fragment: String) throws -> MTLRenderPipelineState {
            try gpu.render(vertex: "bands_vertex", fragment: fragment, format: .bgra8Unorm) {
                $0.colorAttachments[1].pixelFormat = format
            }
        }
        let write = try state("scratch_write_fragment"), fetch = try state("scratch_fetch_fragment")
        let scratchA = try gpu.texture(format, a.width, a.height, usage: [.renderTarget], storage: .memoryless)
        let scratchB = try gpu.texture(format, b.width, b.height, usage: [.renderTarget], storage: .memoryless)
        var out = Read()
        for round in 0 ..< rounds {
            let targetA = try gpu.texture(.bgra8Unorm, a.width, a.height)
            let targetB = try gpu.texture(.bgra8Unorm, b.width, b.height)
            out.paths.append(
                try await gpu.run { commands in
                    var seed = UInt32(round + 1)
                    let first = commands.makeRenderCommandEncoder(descriptor: pass(targetA, scratchA, clear: nil))!
                    first.setRenderPipelineState(write)
                    first.setFragmentBytes(&seed, length: 4, index: 0)
                    first.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    first.setRenderPipelineState(fetch)
                    first.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    first.endEncoding()
                    let second = commands.makeRenderCommandEncoder(descriptor: pass(targetB, scratchB, clear: clear))!
                    second.setRenderPipelineState(fetch)
                    second.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    second.endEncoding()
                }.path
            )
            out.a.append(try await gpu.read(targetA, bytesPerPixel: 4))
            out.b.append(try await gpu.read(targetB, bytesPerPixel: 4))
        }
        return out
    }

    static func pass(_ target: MTLTexture, _ scratch: MTLTexture, clear: MTLClearColor?) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .dontCare
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[1].texture = scratch
        pass.colorAttachments[1].loadAction = clear == nil ? .dontCare : .clear
        pass.colorAttachments[1].clearColor = clear ?? MTLClearColor()
        pass.colorAttachments[1].storeAction = .dontCare
        return pass
    }

    /// Pass A's colour 0 as the CPU computes the shader's pattern for `seed` (BGRA8 bytes)
    public static func pattern(seed: UInt32) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: a.width * a.height * 4)
        for y in 0 ..< a.height {
            for x in 0 ..< a.width {
                let h = (UInt32(x) &* 73_856_093) ^ (UInt32(y) &* 19_349_663) ^ (seed &* 83_492_791)
                let at = (y * a.width + x) * 4
                out[at] = UInt8((h >> 16) & 255)
                out[at + 1] = UInt8((h >> 8) & 255)
                out[at + 2] = UInt8(h & 255)
                out[at + 3] = 255
            }
        }
        return out
    }
}
