import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A render state that reads vertex and fragment textures at slots 31, 64 and 127, drawn on the driver: 24B119's A13
/// binds 128 textures a stage, past the A10X's 31 the raw encoder kept on both. Each texture holds a distinct power of two,
/// and slots 0 and 63 hold decoys the functions never read, so a handle at another slot's word, or one left out, shows in
/// the exact sum. 21H461's A10X binds 31: it skips. A build without it does not draw the state on raw: path is not "gm20b".
struct WideTexturesTest: HWTest {
    let id = "gpu.gm20b.wide_textures"
    let name = "Vertex and fragment textures at slots 31, 64 and 127 draw on the driver, each read at its own slot"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 8
    static let slots = [31, 64, 127]
    static let decoys = [0, 63]

    static let source: String = {
        let vertexArguments = slots.map { ", texture2d<float> v\($0) [[texture(\($0))]]" }.joined()
        let fragmentArguments = slots.map { ", texture2d<float> f\($0) [[texture(\($0))]]" }.joined()
        let vertexSum = slots.map { " + v\($0).read(uint2(0)).x" }.joined()
        let fragmentSum = slots.map { " + f\($0).read(uint2(0)).x" }.joined()
        return """
        #include <metal_stdlib>
        using namespace metal;
        struct Out { float4 position [[position]]; float sum; };
        vertex Out wide_textures_vertex(uint vid [[vertex_id]]\(vertexArguments)) {
            return {float4(float(vid & 1) * 4 - 1, float(vid >> 1) * 4 - 1, 0, 1), 0.0f\(vertexSum)};
        }
        fragment float4 wide_textures_fragment(Out in [[stage_in]]\(fragmentArguments)) {
            return float4(0.0f\(fragmentSum), in.sum, 0, 1);
        }
        """
    }()

    /// Vertex slot k of `slots` holds 2^k, fragment slot k 2^(k + 3), each decoy 2^10
    static func value(fragment: Bool, _ k: Int) -> Float { Float(1 << (k + (fragment ? 3 : 0))) }
    static let decoy: Float = 1024
    static let want: [Float] = [56, 7, 0, 1]

    func run(_ context: TestContext) async throws -> Outcome {
        if NativeAnswers.ios17 { return .skip("21H461's A10X binds 31 textures a stage") }
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = Self.n
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "wide_textures_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "wide_textures_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba32Float
        let state = try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
        func texel(_ value: Float) async throws -> MTLTexture {
            let texture = try gpu.texture(.r32Float, 1, 1, usage: [.shaderRead])
            try await gpu.upload(withUnsafeBytes(of: value) { [UInt8]($0) }, to: texture, bytesPerRow: 4)
            return texture
        }
        var vertex: [(Int, MTLTexture)] = []
        var fragment: [(Int, MTLTexture)] = []
        for (k, slot) in Self.slots.enumerated() {
            vertex.append((slot, try await texel(Self.value(fragment: false, k))))
            fragment.append((slot, try await texel(Self.value(fragment: true, k))))
        }
        for slot in Self.decoys {
            vertex.append((slot, try await texel(Self.decoy)))
            fragment.append((slot, try await texel(Self.decoy)))
        }
        let target = try gpu.texture(.rgba32Float, n, n)
        let path = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let render = commands.makeRenderCommandEncoder(descriptor: pass)!
            render.setRenderPipelineState(state)
            for (slot, t) in vertex { render.setVertexTexture(t, index: slot) }
            for (slot, t) in fragment { render.setFragmentTexture(t, index: slot) }
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
        metrics.set("slots", Self.slots.map(String.init).joined(separator: "/"))
        metrics.set("wrong_pixels", wrong)
        metrics.set("middle", floats[middle ..< middle + 4].map { String($0) }.joined(separator: "/"))
        var failures: [String] = []
        if path != "gm20b" { failures.append("path") }
        if wrong > 0 { failures.append("wrong_pixels") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: ","), metrics)
    }
}
