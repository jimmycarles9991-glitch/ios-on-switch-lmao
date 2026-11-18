import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// An app dispatch longer than a band (TegraGM20BSchedPolicy.h TGM_APP_BAND_NS) launched in pieces of its threadgroup
/// grid (raw/runtime/raw_dispatch_bands.h), each ending a batch. The kernel loops over an argument, so its few dozen
/// instructions price it under a band: its first dispatch is whole and measured (raw_band_rates.h), and once its rate is
/// learned the same dispatch must go in pieces, writing what the whole dispatch writes (each thread its threadgroup, the
/// grid's threadgroup and thread counts, and a value of its position), byte for byte, as with the facade's budget at 0.
struct DispatchPiecesTest: HWTest {
    let id = "gpu.sched.long_dispatch_pieces"
    let name = "Long app dispatches in grid pieces: identical results"
    let subsystem = Subsystem.metal

    static let threads = 1 << 20
    static let groupSize = 256
    static let steps: UInt32 = 2048
    static let rounds = 3
    static let budgetUs = 1000

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        guard let production = DrawBandsTest.budget(gpu.device, Self.budgetUs * 1000) else {
            return .skip("the facade has no band budget (a release build)")
        }
        defer { DrawBandsTest.budget(gpu.device, production) }
        var metrics = Metrics()
        var failures: [String] = []
        let kernel = try gpu.compute("pieces_kernel")
        let out = try gpu.buffer(Self.threads * 16)
        let encode = { (commands: MTLCommandBuffer) in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(kernel)
            encoder.setBuffer(out, offset: 0, index: 0)
            var steps = Self.steps
            encoder.setBytes(&steps, length: 4, index: 1)
            encoder.dispatchThreadgroups(
                MTLSize(width: Self.threads / Self.groupSize, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: Self.groupSize, height: 1, depth: 1))
            encoder.endEncoding()
        }
        var wholeMs = 0.0
        var pieces = 0
        for round in 0..<Self.rounds {
            let before = RawFacade.counters(of: gpu.device)
            let timing = try await gpu.run(encode)
            let after = RawFacade.counters(of: gpu.device)
            let banded = (after["banded_dispatches"] ?? 0) - (before["banded_dispatches"] ?? 0)
            let bands = (after["bands"] ?? 0) - (before["bands"] ?? 0)
            let learned = (after["learned_draws"] ?? 0) - (before["learned_draws"] ?? 0)
            if round == 0 {
                wholeMs = timing.gpuMilliseconds
                metrics.set("path", timing.path)
                metrics.set("us_first", timing.gpuMilliseconds * 1000, digits: 0)
                metrics.set("pieces_first", bands)
                metrics.set("learned_first", learned)
                if learned < 1 { failures.append("the first dispatch's GPU time taught its kernel nothing") }
            } else {
                pieces = banded > 0 ? bands : 0
                metrics.set("us_learned", timing.gpuMilliseconds * 1000, digits: 0)
            }
        }
        metrics.set("pieces_learned", pieces)
        let pieced = Data(bytes: out.contents(), count: out.length)
        memset(out.contents(), 0, out.length)
        _ = DrawBandsTest.budget(gpu.device, 0)
        try await gpu.run(encode)
        _ = DrawBandsTest.budget(gpu.device, Self.budgetUs * 1000)
        let identical = pieced == Data(bytes: out.contents(), count: out.length)
        metrics.set("identical", identical)
        if !identical { failures.append("the pieced dispatch wrote other bytes than the whole one") }
        if wholeMs * 1000 > Double(2 * Self.budgetUs) && pieces < 2 {
            failures.append("a \(Int(wholeMs * 1000)) us dispatch went in \(pieces) pieces once learned")
        }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
