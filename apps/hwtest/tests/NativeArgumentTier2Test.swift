import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Tier-2 argument buffers (NativeAnswers.argumentBuffersTier, both builds): argumentBuffersSupport answers Tier2 and
/// 500000 indirect textures and samplers, and a buffer the app writes itself, with no encoder (a texture's and a
/// sampler's gpuResourceID at the fragment function's argument places, 0, 8 and 16), draws two textures through one
/// sampler: the left half the first texture's colour, the right half the second's, through the device's handle tables.
struct NativeArgumentTier2Test: NativeObjectTest {
    let id = "metal.native.argument_tier2"
    let name = "Resource IDs written straight into an argument buffer read through the handle tables on the raw path"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let tier = Int(device.argumentBuffersSupport.rawValue) + 1
        metrics.set("argument_buffers_tier", tier)
        if tier != NativeAnswers.argumentBuffersTier {
            work.failures.append("argumentBuffersSupport answers tier \(tier)")
        }
        for (selector, field) in [
            ("maxIndirectTextures", "indirect_textures"), ("maxIndirectSamplers", "indirect_samplers"),
        ] {
            if let wrong = NativeAnswers.check(
                device, selector, NativeAnswers.indirectResources, field: field, &metrics)
            {
                work.failures.append(wrong)
            }
        }
        guard let sampler = device.makeSamplerState(descriptor: Self.samplerDescriptor()) else {
            throw TestFailure("no sampler")
        }
        let first = try gpu.texture(.rgba8Unorm, 4, 4), second = try gpu.texture(.rgba8Unorm, 4, 4)
        let arguments = try gpu.buffer(24)
        let ids = [first.gpuResourceID._impl, second.gpuResourceID._impl, sampler.gpuResourceID._impl]
        for (k, id) in ids.enumerated() {
            arguments.contents().storeBytes(of: id, toByteOffset: 8 * k, as: UInt64.self)
        }
        let pipeline = try gpu.render(fragment: "argument_textures_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 8, 8)
        work.objects += [sampler, first, second, pipeline, target]
        let timing = try await gpu.run { commands in
            for (texture, colour) in [
                (first, MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)),
                (second, MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)),
            ] {
                let clear = MTLRenderPassDescriptor()
                clear.colorAttachments[0].texture = texture
                clear.colorAttachments[0].loadAction = .clear
                clear.colorAttachments[0].clearColor = colour
                commands.makeRenderCommandEncoder(descriptor: clear)!.endEncoding()
            }
            let render = commands.makeRenderCommandEncoder(descriptor: NativeArgumentEncoderTest.pass(target))!
            render.useResource(first, usage: .read, stages: .fragment)
            render.useResource(second, usage: .read, stages: .fragment)
            NativeArgumentEncoderTest.cover(render, pipeline, arguments)
        }
        work.paths.append(timing.path)
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = (0..<64).filter { i in
            let want: [UInt8] = i % 8 < 4 ? [255, 0, 0, 255] : [0, 0, 255, 255]
            return Array(pixels.pixel(i % 8, i / 8, width: 8)) != want
        }.count
        metrics.set("wrong_pixels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) pixels miss the texture their ID names") }
        return work
    }

    static func samplerDescriptor() -> MTLSamplerDescriptor {
        let d = MTLSamplerDescriptor()
        d.supportArgumentBuffers = true
        return d
    }
}
