import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A resolve into a texture of another sample count: a 32x32 4x target of BGRA8Unorm, RGBA16Float or RGBA8Snorm,
/// MultisampleResolve into a 2x texture of the format cleared green before. drawn: each sample drawn its own value
/// (standin_sample_values; random codes, halves in [-2, 2)): every 2x sample, read through texture2d_ms, stays green.
/// cleared: a pass that only clears: every 2x sample reads as an ordinary resolve of the same pass into a one-sample
/// texture. Tolerance 0. An M4 Max, this scene: both, every sample of every format (drawn per sample, per pixel or
/// sample-masked alike; m2v-passes passoracle.mm resolve_other_count measured the cleared one). Per format and case
/// <fmt>_<case>_off samples off and _max the largest difference (codes; RGBA16Float: half steps); every command buffer
/// raw, failed_cbs=0.
struct ResolveOtherCountTest: HWTest {
    let id = "gpu.gm20b.resolve_other_count"
    let name = "A 4x resolve into a 2x texture keeps it after a draw and spreads a clear-only pass, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 32
    static let formats: [(key: String, format: MTLPixelFormat)] = [
        ("bgra8", .bgra8Unorm), ("rgba16f", .rgba16Float), ("rgba8s", .rgba8Snorm),
    ]
    static let prior = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
    static let cleared = MTLClearColor(red: 0.25, green: -0.5, blue: 0.75, alpha: 1)

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for f in Self.formats {
            for drawn in [true, false] {
                let key = "\(f.key)_\(drawn ? "drawn" : "cleared")"
                do {
                    let judged = try await Self.resolved(gpu, f.format, drawn: drawn)
                    metrics.set(key + "_off", judged.off)
                    metrics.set(key + "_max", judged.worst)
                    let want = drawn ? "every 2x sample kept" : "every 2x sample the 4x resolve"
                    checks.append(("\(key): \(want)", judged.off == 0))
                } catch {
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func values(_ format: MTLPixelFormat) -> [SIMD4<Float>] {
        var state = UInt64(format.rawValue)
        return (0 ..< side * side * 4).map { _ in
            SIMD4<Float>(
                (0 ..< 4).map { _ -> Float in
                    let w = WordMipmapCases.splitmix(&state)
                    switch format {
                    case .rgba8Snorm: return Float(Int(w % 255) - 127) / 127
                    case .rgba16Float: return Float(Float16(Float(w % 4096) / 1024 - 2))
                    default: return Float(w % 256) / 255
                    }
                }
            )
        }
    }

    static func steps(_ a: Float, _ b: Float, _ format: MTLPixelFormat) -> Int {
        switch format {
        case .rgba8Snorm: return Int((abs(a - b) * 127).rounded())
        case .rgba16Float:
            let ordered = { (x: Float) -> Int in
                let bits = Float16(x).bitPattern
                return bits & 0x8000 != 0 ? -Int(bits & 0x7fff) : Int(bits)
            }
            return abs(ordered(a) - ordered(b))
        default: return Int((abs(a - b) * 255).rounded())
        }
    }

    static func resolved(_ gpu: GPU, _ format: MTLPixelFormat, drawn: Bool) async throws -> (off: Int, worst: Int) {
        let values = drawn ? values(format) : nil
        let four = try gpu.texture(format, side, side, usage: [.renderTarget], samples: 4)
        let two = try gpu.texture(format, side, side, usage: [.renderTarget, .shaderRead], samples: 2)
        let one = try gpu.texture(format, side, side)
        try await StandInResolveCases.drawSamples(gpu, nil, into: two, resolve: nil, store: .store, clear: prior)
        let green = try await StandInResolveCases.sampleReads(gpu, two)
        for target in [two, one] {
            try await StandInResolveCases.drawSamples(
                gpu, values, into: four, resolve: target, store: .multisampleResolve, clear: cleared
            )
        }
        let samples = try await StandInResolveCases.sampleReads(gpu, two)
        let reference = try await gpu.texels(one)
        var judged = (off: 0, worst: 0)
        for at in samples.indices {
            let (got, want) = (samples[at], drawn ? green[at] : reference[at / 2])
            judged.off += got == want ? 0 : 1
            judged.worst = max(judged.worst, (0 ..< 4).map { steps(got[$0], want[$0], format) }.max()!)
        }
        return judged
    }
}
