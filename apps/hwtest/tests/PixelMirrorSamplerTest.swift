import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A sampler of pixel coordinates whose s and t axes mirror once to the edge (call.sampler: such a sampler was m2v's alone,
/// so every draw that bound it went there). Metal defines pixel coordinates only under ClampToEdge (MTLSampler.h
/// normalizedCoordinates), so the driver clamps those axes to the edge: a 4x4 texture read at its texel centres through
/// it is the texture's texels, on the driver.
struct PixelMirrorSamplerTest: HWTest {
    let id = "gpu.gm20b.pixel_mirror_sampler"
    let name = "A pixel-coordinate sampler that mirrors once to the edge samples on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var texels: [UInt8] = []
        for i in 0 ..< 16 { texels += [UInt8(16 * i), UInt8(255 - 16 * i), UInt8(7 * i), 255] }
        let source = try gpu.texture(.rgba8Unorm, 4, 4, usage: [.shaderRead])
        try await gpu.upload(texels, to: source, bytesPerRow: 16)
        let descriptor = MTLSamplerDescriptor()
        descriptor.normalizedCoordinates = false
        descriptor.sAddressMode = .mirrorClampToEdge
        descriptor.tAddressMode = .mirrorClampToEdge
        guard let sampler = gpu.device.makeSamplerState(descriptor: descriptor) else {
            throw TestFailure("no pixel mirror sampler")
        }
        let state = try gpu.render(fragment: "pixel_mirror_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 12, 12)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(source, index: 0)
            encoder.setFragmentSamplerState(sampler, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        var wrong = 0
        for y in 4 ..< 8 {
            for x in 4 ..< 8 {
                let at = 4 * (x + 12 * y), i = 4 * ((x - 4) + 4 * (y - 4))
                wrong += Array(bytes[at ..< at + 4]) == Array(texels[i ..< i + 4]) ? 0 : 1
            }
        }
        var metrics = Metrics()
        metrics.set("path", timing.path)
        metrics.set("texels_wrong", wrong)
        metrics.set("first", bytes[4 * (4 + 12 * 4) ..< 4 * (4 + 12 * 4) + 4].map { String($0) }.joined(separator: "/"))
        var failures: [String] = []
        if timing.path != "gm20b" { failures.append("the draw went \(timing.path)") }
        if wrong != 0 { failures.append("\(wrong) of 16 texels wrong") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
