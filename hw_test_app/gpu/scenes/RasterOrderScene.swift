import Foundation
import HWTestCore
import Metal

/// Raster order groups: one instanced draw of full-target quads, each fragment folding its instance into its pixel's
/// word through a read-write texture in raster order group 0 (word * 3 + instance + 1), so a word is the CPU's only if
/// every pixel's fragments ran one at a time in submission order. On the raw device the answer is reported beside it.
struct RasterOrderScene: GPUScene {
    let id = "gpu.raw.raster_order"
    let name = "Raster order group: ordered read-modify-write of every pixel by 48 overlapping quads"

    static let size = 64
    static let quads = 48

    static var expected: UInt32 { (0..<UInt32(quads)).reduce(0) { $0 &* 3 &+ $1 &+ 1 } }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let words = try gpu.texture(.r32Uint, Self.size, Self.size, usage: [.shaderRead, .shaderWrite])
        let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let pipeline = try gpu.render(
            vertex: "raster_order_vertex", fragment: "raster_order_fragment", format: .rgba8Unorm
        ) { $0.colorAttachments[0].writeMask = [] }
        let zeros = try gpu.buffer(Self.size * Self.size * 4)
        memset(zeros.contents(), 0, zeros.length)
        let timing = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: zeros, sourceOffset: 0, sourceBytesPerRow: Self.size * 4, sourceBytesPerImage: 0,
                sourceSize: MTLSize(width: Self.size, height: Self.size, depth: 1), to: words, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .dontCare
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setFragmentTexture(words, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: Self.quads)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(words, bytesPerPixel: 4)
        let got = bytes.withUnsafeBytes { Array($0.bindMemory(to: UInt32.self)) }
        let wrong = got.filter { $0 != Self.expected }.count
        result.metrics.set("path", timing.path)
        result.metrics.set("wrong_pixels", wrong)
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(timing.path == "raw", "the ordered draw left raw: \(timing.path)")
            let object = gpu.device as AnyObject as! NSObject
            let answer = (object.value(forKey: "areRasterOrderGroupsSupported") as? NSNumber)?.intValue ?? -1
            result.metrics.set("answer", answer)
            result.expect(answer == 1, "areRasterOrderGroupsSupported answers \(answer)")
        }
        let first = got.first { $0 != Self.expected } ?? 0
        result.expect(wrong == 0, "\(wrong) of \(got.count) words wrong, first \(first), want \(Self.expected)")
        return result
    }
}
