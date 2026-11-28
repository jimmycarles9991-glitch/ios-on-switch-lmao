import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// The resolve of the six signed normalized formats and BGR10_XR, which the driver runs on the 2D engine's signed and
/// A2B10G10R10 formats with no reference driving its rounding. A 64x64 target at 2x and 4x, each sample drawn its own
/// code by a draw masked to it (masked_texel_values), resolved by MultisampleResolve into a one-sample texture of the
/// format. Rows 0-15 hold ties (2x: c and c+1; 4x: four codes summing to 4k+2), the first four texels the ties near
/// zero, at each end and across zero; rows 16-63 random codes. Every channel must be floor(mean of the codes + 0.5),
/// half up toward +inf for negative means too: the rule an M4 Max resolves these formats by (harness/
/// resolvecodesoracle.mm, every channel at 2x and 4x), and this scene's every channel on an M4 Max. Per format and
/// count: <fmt>_x<n>_off channels off, _max the largest |off| in codes, _ties the first four tie texels' offsets (got -
/// want, channels by "/"); every command buffer raw, failed_cbs=0.
struct ResolveCodesTest: HWTest {
    let id = "gpu.gm20b.resolve_codes"
    let name = "Signed normalized and BGR10_XR targets resolve to their codes' mean rounded half up, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Format {
        let key: String
        let format: MTLPixelFormat
        let channels: Int
        /// The largest snorm code; 0 for BGR10_XR's codes 0...1023, value (c - 384) / 510
        let snorm: Int

        var range: ClosedRange<Int> { snorm > 0 ? -snorm ... snorm : 0 ... 1023 }
        var bytesPerTexel: Int { snorm == 0 ? 4 : channels * (snorm == 127 ? 1 : 2) }
        var ties: [(Int, Int)] {
            snorm > 0
                ? [(-3, -2), (-snorm, 1 - snorm), (snorm - 1, snorm), (-1, 0)]
                : [(381, 382), (0, 1), (1022, 1023), (383, 384)]
        }

        func value(_ code: Int) -> Float { snorm > 0 ? Float(code) / Float(snorm) : (Float(code) - 384) / 510 }

        func code(_ bytes: [UInt8], texel: Int, channel: Int) -> Int {
            switch snorm {
            case 127: return Int(Int8(bitPattern: bytes[texel * channels + channel]))
            case 0: return Int(bytes.loaded(as: UInt32.self, at: texel) >> (10 * (2 - channel)) & 1023)
            default: return Int(bytes.loaded(as: Int16.self, at: texel * channels + channel))
            }
        }
    }

    static let formats = [
        Format(key: "r8s", format: .r8Snorm, channels: 1, snorm: 127),
        Format(key: "rg8s", format: .rg8Snorm, channels: 2, snorm: 127),
        Format(key: "rgba8s", format: .rgba8Snorm, channels: 4, snorm: 127),
        Format(key: "r16s", format: .r16Snorm, channels: 1, snorm: 32767),
        Format(key: "rg16s", format: .rg16Snorm, channels: 2, snorm: 32767),
        Format(key: "rgba16s", format: .rgba16Snorm, channels: 4, snorm: 32767),
        Format(key: "bgr10xr", format: .bgr10_xr, channels: 3, snorm: 0),
    ]
    static let side = 64, tieTexels = 16 * 64

    struct Result {
        var off = 0
        var worst = 0
        var ties: [[Int]] = []

        var tiesText: String { ties.map { $0.map(String.init).joined(separator: "/") }.joined(separator: ",") }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for format in Self.formats {
            for samples in [2, 4] {
                let key = "\(format.key)_x\(samples)"
                do {
                    let result = try await Self.resolved(gpu, format, samples: samples)
                    metrics.set(key + "_off", result.off)
                    metrics.set(key + "_max", result.worst)
                    metrics.set(key + "_ties", result.tiesText)
                    checks.append(("\(key): every channel the codes' mean rounded half up", result.off == 0))
                } catch {
                    checks.append(("\(key): \(error)", false))
                }
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["paths", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// Each sample's codes, `codes[(sample * texels + texel) * 4 + channel]`
    static func codes(_ f: Format, samples: Int) -> [Int] {
        let texels = side * side
        var state = UInt64(f.format.rawValue) * 131 + UInt64(samples)
        let pick = { (range: ClosedRange<Int>) in
            range.lowerBound + Int(YcbcrLoadTest.splitmix(&state) % UInt64(range.count))
        }
        let patterns = [[0, 0, 1, 1], [-1, 1, 1, 1], [2, 0, 0, 0], [3, -1, 0, 0], [-2, 2, 1, 1]]
        var codes = [Int](repeating: 0, count: samples * texels * 4)
        for texel in 0 ..< texels {
            for channel in 0 ..< f.channels {
                var drawn: [Int]
                if texel < f.ties.count {
                    let (a, b) = f.ties[texel]
                    drawn = samples == 2 ? [a, b] : [a, a, b, b]
                } else if texel < tieTexels && samples == 2 {
                    let c = pick(f.range.lowerBound ... f.range.upperBound - 1)
                    drawn = pick(0 ... 1) == 0 ? [c, c + 1] : [c + 1, c]
                } else if texel < tieTexels {
                    let k = pick(f.range.lowerBound + 2 ... f.range.upperBound - 3)
                    let pattern = patterns[pick(0 ... patterns.count - 1)]
                    let turn = pick(0 ... 3)
                    drawn = (0 ..< 4).map { k + pattern[($0 + turn) % 4] }
                } else {
                    drawn = (0 ..< samples).map { _ in pick(f.range) }
                }
                for (sample, code) in drawn.enumerated() { codes[(sample * texels + texel) * 4 + channel] = code }
            }
        }
        return codes
    }

    static func resolved(_ gpu: GPU, _ f: Format, samples: Int) async throws -> Result {
        let texels = side * side
        let codes = codes(f, samples: samples)
        let values = try gpu.buffer(
            (0 ..< samples * texels).map { at in
                SIMD4<Float>((0 ..< 4).map { $0 < f.channels ? f.value(codes[at * 4 + $0]) : 0 })
            }
        )
        let state = try gpu.render(fragment: "masked_texel_values", format: f.format, samples: samples)
        let drawn = try gpu.texture(f.format, side, side, usage: [.renderTarget], samples: samples)
        let resolved = try gpu.texture(f.format, side, side)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawn
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .multisampleResolve
            pass.colorAttachments[0].resolveTexture = resolved
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            for sample in 0 ..< samples {
                var at = SIMD2<UInt32>(UInt32(side), UInt32(sample))
                encoder.setFragmentBuffer(values, offset: sample * texels * 16, index: 0)
                encoder.setFragmentBytes(&at, length: 8, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(resolved, bytesPerPixel: f.bytesPerTexel)
        var result = Result()
        for texel in 0 ..< texels {
            var offs: [Int] = []
            for channel in 0 ..< f.channels {
                let sum = (0 ..< samples).reduce(0) { $0 + codes[($1 * texels + texel) * 4 + channel] }
                let want = Int((Double(sum) / Double(samples) + 0.5).rounded(.down))
                let off = f.code(bytes, texel: texel, channel: channel) - want
                result.off += off == 0 ? 0 : 1
                result.worst = max(result.worst, abs(off))
                offs.append(off)
            }
            if texel < 4 { result.ties.append(offs) }
        }
        return result
    }
}
