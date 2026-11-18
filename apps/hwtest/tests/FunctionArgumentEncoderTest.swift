import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A function's own argument encoder (newArgumentEncoderWithBufferIndex:) is the shader compiler's answer from the
/// function's AIR (drivers/TegraGM20BMetal/raw/runtime/raw_buffer_encoders.h), never an m2v twin. The expected encoded
/// lengths, alignments and per-index offsets for shaders/FunctionArgumentEncoders.metal are what an M4 Max's Metal
/// answers for the same source (macOS 27.2): constants, arrays, struct arrays, gaps between ids, every texture kind, a
/// pointer to another argument buffer (its nested encoder), and plain buffers of no struct (an encoder of no bytes). A
/// draw then reads every kind of member through what the encoders wrote and must give the pixels the M4 Max gave. The
/// case fails on a wrong answer or pixel, a draw off the raw path, or any m2v need asked (m2v_needs_asked).
struct FunctionArgumentEncoderTest: HWTest {
    let id = "gpu.raw.function_argument_encoder"
    let name = "A function's argument encoders are the shader compiler's and Metal's, without m2v"
    let subsystem = Subsystem.metal

    /// (function, buffer index, "length/alignment", "id@offset ...")
    static let measured: [(String, Int, String, String)] = [
        ("argenc_mixed_fragment", 0, "160/16", "0@0 1@8 2@16 3@24 4@32 5@48 6@64 7@80 8@144 9@152 10@153"),
        ("argenc_mixed_fragment", 1, "0/1", ""),
        (
            "argenc_arrays_fragment", 2, "112/8",
            "0@0 1@8 2@16 3@24 4@32 5@40 6@48 7@56 8@64 10@72 11@80 12@88 13@96 20@104"
        ),
        ("argenc_arrays_fragment", 5, "24/8", "3@0 10@8 15@16"),
        ("argenc_kinds_kernel", 0, "56/8", "0@0 1@8 2@16 3@24 4@32 5@40 6@48"),
        ("argenc_kinds_kernel", 1, "0/1", ""),
        ("argenc_kinds_kernel", 3, "48/16", "0@0 1@16 2@32 3@40"),
    ]

    /// The M4 Max's pixels for the draw: the array's second texture, the colour times the tint, the nested texture and
    /// colour, the constants, the plain buffer
    static let drawn: [[UInt8]] = [
        [0, 255, 0, 255], [51, 102, 0, 255], [0, 0, 255, 255], [255, 255, 0, 255], [17, 255, 51, 255],
        [255, 0, 255, 255],
    ]

    /// The raw facade answers nil where it makes no encoder, which the Swift signature cannot say
    static func made(_ encoder: MTLArgumentEncoder) -> MTLArgumentEncoder? {
        unsafeBitCast(encoder as AnyObject, to: Int.self) == 0 ? nil : encoder
    }

    /// "length/alignment" and each id's offset from constantData(at:), for the ids `ids` names
    static func answer(_ gpu: GPU, _ encoder: MTLArgumentEncoder, ids: [Int]) throws -> (String, String) {
        let arguments = try gpu.buffer(max(encoder.encodedLength, 16) + 256)
        encoder.setArgumentBuffer(arguments, offset: 256)
        let places = ids.map { id in
            "\(id)@\(arguments.contents().distance(to: encoder.constantData(at: id)) - 256)"
        }
        return ("\(encoder.encodedLength)/\(encoder.alignment)", places.joined(separator: " "))
    }

    static func ids(_ places: String) -> [Int] {
        places.split(separator: " ").compactMap { Int($0.split(separator: "@")[0]) }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = OwnAnswersTest.needs(gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []

        for (function, index, sized, places) in Self.measured {
            guard let f = gpu.library.makeFunction(name: function) else { throw TestFailure("no \(function)") }
            guard let encoder = Self.made(f.makeArgumentEncoder(bufferIndex: index)) else {
                wrong.append("\(function):\(index)=nil")
                continue
            }
            let (gotSized, gotPlaces) = try Self.answer(gpu, encoder, ids: Self.ids(places))
            if gotSized != sized || gotPlaces != places {
                wrong.append("\(function):\(index)")
                metrics.set("\(function)_\(index)", "\(gotSized) \(gotPlaces)")
            }
        }
        do {
            try await draw(gpu, &metrics, &wrong)
        } catch {
            metrics.set("draw", "threw: \(String(describing: error).prefix(200))")
            wrong.append("draw")
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

    /// Every member argenc_mixed_fragment reads, written through its own encoder and the nested one at id 8
    func draw(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        guard let function = gpu.library.makeFunction(name: "argenc_mixed_fragment"),
            let encoder = Self.made(function.makeArgumentEncoder(bufferIndex: 0)),
            let nested = encoder.makeArgumentEncoderForBuffer(atIndex: 8),
            let sampler = gpu.device.makeSamplerState(descriptor: MTLSamplerDescriptor())
        else {
            wrong.append("draw_setup")
            return
        }
        let red = try texel(gpu, [255, 0, 0, 255]), green = try texel(gpu, [0, 255, 0, 255])
        let blue = try texel(gpu, [0, 0, 255, 255])
        let colour = try gpu.buffer([SIMD4<Float>(102.0 / 255, 204.0 / 255, 0, 2)])
        let yellow = try gpu.buffer([SIMD4<Float>(1, 1, 0, 1)]), plain = try gpu.buffer([SIMD4<Float>(1, 0, 1, 1)])
        let arguments = try gpu.buffer(encoder.encodedLength), inner = try gpu.buffer(nested.encodedLength)
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.constantData(at: 0).storeBytes(of: Float(0.5), as: Float.self)
        encoder.setTextures([red, green], range: 1..<3)
        encoder.setSamplerState(sampler, index: 3)
        encoder.setBuffer(colour, offset: 0, index: 4)
        encoder.constantData(at: 5).storeBytes(of: SIMD4<UInt32>(0, 17, 0, 0), as: SIMD4<UInt32>.self)
        encoder.constantData(at: 6).storeBytes(of: SIMD2<Float16>(1, 0), as: SIMD2<Float16>.self)
        var matrix = [Float](repeating: 0, count: 16)
        matrix[6] = 51.0 / 255
        matrix.withUnsafeBytes { encoder.constantData(at: 7).copyMemory(from: $0.baseAddress!, byteCount: 64) }
        encoder.setBuffer(inner, offset: 0, index: 8)
        encoder.constantData(at: 9).storeBytes(of: Int8(3), as: Int8.self)
        encoder.constantData(at: 10).storeBytes(of: UInt8(1), as: UInt8.self)
        nested.setArgumentBuffer(inner, offset: 0)
        nested.setTexture(blue, index: 0)
        nested.setBuffer(yellow, offset: 0, index: 1)

        let pipeline = try gpu.render(fragment: "argenc_mixed_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 6, 1)
        let timing = try await gpu.run { commands in
            let render = commands.makeRenderCommandEncoder(descriptor: NativeArgumentEncoderTest.pass(target))!
            render.useResources(
                [red, green, blue, inner, colour, yellow] as [MTLResource], usage: .read, stages: .fragment)
            render.setFragmentBuffer(plain, offset: 0, index: 1)
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
