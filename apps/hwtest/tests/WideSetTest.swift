import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A render state of 40 Metal arguments, twenty storage buffers a stage, drawn on the driver: its set 0 holds more than
/// the 32 entries a pack record had room for (pack version 27 keeps the rest in a far block) and past the encoder's 512
/// bytes, and the uniform buffer m2v binds of its own lies past set-0 byte 496 (a constant buffer NVK names by offset). Each buffer
/// holds a distinct power of two, so a descriptor at another entry's bytes, or one left out, shows in the exact sum. A build
/// without it does not draw the state on raw: path is not "gm20b".
struct WideSetTest: HWTest {
    let id = "gpu.gm20b.wide_set"
    let name = "A render state of 40 buffer arguments draws on the driver with every buffer read at its own entry"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 8
    static let buffers = 20

    static let source: String = {
        let vertexArguments = (0 ..< buffers).map { ", device const float *v\($0) [[buffer(\($0))]]" }.joined()
        let fragmentArguments = (0 ..< buffers).map { ", device const float *f\($0) [[buffer(\($0))]]" }.joined()
        let vertexSum = (0 ..< buffers).map { " + v\($0)[0]" }.joined()
        let fragmentSum = (0 ..< buffers).map { " + f\($0)[0]" }.joined()
        return """
        #include <metal_stdlib>
        using namespace metal;
        struct Out { float4 position [[position]]; float sum; };
        vertex Out wide_set_vertex(uint vid [[vertex_id]]\(vertexArguments)) {
            return {float4(float(vid & 1) * 4 - 1, float(vid >> 1) * 4 - 1, 0, 1), 0.0f\(vertexSum)};
        }
        fragment float4 wide_set_fragment(Out in [[stage_in]]\(fragmentArguments)) {
            return float4(0.0f\(fragmentSum), in.sum, 0, 1);
        }
        """
    }()

    /// Vertex buffer i holds 2^i, fragment buffer i 2^(i - 20): both sums exact in a float
    static let want: [Float] = [Float((1 << buffers) - 1) / Float(1 << buffers), Float((1 << buffers) - 1), 0, 1]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = Self.n
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "wide_set_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "wide_set_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba32Float
        let state = try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
        let vertex = try (0 ..< Self.buffers).map { try gpu.buffer([Float(1 << $0)]) }
        let fragment = try (0 ..< Self.buffers).map { try gpu.buffer([Float(1 << $0) / Float(1 << Self.buffers)]) }
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
            render.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            render.endEncoding()
        }.path
        let texels = try await gpu.read(target, bytesPerPixel: 16)
        let floats = texels.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
        var wrong = 0
        for p in 0 ..< (n * n) where Array(floats[4 * p ..< 4 * p + 4]) != Self.want { wrong += 1 }
        let middle = 4 * (n / 2 * n + n / 2)
        var metrics = Metrics()
        metrics.set("path", path)
        metrics.set("entries", GM20BFacade.entries(of: state).map(String.init) ?? "-")
        metrics.set("arguments", 2 * Self.buffers)
        metrics.set("wrong_pixels", wrong)
        metrics.set("middle", floats[middle ..< middle + 4].map { String($0) }.joined(separator: "/"))
        var failures: [String] = []
        if path != "gm20b" { failures.append("path") }
        if wrong > 0 { failures.append("wrong_pixels") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: ","), metrics)
    }
}
