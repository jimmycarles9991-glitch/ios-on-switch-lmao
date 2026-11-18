import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// metal.native.argument_encoder's draws of a texture array, a sampler array and an argument buffer behind a pointer, each
/// encoded through the fragment function's own encoder (its buffer index form, and the nested encoder it answers)
extension NativeArgumentEncoderTest {
    static let arrayTexels: [[UInt8]] = [
        [255, 0, 0, 255, 0, 255, 0, 255], [0, 0, 255, 255, 255, 255, 0, 255], [255, 0, 255, 255, 0, 255, 255, 255],
    ]

    static func pass(_ target: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        return pass
    }

    static func cover(_ render: MTLRenderCommandEncoder, _ pipeline: MTLRenderPipelineState, _ arguments: MTLBuffer) {
        render.setRenderPipelineState(pipeline)
        var corners = GPU.fullScreen
        render.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        render.setFragmentBuffer(arguments, offset: 0, index: 0)
        render.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        render.endEncoding()
    }

    /// Where a pass that samples an argument buffer's three textures through its two samplers went, and whether each pixel
    /// read its own texture through its row's sampler (the clamp a texture's right texel, the repeat its left)
    func drawArraysThroughArguments(_ gpu: GPU) async throws -> String {
        guard let function = gpu.library.makeFunction(name: "argument_array_fragment") else { return "no_function" }
        let samplers = [MTLSamplerAddressMode.clampToEdge, .repeat].compactMap { mode -> MTLSamplerState? in
            let descriptor = MTLSamplerDescriptor()
            descriptor.sAddressMode = mode
            return gpu.device.makeSamplerState(descriptor: descriptor)
        }
        guard samplers.count == 2 else { return "no_sampler" }
        let encoder = function.makeArgumentEncoder(bufferIndex: 0)
        let textures = try Self.arrayTexels.map { texels in
            let texture = try gpu.texture(.rgba8Unorm, 2, 1, usage: [.shaderRead], storage: .shared)
            texture.replace(region: MTLRegionMake2D(0, 0, 2, 1), mipmapLevel: 0, withBytes: texels, bytesPerRow: 8)
            return texture
        }
        let arguments = try gpu.buffer(max(encoder.encodedLength, 16))
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setTextures(textures, range: 0..<3)
        encoder.setSamplerStates(samplers, range: 3..<5)
        let pipeline = try gpu.render(fragment: "argument_array_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 3, 2)
        let drawn = try await gpu.run { commands in
            let render = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
            render.useResources(textures.map { $0 as MTLResource }, usage: .read, stages: .fragment)
            Self.cover(render, pipeline, arguments)
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let exact = (0..<3).allSatisfy { k in
            Array(pixels.pixel(k, 0, width: 3)) == Array(Self.arrayTexels[k][4..<8])
                && Array(pixels.pixel(k, 1, width: 3)) == Array(Self.arrayTexels[k][0..<4])
        }
        return "\(drawn.path)/\(exact ? "exact" : "wrong")"
    }

    /// Where a pass that reads a colour through one pointer that is a bound buffer at one pixel and an argument buffer's
    /// pointer at the other went (m2v reaches the slot through the draw's root table), and whether it drew both
    func drawSlotThroughArguments(_ gpu: GPU) async throws -> String {
        guard let encoder = gpu.device.makeArgumentEncoder(arguments: [Self.argument(.pointer, 0)]) else {
            return "no_encoder"
        }
        let colour = try gpu.buffer([SIMD4<Float>(1, 0, 1, 1)]), arguments = try gpu.buffer(16)
        let direct = try gpu.buffer([SIMD4<Float>(1, 1, 1, 1), SIMD4<Float>(1, 1, 1, 1), SIMD4<Float>(0, 0, 1, 1)])
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setBuffer(colour, offset: 0, index: 0)
        let pipeline = try gpu.render(fragment: "argument_slot_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 2, 1)
        let drawn = try await gpu.run { commands in
            let render = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
            render.useResource(colour, usage: .read, stages: .fragment)
            render.setFragmentBuffer(direct, offset: 32, index: 1)
            Self.cover(render, pipeline, arguments)
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let exact =
            Array(pixels.pixel(0, 0, width: 2)) == [0, 0, 255, 255]
            && Array(pixels.pixel(1, 0, width: 2)) == [255, 0, 255, 255]
        return "\(drawn.path)/\(exact ? "exact" : "wrong")"
    }

    /// Where a pass that reads a texture and a colour through an argument buffer behind an argument buffer's pointer went,
    /// the inner one written through the nested encoder the function's encoder answers, and whether it drew both
    func drawNestedThroughArguments(_ gpu: GPU, _ metrics: inout Metrics) async throws -> String {
        guard let function = gpu.library.makeFunction(name: "argument_nested_fragment") else { return "no_function" }
        let outer = function.makeArgumentEncoder(bufferIndex: 0)
        guard let inner = outer.makeArgumentEncoderForBuffer(atIndex: 0) else { return "no_nested_encoder" }
        metrics.set("nested", "\(type(of: inner)):\(inner.encodedLength)/\(inner.alignment)")
        let texture = try gpu.texture(.rgba8Unorm, 4, 4)
        let colour = try gpu.buffer([SIMD4<Float>(0, 1, 1, 1)])
        let outerBuffer = try gpu.buffer(max(outer.encodedLength, 16))
        let innerBuffer = try gpu.buffer(max(inner.encodedLength, 16))
        outer.setArgumentBuffer(outerBuffer, offset: 0)
        outer.setBuffer(innerBuffer, offset: 0, index: 0)
        inner.setArgumentBuffer(innerBuffer, offset: 0)
        inner.setTexture(texture, index: 0)
        inner.setBuffer(colour, offset: 0, index: 1)
        let pipeline = try gpu.render(fragment: "argument_nested_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 2, 1)
        let drawn = try await gpu.run { commands in
            let clear = Self.pass(texture)
            clear.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 1, blue: 0, alpha: 1)
            commands.makeRenderCommandEncoder(descriptor: clear)!.endEncoding()
            let render = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
            render.useResources([innerBuffer, texture, colour] as [MTLResource], usage: .read, stages: .fragment)
            Self.cover(render, pipeline, outerBuffer)
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let exact =
            Array(pixels.pixel(0, 0, width: 2)) == [255, 255, 0, 255]
            && Array(pixels.pixel(1, 0, width: 2)) == [0, 255, 255, 255]
        return "\(drawn.path)/\(exact ? "exact" : "wrong")"
    }
}
