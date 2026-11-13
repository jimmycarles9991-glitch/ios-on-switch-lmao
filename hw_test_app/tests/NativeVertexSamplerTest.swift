import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Vertex samplers bound by range (with and without LOD clamps) and singly, beside a state whose vertex function
/// samples nothing: the command buffer stays raw and draws its colour. A state whose vertex function samples a texture
/// draws its texel's position wherever the facade sends it (its path is a metric).
struct NativeVertexSamplerTest: NativeObjectTest {
    let id = "metal.native.vertex_samplers"
    let name = "Vertex samplers bound by range keep a command buffer raw; a vertex-sampling state draws its texel"

    static let samplingSource = """
        #include <metal_stdlib>
        using namespace metal;
        vertex float4 sampled_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]],
                                     texture2d<float> scale [[texture(0)]], sampler s [[sampler(0)]]) {
            float4 p = corners[vid];
            return float4(p.xy * scale.sample(s, float2(0.5), level(0)).r, p.zw);
        }
        """

    static func sampler(_ device: MTLDevice, _ filter: MTLSamplerMinMagFilter) throws -> MTLSamplerState {
        let descriptor = MTLSamplerDescriptor()
        descriptor.minFilter = filter
        descriptor.magFilter = filter
        guard let sampler = device.makeSamplerState(descriptor: descriptor) else {
            throw TestFailure("makeSamplerState returned nil")
        }
        return sampler
    }

    static func pass(_ target: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        return pass
    }

    /// Pixels of `target` that differ from green inside [lo, hi) of both axes and from black outside
    static func wrong(_ pixels: [UInt8], size: Int, inside lo: Int, _ hi: Int) -> Int {
        (0..<size * size).filter { i in
            let (x, y) = (i % size, i / size)
            let want: [UInt8] = (lo..<hi).contains(x) && (lo..<hi).contains(y) ? [0, 255, 0, 255] : [0, 0, 0, 255]
            return Array(pixels.pixel(x, y, width: size)) != want
        }.count
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        let samplers = try [MTLSamplerMinMagFilter.nearest, .linear, .nearest].map { try Self.sampler(device, $0) }
        var work = NativeWork(objects: samplers)
        let size = 32
        let target = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let bound = try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexSamplerStates(samplers, range: 0..<3)
            encoder.setVertexSamplerStates(
                [samplers[1], samplers[0]], lodMinClamps: [0, 1], lodMaxClamps: [4, 2], range: 3..<5)
            encoder.setVertexSamplerState(samplers[2], index: 5)
            GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0, 1, 0, 1))
            encoder.endEncoding()
        }
        work.paths.append(bound.path)
        let wrongBound = Self.wrong(try await gpu.read(target, bytesPerPixel: 4), size: size, inside: 0, size)
        metrics.set("wrong_bound", wrongBound)
        if wrongBound != 0 { work.failures.append("\(wrongBound) pixels wrong beside the vertex samplers") }

        let library = try await device.makeLibrary(source: Self.samplingSource, options: nil)
        let sampling = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm) {
            $0.vertexFunction = library.makeFunction(name: "sampled_vertex")
        }
        let half = try gpu.texture(.r8Unorm, 1, 1, usage: [.shaderRead])
        try await gpu.upload([128], to: half, bytesPerRow: 1)
        let ownSampler = try Self.sampler(device, .nearest)
        let sampled = try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
            encoder.setRenderPipelineState(sampling)
            encoder.setVertexTexture(half, index: 0)
            encoder.setVertexSamplerStates([ownSampler], range: 0..<1)
            GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0, 1, 0, 1))
            encoder.endEncoding()
        }
        metrics.set("sampling_path", sampled.path)
        let wrongSampled = Self.wrong(try await gpu.read(target, bytesPerPixel: 4), size: size, inside: 8, 24)
        metrics.set("wrong_sampled", wrongSampled)
        if wrongSampled > 2 * size { work.failures.append("\(wrongSampled) pixels miss the sampled half-size quad") }
        return work
    }
}
