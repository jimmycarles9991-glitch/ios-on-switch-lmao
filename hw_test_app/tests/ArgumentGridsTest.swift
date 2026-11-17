import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Arrays of arrays in an argument buffer (textures, samplers, pointers, constants, three deep, and pointers to
/// argument buffers) take one id per innermost element, laid out flat, as an M4 Max's Metal answers for
/// shaders/ArgumentShapes.metal (macOS 27.2). grid_fragment's own encoder must give those lengths and offsets, its
/// nested encoders included, and a draw that reads one element of each grid through what the encoders wrote must give
/// the M4 Max's pixels on the raw path. The case fails on a wrong answer or pixel, a draw off the raw path, or any m2v
/// need asked (m2v_needs_asked).
struct ArgumentGridsTest: HWTest {
    let id = "gpu.raw.argument_array_of_arrays"
    let name = "Arrays of arrays in an argument buffer lay out flat, as Metal's, without m2v"
    let subsystem = Subsystem.metal

    static let places =
        "3@8 4@16 5@24 6@32 9@40 10@48 11@56 12@64 13@72 14@80 20@88 21@96 22@104 23@112 24@120 25@128 "
        + "30@136 31@144 32@152 33@160 34@168 35@176 36@184 37@192 40@200 41@208 42@216 43@224 44@232 45@240 "
        + "70@248 71@256 72@264 73@272"

    static let drawn: [[UInt8]] = [
        [0, 255, 0, 255], [51, 102, 153, 255], [0, 0, 255, 255], [51, 102, 153, 255], [255, 255, 0, 255],
        [0, 255, 255, 255],
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = OwnAnswersTest.needs(gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []
        do {
            try await exercise(gpu, &metrics, &wrong)
        } catch {
            metrics.set("draw", "threw: \(String(describing: error).prefix(200))")
            wrong.append("threw")
        }
        let asked = OwnAnswersTest.needs(gpu.device).filter { $0.value > (before[$0.key] ?? 0) }.keys.sorted()
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    func texel(_ gpu: GPU, _ rgba: [UInt8]) throws -> MTLTexture {
        let texture = try gpu.texture(.rgba8Unorm, 2, 2, usage: [.shaderRead], storage: .shared)
        let pixels = Array([[UInt8]](repeating: rgba, count: 4).joined())
        texture.replace(region: MTLRegionMake2D(0, 0, 2, 2), mipmapLevel: 0, withBytes: pixels, bytesPerRow: 8)
        return texture
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        guard let function = gpu.library.makeFunction(name: "grid_fragment") else { throw TestFailure("no function") }
        guard let encoder = FunctionArgumentEncoderTest.made(function.makeArgumentEncoder(bufferIndex: 0)) else {
            wrong.append("encoder=nil")
            return
        }
        let (sized, places) = try FunctionArgumentEncoderTest.answer(
            gpu, encoder, ids: FunctionArgumentEncoderTest.ids(Self.places))
        if sized != "280/8" || places != Self.places {
            wrong.append("answers")
            metrics.set("answers", "\(sized) \(places)")
        }
        let arguments = try gpu.buffer(encoder.encodedLength)
        encoder.setArgumentBuffer(arguments, offset: 0)
        guard let sampler = gpu.device.makeSamplerState(descriptor: MTLSamplerDescriptor()) else {
            throw TestFailure("no sampler")
        }
        let red = try texel(gpu, [255, 0, 0, 255]), green = try texel(gpu, [0, 255, 0, 255])
        let blue = try texel(gpu, [0, 0, 255, 255]), yellow = try texel(gpu, [255, 255, 0, 255])
        let miss = try gpu.buffer([SIMD4<Float>(1, 0, 0, 1)]), hit = try gpu.buffer([SIMD4<Float>(0.2, 0.4, 0.6, 1)])
        let cyan = try gpu.buffer([SIMD4<Float>(0, 1, 1, 1)])
        var used: [MTLResource] = [red, green, blue, yellow, miss, hit, cyan]
        encoder.constantData(at: 0).storeBytes(of: Float(1), as: Float.self)
        for k in 0..<4 { encoder.setTexture(k == 1 ? green : red, index: 3 + k) }
        for k in 0..<6 { encoder.setSamplerState(sampler, index: 9 + k) }
        for k in 0..<6 { encoder.setBuffer(k == 5 ? hit : miss, offset: 0, index: 20 + k) }
        for k in 0..<8 { encoder.setTexture(k == 5 ? blue : red, index: 30 + k) }
        let consts: [SIMD2<Float>] = [[0.9, 0.9], [0.6, 0.9], [0.9, 0.9], [0.9, 0.9], [0.9, 0.9], [0.2, 0.4]]
        for (k, c) in consts.enumerated() {
            encoder.constantData(at: 40 + k).storeBytes(of: c, as: SIMD2<Float>.self)
        }
        var nested: [String] = []
        for k in 0..<4 {
            guard let inner = encoder.makeArgumentEncoderForBuffer(atIndex: 70 + k) else {
                nested.append("nil")
                continue
            }
            nested.append("\(inner.encodedLength)/\(inner.alignment)")
            let innerArguments = try gpu.buffer(inner.encodedLength)
            inner.setArgumentBuffer(innerArguments, offset: 0)
            inner.setTexture(k == 2 ? yellow : red, index: 0)
            inner.setBuffer(k == 1 ? cyan : miss, offset: 0, index: 1)
            encoder.setBuffer(innerArguments, offset: 0, index: 70 + k)
            used.append(innerArguments)
        }
        if nested != Array(repeating: "16/8", count: 4) {
            wrong.append("nested")
            metrics.set("nested", nested.joined(separator: ","))
        }

        let pipeline = try gpu.render(fragment: "grid_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 6, 1)
        let timing = try await gpu.run { commands in
            let render = commands.makeRenderCommandEncoder(descriptor: NativeArgumentEncoderTest.pass(target))!
            render.useResources(used, usage: .read, stages: .fragment)
            NativeArgumentEncoderTest.cover(render, pipeline, arguments)
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let got = (0..<6).map { Array(pixels.pixel($0, 0, width: 6)) }
        metrics.set("draw", "\(timing.path)/\(got == Self.drawn ? "exact" : "wrong")")
        if got != Self.drawn {
            metrics.set("pixels", got.map { $0.map(String.init).joined(separator: ",") }.joined(separator: " "))
            wrong.append("draw")
        }
        if timing.path != "raw" { wrong.append("draw_path") }
    }
}
