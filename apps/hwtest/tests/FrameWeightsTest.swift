import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// The frame-weights capture (draw_times.h, through switchMetalFrameWeights: and switchMetalFrameWeightsTaken; debug
/// builds, else SKIP) over one pass into a 1280x720 BGRA8 target: A (a constant colour), B (64 dependent FMAs), C
/// (512) and D (A's pipeline, its triangle one point), four times, each a triangle covering the target so a draw
/// shades exactly 921,600 pixels. The scene runs once uncaptured, once captured, once more for normal_us. Judged: the
/// capture ended over one buffer with nothing unwritten or full; its 20 rows are s, b, p, 16 d, t; A's, B's and C's
/// rows each share a key, D's carries A's; A, B and C count 921,600 invocations and a box of 921,600, D none; each
/// state's four draws within 1.25 of one another (spread_max), medians C > B > A > D; the rows' ns sum to the buffer's
/// gpu_ns within 1% (accounted_ratio); (C - floor) / (B - floor) within 25% of their fragment words' ratio
/// (scaling_c_over_b, words_c_over_b; floor_ns: D's median); captured_us at least 0.95 normal_us (isolation_factor).
struct FrameWeightsTest: HWTest {
    let id = "gpu.diag.frame_weights"
    let name = "The per-draw GPU-time capture over A B C D draws: rows, invocations, timing and isolation"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let arm = NSSelectorFromString("switchMetalFrameWeights:")
    static let taken = NSSelectorFromString("switchMetalFrameWeightsTaken")
    static let pixels = 1280 * 720

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard let device = gpu.device as? NSObject, device.responds(to: Self.arm), device.responds(to: Self.taken)
        else { return .skip(GM20BFacade.missing("switchMetalFrameWeights:", on: gpu.device)) }
        let scene = try Scene(gpu)
        _ = try await scene.run(gpu)
        if let refusal = device.perform(Self.arm, with: NSNumber(value: 1))?.takeUnretainedValue() as? String {
            return .fail("switchMetalFrameWeights: refused: \(refusal)")
        }
        let captured = try await scene.run(gpu)
        guard let capture = device.perform(Self.taken)?.takeUnretainedValue() as? NSDictionary else {
            return .fail("switchMetalFrameWeightsTaken answered no dictionary")
        }
        let normal = try await scene.run(gpu)
        var metrics = Metrics()
        let checks = Self.judged(capture, captured: captured * 1e3, normal: normal * 1e3, &metrics)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        return .judged(checks, metrics, proof: gpu.proof)
    }

    struct Scene {
        let target: MTLTexture
        let states: [MTLRenderPipelineState]

        init(_ gpu: GPU) throws {
            target = try gpu.texture(.bgra8Unorm, 1280, 720)
            states = try ["solid_fragment", "frame_weights_fma64", "frame_weights_fma512"].map {
                try gpu.render(fragment: $0, format: .bgra8Unorm)
            }
        }

        /// The pass's GPU milliseconds
        func run(_ gpu: GPU) async throws -> Double {
            let covering: [SIMD4<Float>] = [SIMD4(-1, -1, 0.5, 1), SIMD4(3, -1, 0.5, 1), SIMD4(-1, 3, 0.5, 1)]
            let point = [SIMD4<Float>](repeating: SIMD4(0, 0, 0.5, 1), count: 3)
            return try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                var colour = SIMD4<Float>(0.25, 0.5, 0.75, 1)
                encoder.setFragmentBytes(&colour, length: 16, index: 0)
                for _ in 0 ..< 4 {
                    for (state, corners) in [(0, covering), (1, covering), (2, covering), (0, point)] {
                        var corners = corners
                        encoder.setRenderPipelineState(states[state])
                        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 3, index: 0)
                        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    }
                }
                encoder.endEncoding()
            }.gpuMilliseconds
        }
    }

    static func median(_ values: [Double]) -> Double { values.sorted()[values.count / 2] }

    static func judged(
        _ capture: NSDictionary, captured: Double, normal: Double, _ metrics: inout Metrics
    ) -> [(String, Bool)] {
        let buffers = capture["buffers"] as? [NSDictionary] ?? []
        let rows = capture["rows"] as? [NSDictionary] ?? []
        let int = { (d: NSDictionary, k: String) in (d[k] as? NSNumber)?.intValue ?? -1 }
        let kinds = rows.map { $0["kind"] as? String ?? "?" }.joined()
        let draws = rows.filter { ($0["kind"] as? String) == "d" }
        let state = { (s: Int) in stride(from: s, to: draws.count, by: 4).map { draws[$0] } }
        let byState = draws.count == 16 ? (0 ..< 4).map(state) : []
        let ns = byState.map { $0.map { Double(int($0, "ns")) } }
        let keys = byState.map { Set($0.map { $0["key"] as? String ?? "" }) }
        let medians = ns.map(median)
        let spread = ns.prefix(3).map { ($0.max() ?? 0) / max($0.min() ?? 1, 1) }.max() ?? 0
        let gpuNs = buffers.first.map { Double(int($0, "gpu_ns")) } ?? 0
        let accounted = rows.map { Double(int($0, "ns")) }.reduce(0, +) / max(gpuNs, 1)
        let floor = medians.count == 4 ? medians[3] : 0
        let scaling = medians.count == 4 ? (medians[2] - floor) / max(medians[1] - floor, 1) : 0
        let words =
            byState.count == 4
                ? Double(int(byState[2][0], "fragment_words")) / Double(max(int(byState[1][0], "fragment_words"), 1)) :
                0
        metrics.set("rows", rows.count)
        metrics.set("draw_rows", draws.count)
        metrics.set("invocations_a", byState.first.map { int($0[0], "invocations") } ?? -1)
        metrics.set("invocations_d", byState.count == 4 ? int(byState[3][0], "invocations") : -1)
        metrics.set("box_a", byState.first.map { int($0[0], "box") } ?? -1)
        for (k, m) in zip(["ns_a", "ns_b", "ns_c", "floor_ns"], medians) { metrics.set(k, Int(m)) }
        metrics.set("spread_max", spread, digits: 3)
        metrics.set("accounted_ratio", accounted, digits: 4)
        metrics.set("captured_us", captured, digits: 1)
        metrics.set("normal_us", normal, digits: 1)
        metrics.set("isolation_factor", captured / max(normal, 1e-9), digits: 3)
        metrics.set("scaling_c_over_b", scaling, digits: 3)
        metrics.set("words_c_over_b", words, digits: 3)
        let counted = { (s: Int, k: String, want: Int) in
            byState.count == 4 && byState[s].allSatisfy { int($0, k) == want }
        }
        return [
            ("the capture ended", (capture["ended"] as? NSNumber)?.boolValue == true),
            (
                "one buffer, nothing unwritten or full",
                buffers.count == 1 && buffers.allSatisfy { int($0, "unwritten") == 0 && int($0, "full") == 0 }
            ),
            ("rows s b p, 16 d, t", kinds == "sbp" + String(repeating: "d", count: 16) + "t"),
            (
                "A, B and C each one key, D A's",
                keys.count == 4 && keys.prefix(3).allSatisfy { $0.count == 1 }
                    && Set(keys.prefix(3).map { $0.first! }).count == 3 && keys[3] == keys[0]
            ),
            (
                "A, B, C 921,600 invocations, D none",
                (0 ..< 3).allSatisfy { counted($0, "invocations", pixels) } && counted(3, "invocations", 0)
            ),
            ("A, B, C boxes of 921,600", (0 ..< 3).allSatisfy { counted($0, "box", pixels) }),
            ("each state's draws within 1.25", byState.count == 4 && spread <= 1.25),
            (
                "medians C > B > A > D",
                medians.count == 4 && medians[2] > medians[1] && medians[1] > medians[0] && medians[0] > medians[3]
            ),
            ("the rows account for the buffer within 1%", abs(accounted - 1) <= 0.01),
            ("time scales with the work within 25%", words > 0 && abs(scaling / words - 1) <= 0.25),
            ("capturing is never faster", captured >= 0.95 * normal),
        ]
    }
}
