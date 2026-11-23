import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The output merger's own sRGB encode of an unblended write (the translator writes the value, no colour table), into
/// BGRA8Unorm_sRGB and RGBA8Unorm_sRGB targets, each value in red, green and blue: a half pipeline (texel_halves) over
/// every half in [0, 1] (15,361) and the specials (negatives, -0, NaN, infinities, past 1); a float pipeline
/// (texel_values) over (i +- 0.5) / 4095 and 1 and 2 ulps either side for i = 0..4095, the 2^-14 grid over [0, 1], and
/// NaN, infinities, -0, 1.5, 1e30 and a denormal. Each byte against Metal's twelve-bit encode (SrgbEncodeTable), which
/// an M4 Max stores on every one of these values in both formats and pipelines. srgb_encode_values: the values per
/// target and pipeline summed; srgb_encode_half_wrong, srgb_encode_float_wrong, srgb_encode_special_wrong: channels
/// off Metal's byte (a correctly rounded encode is off by one at 1,643 of the halves; reported); srgb_encode_max_delta:
/// the largest difference, judged at most 1. Every command buffer on the driver.
struct SrgbEncodeTest: HWTest {
    let id = "gpu.gm20b.srgb_encode"
    let name = "The output merger's sRGB encode of half and float writes against Metal's twelve-bit encode, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let width = 256

    struct Sweep {
        let fragment: String
        let values: [Float]
        /// Which of them are specials
        let special: (Float) -> Bool
    }

    static let halfSpecials: [UInt16] = [
        0x8000, 0x8001, 0xb800, 0xbc00, 0xfbff, 0x7e00, 0x7c00, 0xfc00, 0x3c01, 0x4000, 0x7bff,
    ]
    static let half = Sweep(
        fragment: "texel_halves",
        values: (0 ... 0x3c00).map { Float(Float16(bitPattern: UInt16($0))) }
            + halfSpecials.map { Float(Float16(bitPattern: $0)) },
        special: { !($0 >= 0 && $0 <= 1) || $0.sign == .minus }
    )
    static let floatSpecials: [Float] = [.nan, .infinity, -.infinity, -0.0, 1.5, 1e30, .leastNormalMagnitude / 4]
    static let float = Sweep(
        fragment: "texel_values",
        values: (0 ... 4095).flatMap { i -> [Float] in
            [Double(i) - 0.5, Double(i) + 0.5].flatMap { edge -> [Float] in
                let x = Float(edge / 4095)
                return [x.nextDown.nextDown, x.nextDown, x, x.nextUp, x.nextUp.nextUp]
            }
        } + (0 ... 16384).map { Float($0) / 16384 } + floatSpecials,
        special: { v in floatSpecials.contains { $0.bitPattern == v.bitPattern } }
    )
    static let formats: [(MTLPixelFormat, red: Int)] = [(.bgra8Unorm_srgb, 2), (.rgba8Unorm_srgb, 0)]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var tally = (values: 0, half: 0, float: 0, special: 0, delta: 0)
        for format in Self.formats {
            for (name, sweep) in [("half", Self.half), ("float", Self.float)] {
                do {
                    let reading = try await Self.encoded(gpu, format, sweep)
                    tally.values += sweep.values.count
                    tally.special += reading.special
                    tally.delta = max(tally.delta, reading.delta)
                    if name == "half" { tally.half += reading.wrong } else { tally.float += reading.wrong }
                } catch {
                    checks.append(("\(format.0.rawValue) \(name): \(GPU.describe(error))", false))
                }
            }
        }
        metrics.set("srgb_encode_values", tally.values)
        metrics.set("srgb_encode_half_wrong", tally.half)
        metrics.set("srgb_encode_float_wrong", tally.float)
        metrics.set("srgb_encode_special_wrong", tally.special)
        metrics.set("srgb_encode_max_delta", tally.delta)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every byte within one code of Metal's", tally.delta <= 1))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// Channels off Metal's byte among the ordinary values and among the specials, and the largest difference
    static func encoded(
        _ gpu: GPU, _ format: (MTLPixelFormat, red: Int), _ sweep: Sweep
    ) async throws -> (wrong: Int, special: Int, delta: Int) {
        let height = (sweep.values.count + width - 1) / width
        let target = try gpu.texture(format.0, width, height)
        let state = try gpu.render(fragment: sweep.fragment, format: format.0)
        let data = try gpu.buffer(
            (0 ..< (width * height)).map { i in
                let v = i < sweep.values.count ? sweep.values[i] : 0
                return SIMD4<Float>(v, v, v, 1)
            }
        )
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            var row = UInt32(width)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(data, offset: 0, index: 0)
            encoder.setFragmentBytes(&row, length: 4, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        var reading = (wrong: 0, special: 0, delta: 0)
        for (i, v) in sweep.values.enumerated() {
            let want = SrgbEncodeTable.byte(Double(v))
            for channel in [format.red, 1, 2 - format.red] {
                let delta = abs(Int(bytes[i * 4 + channel]) - want)
                guard delta != 0 else { continue }
                reading.delta = max(reading.delta, delta)
                if sweep.special(v) { reading.special += 1 } else { reading.wrong += 1 }
            }
        }
        return reading
    }
}
