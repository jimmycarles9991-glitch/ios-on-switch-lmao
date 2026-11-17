import Foundation
import HWTestCore
import Metal

/// An RGBA8 texture over a buffer at offset 48 with rows of 48 bytes (16 mod 32), which the texture header cannot address:
/// raw samples it through a pool image of its texels (raw/runtime/raw_gpu.cpp createBufferTexture), where it once refused
/// to make it and m2v held it. Read by a kernel, again after a blit fills the buffer's bytes in the same command buffer,
/// and by two passes with a blit between them, each of which must see the bytes as they are then.
struct BufferTextureRowsScene: GPUScene {
    let id = "gpu.raw.buffer_texture_rows16"
    let name = "Textures over a buffer whose rows and start are 16 bytes off 32"

    static let (width, height, row, start) = (12, 8, 48, 48)
    static let corners: [Float] = [-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (w, h) = (Self.width, Self.height)
        let source = try gpu.buffer(Self.start + Self.row * h)
        let bytes = source.contents().assumingMemoryBound(to: UInt8.self)
        for i in 0..<(Self.row * h) { bytes[Self.start + i] = UInt8((i * 37 + 11) & 255) }
        let pattern = (0..<(w * h * 4)).map { bytes[Self.start + ($0 / (w * 4)) * Self.row + $0 % (w * 4)] }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let texture = source.makeTexture(descriptor: descriptor, offset: Self.start, bytesPerRow: Self.row) else {
            throw TestFailure("makeTexture over the buffer returned nil")
        }
        let kernel = try gpu.compute("sample_texture")
        let render = MTLRenderPipelineDescriptor()
        render.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        render.fragmentFunction = gpu.library.makeFunction(name: "texel_fragment")
        render.colorAttachments[0].pixelFormat = .rgba8Unorm
        let pipeline = try await gpu.device.makeRenderPipelineState(descriptor: render)
        let corners = try gpu.buffer(Self.corners)
        let sampled = [try gpu.buffer(w * h * 16), try gpu.buffer(w * h * 16)]
        let targets = [try gpu.texture(.rgba8Unorm, w, h), try gpu.texture(.rgba8Unorm, w, h)]
        let drawn = [try gpu.buffer(w * h * 4), try gpu.buffer(w * h * 4)]
        let fills: [UInt8] = [0x40, 0x9c]
        let range = Self.start..<(Self.start + Self.row * h)
        let timing = try await gpu.run { commands in
            for (i, out) in sampled.enumerated() {
                if i == 1 {
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.fill(buffer: source, range: range, value: fills[0])
                    blit.endEncoding()
                }
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(kernel)
                encoder.setTexture(texture, index: 0)
                encoder.setBuffer(out, offset: 0, index: 0)
                encoder.dispatchThreads(
                    MTLSize(width: w, height: h, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 4, height: 4, depth: 1))
                encoder.endEncoding()
            }
            for (i, target) in targets.enumerated() {
                if i == 1 {
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.fill(buffer: source, range: range, value: fills[1])
                    blit.endEncoding()
                }
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                encoder.setVertexBuffer(corners, offset: 0, index: 0)
                encoder.setFragmentTexture(texture, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: w, height: h, depth: 1), to: drawn[i], destinationOffset: 0,
                    destinationBytesPerRow: w * 4, destinationBytesPerImage: w * h * 4)
                blit.endEncoding()
            }
        }
        let expected: [[UInt8]] = [pattern, [UInt8](repeating: fills[0], count: pattern.count)]
        for (i, out) in sampled.enumerated() {
            let texels = out.contents().assumingMemoryBound(to: Float.self)
            let wrong = (0..<pattern.count).filter { UInt8((texels[$0] * 255).rounded()) != expected[i][$0] }.count
            result.metrics.set("kernel\(i)_wrong", wrong)
            result.expect(wrong == 0, "kernel read \(i): \(wrong) texel channels wrong")
        }
        let passExpected: [[UInt8]] = [expected[1], [UInt8](repeating: fills[1], count: pattern.count)]
        for (i, out) in drawn.enumerated() {
            let pixels = out.contents().assumingMemoryBound(to: UInt8.self)
            let wrong = (0..<pattern.count).filter { pixels[$0] != passExpected[i][$0] }.count
            result.metrics.set("pass\(i)_wrong", wrong)
            result.expect(wrong == 0, "pass \(i): \(wrong) pixel channels wrong")
        }
        result.metrics.set("path", timing.path)
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(timing.path == "raw", "the command buffer took \(timing.path), not raw")
        }
        return result
    }
}
