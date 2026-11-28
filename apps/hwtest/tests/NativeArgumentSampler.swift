import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// metal.native.argument_encoder's array draw reads row 1's repeating sampler as a clamp. This draw takes the array out: one
/// texture and one repeating sampler in an argument buffer. `gm20b/exact`: the repeat read the left texel, so the array's
/// stride or element addressing is the fault; `gm20b/clamp`: the right one, so an argument buffer's sampler is lowered or
/// its header made as a clamp whatever its address mode.
extension NativeArgumentEncoderTest {
    func drawOneSamplerThroughArguments(_ gpu: GPU) async throws -> String {
        guard let function = gpu.library.makeFunction(name: "argument_one_sampler_fragment")
        else { return "no_function" }
        let descriptor = MTLSamplerDescriptor()
        descriptor.sAddressMode = .repeat
        descriptor.supportArgumentBuffers = true
        guard let sampler = gpu.device.makeSamplerState(descriptor: descriptor) else { return "no_sampler" }
        let texels = Self.arrayTexels[0]
        let texture = try gpu.texture(.rgba8Unorm, 2, 1, usage: [.shaderRead], storage: .shared)
        texture.replace(region: MTLRegionMake2D(0, 0, 2, 1), mipmapLevel: 0, withBytes: texels, bytesPerRow: 8)
        let encoder = function.makeArgumentEncoder(bufferIndex: 0)
        let arguments = try gpu.buffer(max(encoder.encodedLength, 16))
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setTexture(texture, index: 0)
        encoder.setSamplerState(sampler, index: 1)
        let pipeline = try gpu.render(fragment: "argument_one_sampler_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 1, 1)
        let drawn = try await gpu.run { commands in
            let render = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
            render.useResource(texture, usage: .read, stages: .fragment)
            Self.cover(render, pipeline, arguments)
        }
        let pixel = Array(try await gpu.read(target, bytesPerPixel: 4).prefix(4))
        let read = pixel == Array(texels[0 ..< 4]) ? "exact" : pixel == Array(texels[4 ..< 8]) ? "clamp" : "wrong"
        return "\(drawn.path)/\(read)"
    }
}
