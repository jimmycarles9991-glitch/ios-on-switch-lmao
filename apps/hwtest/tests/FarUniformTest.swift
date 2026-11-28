import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Two fragment uniform buffers whose set-0 descriptors lie past byte 496, after twenty storage buffers a stage: NVK reads
/// each through a constant buffer its program names by offset, which the record keeps out of line (pack.h
/// PackProgram::farUbos). Each uniform scales or shifts one channel, so binding either at the other's offset shows. A build
/// without it does not draw the state on raw: path is not "gm20b".
struct FarUniformTest: HWTest {
    let id = "gpu.gm20b.far_uniform"
    let name = "Two uniform buffers past set-0 byte 496 each bind as their own constant buffer on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 8
    static let buffers = 20
    static let tint: SIMD4<Float> = [2, 0.5, 3, 1]
    static let shift: SIMD4<Float> = [1, 2, 3, 4]

    static let source: String = {
        let vertexArguments = (0 ..< buffers).map { ", device const float *v\($0) [[buffer(\($0))]]" }.joined()
        let fragmentArguments = (0 ..< buffers).map { ", device const float *f\($0) [[buffer(\($0))]]" }.joined()
        let vertexSum = (0 ..< buffers).map { " + v\($0)[0]" }.joined()
        let fragmentSum = (0 ..< buffers).map { " + f\($0)[0]" }.joined()
        return """
        #include <metal_stdlib>
        using namespace metal;
        struct Out { float4 position [[position]]; float sum; };
        vertex Out far_uniform_vertex(uint vid [[vertex_id]]\(vertexArguments)) {
            return {float4(float(vid & 1) * 4 - 1, float(vid >> 1) * 4 - 1, 0, 1), 0.0f\(vertexSum)};
        }
        fragment float4 far_uniform_fragment(Out in [[stage_in]]\(fragmentArguments),
                                             constant float4 &tint [[buffer(\(buffers))]],
                                             constant float4 &shift [[buffer(\(buffers + 1))]]) {
            return float4(0.0f\(fragmentSum), in.sum, 0, 1) * tint + shift;
        }
        """
    }()

    static var want: [Float] {
        let sums: SIMD4<Float> = [Float((1 << buffers) - 1) / Float(1 << buffers), Float((1 << buffers) - 1), 0, 1]
        let out = sums * tint + shift
        return [out.x, out.y, out.z, out.w]
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = Self.n
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "far_uniform_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "far_uniform_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba32Float
        let state = try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
        let vertex = try (0 ..< Self.buffers).map { try gpu.buffer([Float(1 << $0)]) }
        let fragment = try (0 ..< Self.buffers).map { try gpu.buffer([Float(1 << $0) / Float(1 << Self.buffers)]) }
        let tint = try gpu.buffer([Self.tint])
        let shift = try gpu.buffer([Self.shift])
        let target = try gpu.texture(.rgba32Float, n, n)
        let path = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let render = commands.makeRenderCommandEncoder(descriptor: pass)!
            render.setRenderPipelineState(state)
            for (i, b) in vertex.enumerated() { render.setVertexBuffer(b, offset: 0, index: i) }
            for (i, b) in fragment.enumerated() { render.setFragmentBuffer(b, offset: 0, index: i) }
            render.setFragmentBuffer(tint, offset: 0, index: Self.buffers)
            render.setFragmentBuffer(shift, offset: 0, index: Self.buffers + 1)
            render.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            render.endEncoding()
        }.path
        let texels = try await gpu.read(target, bytesPerPixel: 16)
        let floats = texels.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
        let want = Self.want
        var wrong = 0
        for p in 0 ..< (n * n) where Array(floats[4 * p ..< 4 * p + 4]) != want { wrong += 1 }
        let middle = 4 * (n / 2 * n + n / 2)
        var metrics = Metrics()
        metrics.set("path", path)
        metrics.set("entries", GM20BFacade.entries(of: state).map(String.init) ?? "-")
        metrics.set("wrong_pixels", wrong)
        metrics.set("middle", floats[middle ..< middle + 4].map { String($0) }.joined(separator: "/"))
        metrics.set("want", want.map { String($0) }.joined(separator: "/"))
        var failures: [String] = []
        if path != "gm20b" { failures.append("path") }
        if wrong > 0 { failures.append("wrong_pixels") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: ","), metrics)
    }
}
