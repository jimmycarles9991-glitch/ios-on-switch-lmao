import Foundation
import HWTestCore
import HWTestGPU
import Metal

extension X1FilterProbes {
    struct WeightFormat {
        let name: String
        let format: MTLPixelFormat
        /// The two texels, 0 then 1, as the format stores them
        let texels: [UInt8]
    }

    static let weightFormats = [
        WeightFormat(name: "bgra8", format: .bgra8Unorm, texels: [0, 0, 0, 0, 255, 255, 255, 255]),
        WeightFormat(
            name: "rgba16f", format: .rgba16Float,
            texels: [0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x3c, 0x00, 0x3c, 0x00, 0x3c, 0x00, 0x3c]
        ),
        WeightFormat(name: "r32f", format: .r32Float, texels: [0, 0, 0, 0, 0x00, 0x00, 0x80, 0x3f]),
    ]

    static let weightSteps = 65536
    static let coarseStride = 64

    /// The ideal lerp of the sweep's point i: its distance from texel 0's centre towards texel 1's
    static func weightPoint(_ i: Int) -> Double { Double(i) / Double(weightSteps) }

    /// What the sweep's samples say of the filter: the distinct values over every point and over the 1/1024 points, the
    /// largest difference from the ideal lerp, and the mean signed difference (below zero: the weights truncate)
    static func weightReading(_ got: [Float]) -> (distinct: Int, coarse: Int, worst: Double, bias: Double) {
        let points = 0 ... weightSteps
        let errors = points.map { Double(got[$0]) - weightPoint($0) }
        let coarse = Set(points.filter { $0 % coarseStride == 0 }.map { got[$0].bitPattern }).count
        return (
            Set(points.map { got[$0].bitPattern }).count, coarse, errors.map(abs).max() ?? 0,
            errors.reduce(0, +) / Double(errors.count)
        )
    }

    /// The fractional bits a count of distinct values from 0 to 1 takes: 257 values are 8 bits
    static func weightBits(_ distinct: Int) -> Int { Int(log2(Double(max(distinct - 1, 1))).rounded()) }

    /// gpu.x1.filter_weight_bits (fixed-function survey finding 2): linear samples of a two-texel ramp, 0 to 1, from texel
    /// 0's centre to texel 1's in steps of 1/65,536, one sample a thread, read as floats. <format>_distinct counts the
    /// values over every step, <format>_distinct_1024 over the 1/1024 steps, <format>_bits the fractional bits that many
    /// make, <format>_worst the largest difference from the ideal lerp, <format>_bias the mean signed one;
    /// filter_weight_bits is bgra8's. R32F is inferred to go through the texture unit's own filter
    /// (supports32BitFloatFiltering is the chip's 3D_H9 answer; the lowering was not read). Reported, not judged. An M4 Max: 257 values (8 bits, rounded to nearest) in every
    /// format and at both step sizes; worst 0.0020757 and bias 0.0000153 for bgra8, 0.0019531 and 0.0000076 for rgba16f
    /// and r32f.
    public static func filterWeights(_ gpu: GPU) async throws -> ProbeFinding {
        let count = weightSteps + coarseStride
        let uv = (0 ..< count).map { SIMD2<Float>(Float(0.5 + weightPoint(min($0, weightSteps))) / 2, 0.5) }
        let points = try gpu.buffer(uv)
        let kernel = try gpu.compute("x1_sample_points")
        var metrics = Metrics()
        var paths: [String] = []
        var bits: [String: Int] = [:]
        for f in weightFormats {
            let texture = try gpu.texture(f.format, 2, 1, usage: [.shaderRead])
            try await gpu.upload(f.texels, to: texture, bytesPerRow: f.texels.count)
            let out = try gpu.buffer(count * 4)
            let timing = try await gpu.run(tolerateError: true) { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(kernel)
                encoder.setTexture(texture, index: 0)
                encoder.setBuffer(points, offset: 0, index: 0)
                encoder.setBuffer(out, offset: 0, index: 1)
                encoder.dispatchThreads(
                    MTLSize(width: count, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: coarseStride, height: 1, depth: 1)
                )
                encoder.endEncoding()
            }
            paths.append(timing.path)
            guard !timing.path.hasPrefix("error:") else {
                metrics.set(f.name + "_failed", 1)
                continue
            }
            let got = Array(
                UnsafeBufferPointer(start: out.contents().bindMemory(to: Float.self, capacity: count), count: count)
            )
            let reading = weightReading(got)
            bits[f.name] = weightBits(reading.distinct)
            metrics.set(f.name + "_distinct", reading.distinct)
            metrics.set(f.name + "_distinct_1024", reading.coarse)
            metrics.set(f.name + "_bits", bits[f.name]!)
            metrics.set(f.name + "_worst", reading.worst, digits: 7)
            metrics.set(f.name + "_bias", reading.bias, digits: 7)
        }
        metrics.set("filter_weight_bits", bits["bgra8"] ?? -1)
        metrics.set("supports_32bit_filtering", gpu.device.supports32BitFloatFiltering)
        let ran = paths.filter { !$0.hasPrefix("error:") }
        guard !ran.isEmpty else { return ProbeFinding(.inconclusive, metrics, note: "every format's sweep failed") }
        return ProbeFinding.counted([:], paths: ran, metrics: metrics)
    }
}
