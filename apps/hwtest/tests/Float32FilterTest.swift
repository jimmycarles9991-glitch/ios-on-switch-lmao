import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// 32-bit float filtering (kits/SwitchGM20B/caps/features.tsv 3D-H9, probe d7): the device answers
/// supports32BitFloatFiltering YES, and R32F, RG32F and RGBA32F textures of three levels, holding values half precision
/// cannot carry, sample bilinearly within a level and linearly between levels exactly as the weights say. The sample
/// points put every weight on a multiple of 1/8, so the exact result is a float the GPU must return.
struct Float32FilterTest: HWTest {
    let id = "metal.float32_filtering"
    let name = "R32F, RG32F and RGBA32F filter linearly within and across mip levels"
    let subsystem = Subsystem.metal

    static let formats: [(name: String, format: MTLPixelFormat, channels: Int)] = [
        ("r32f", .r32Float, 1), ("rg32f", .rg32Float, 2), ("rgba32f", .rgba32Float, 4),
    ]

    /// Level `level`'s texel (x, y), channel c: integers below 2^24 that are multiples of 8
    static func value(_ level: Int, _ x: Int, _ y: Int, _ c: Int) -> Double {
        let side = 4 >> level
        return Double(1_000_000 * (level + 1) + 8192 * (x + side * y) + 1024 * c)
    }

    static func bilinear(_ level: Int, _ u: Double, _ v: Double, _ c: Int) -> Double {
        let side = 4 >> level
        let fx = u * Double(side) - 0.5, fy = v * Double(side) - 0.5
        let x0 = Int(fx.rounded(.down)), y0 = Int(fy.rounded(.down))
        let ax = fx - Double(x0), ay = fy - Double(y0)
        let at = { (x: Int, y: Int) in value(level, min(max(x, 0), side - 1), min(max(y, 0), side - 1), c) }
        let top = (1 - ax) * at(x0, y0) + ax * at(x0 + 1, y0)
        let bottom = (1 - ax) * at(x0, y0 + 1) + ax * at(x0 + 1, y0 + 1)
        return (1 - ay) * top + ay * bottom
    }

    static func expected(_ point: SIMD4<Float>, _ c: Int) -> Double {
        let lod = Double(point.z), base = Int(lod.rounded(.down))
        let low = bilinear(base, Double(point.x), Double(point.y), c)
        guard lod > Double(base), base < 2 else { return low }
        let high = bilinear(base + 1, Double(point.x), Double(point.y), c)
        return low + (lod - Double(base)) * (high - low)
    }

    static let points: [SIMD4<Float>] = [
        SIMD4(0.1875, 0.1875, 0, 0), SIMD4(0.4375, 0.6875, 0, 0), SIMD4(0.6875, 0.4375, 0, 0),
        SIMD4(0.4375, 0.6875, 0.5, 0), SIMD4(0.6875, 0.1875, 0.5, 0), SIMD4(0.4375, 0.4375, 1, 0),
        SIMD4(0.375, 0.625, 1.5, 0), SIMD4(0.5, 0.5, 2, 0),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        var metrics = Metrics()
        metrics.set("supports_32bit_filtering", gpu.device.supports32BitFloatFiltering)
        var wrong: [String] = []
        if kind == "raw" && !gpu.device.supports32BitFloatFiltering { wrong.append("answer") }
        let kernel = try gpu.compute("float32_filter_sample")
        let points = try gpu.buffer(Self.points)
        var paths: [String] = []
        for (name, format, channels) in Self.formats {
            let texture = try gpu.texture(format, 4, 4, usage: [.shaderRead], mipmaps: 3)
            for level in 0..<3 {
                let side = 4 >> level
                var floats: [Float] = []
                for y in 0..<side {
                    for x in 0..<side { for c in 0..<channels { floats.append(Float(Self.value(level, x, y, c))) } }
                }
                try await gpu.upload(
                    floats.withUnsafeBytes { Array($0) }, to: texture, bytesPerRow: side * channels * 4, level: level)
            }
            let out = try gpu.buffer(Self.points.count * 16)
            paths.append(
                try await gpu.run { commands in
                    let encoder = commands.makeComputeCommandEncoder()!
                    encoder.setComputePipelineState(kernel)
                    encoder.setTexture(texture, index: 0)
                    encoder.setBuffer(points, offset: 0, index: 0)
                    encoder.setBuffer(out, offset: 0, index: 1)
                    encoder.dispatchThreads(
                        MTLSize(width: Self.points.count, height: 1, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: Self.points.count, height: 1, depth: 1))
                    encoder.endEncoding()
                }.path)
            let got = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: Self.points.count)
            var worst = 0.0
            for (i, point) in Self.points.enumerated() {
                for c in 0..<channels { worst = max(worst, abs(Double(got[i][c]) - Self.expected(point, c))) }
            }
            metrics.set("\(name)_max_diff", worst, digits: 3)
            if worst > 1 { wrong.append(name) }
        }
        metrics.set("paths", paths.allSatisfy { $0 == "raw" } ? "raw" : paths.joined(separator: ","))
        if kind == "raw" && !paths.allSatisfy({ $0 == "raw" }) { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
