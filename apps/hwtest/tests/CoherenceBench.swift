import Foundation
import HWTestCore
import HWTestKit

/// The bench's stale cells in its own shape: a 64x64 BGRA8 target the GPU writes and the bench frees, then one of the
/// same size on the same span, filled by the CPU and blended into by a program the bench has placed before.
/// tools/benchloop/batches/fvt1.json's program and additive blend, its every column's colour 16/255: the first
/// experiment's texels are the GPU's write (its clear, 0, plus 16), the second's the CPU's fill plus 16. The blend is
/// colour 0's per-target block under the per-target switch the driver owns and keeps on (owned.h, fetch_free.h).
enum CoherenceBench {
    static let sph =
        "YlQCAAAAAAAAAAAAAAAAAAAAAAAAAACQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADwAAAAAAAAA="
    static let code =
        "Agfg4QG8PwAE//cPh/8D4AQKRwCAAbBcBARHAAAASDgBB0DgALT/BwAEBwAQAJXvAgSHABAAle8PAAcAAAAA4+IHIPwAhB8AAA8HAAAAsFAADwcAAACwUAAPBwAAALBQ"
    // colour 0's ADD of ONE and ONE for colour and alpha, SET_BLEND(0)
    static let methods: [[Int]] = [
        [0x1e00, 1], [0x1e04, 0x8006], [0x1e08, 0x4001], [0x1e0c, 0x4001], [0x1e10, 0x8006], [0x1e14, 0x4001],
        [0x1e18, 0x4001], [0x1360, 1],
    ]
    static let restore: [[Int]] = [[0x1360, 0]]

    static func run(rounds: Int) throws -> CoherenceTally {
        var tally = CoherenceTally(arm: "bench")
        let runner: BenchRunner
        do {
            runner = try BenchRunner.open()
        } catch {
            tally.failure = "the bench did not open: \(error)"
            return tally
        }
        var cpu = [UInt8](repeating: 0, count: CoherenceRewriteTest.side * CoherenceRewriteTest.side * 4)
        let (write, fillTemplate) = try experiments(cpu)
        for round in 0 ..< rounds {
            CoherenceRewriteTest.fillCpuBytes(&cpu, round: round)
            var fill = fillTemplate
            fill.fill = Data(cpu)
            let wrote = runner.run(write)
            let filled = runner.run(fill)
            guard wrote.status == "ok", filled.status == "ok", let first = wrote.texels, let out = filled.texels,
                  first.count == cpu.count, out.count == cpu.count
            else {
                tally.failure = "round \(round): \(wrote.status) \(wrote.why) / \(filled.status) \(filled.why)"
                return tally
            }
            // the GPU's last write to the span: the first experiment's draw (D = its colour), or the empty clearing pass
            // the bench's liveness check ends a fill-less experiment with (D = 0)
            tally.countBench(out: out, cpu: cpu, gpu: first)
        }
        return tally
    }

    static func experiments(_ fill: [UInt8]) throws -> (BenchExperiment, BenchExperiment) {
        let side = CoherenceRewriteTest.side
        let data = [Float](repeating: 16.0 / 255.0, count: 256).withUnsafeBytes { Data($0) }.base64EncodedString()
        let shared: [String: Any] = [
            "kind": "readback", "readback": "full",
            "target": ["format": "bgra8unorm", "width": side, "height": side],
            "program": ["sph": sph, "code": code, "gprs": 8, "data": data],
            "methods": methods, "restore": restore,
        ]
        var write = shared
        write["id"] = "coherence-gpu-write"
        var filled = shared
        filled["id"] = "coherence-cpu-fill"
        filled["fill"] = Data(fill).base64EncodedString()
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "coherence", "experiments": [write, filled],
        ]
        let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch)).entries.compactMap {
            if case .experiment(let e) = $0 { return e }
            return nil
        }
        guard decoded.count == 2 else { throw TestFailure("the coherence batch does not decode") }
        return (decoded[0], decoded[1])
    }
}

/// Per arm: the pixels whose read found the GPU's own older bytes (stale) or anything else wrong, and the pixels where
/// the CPU did not read what the GPU had just written
struct CoherenceTally {
    let arm: String
    var rounds = 0
    var stalePixels = 0, staleRounds = 0, wrongPixels = 0, cpuStale = 0
    var failure = ""

    var clean: Bool { rounds > 0 && stalePixels == 0 && wrongPixels == 0 }

    mutating func countConstantOlder(_ out: [UInt8], cpu: [UInt8], added: SIMD4<Int>, older: SIMD4<Int>) {
        rounds += 1
        let r = CoherenceCheck.countConstantOlder(out: out, cpu: cpu, added: added, older: older)
        stalePixels += r.stale
        staleRounds += r.stale > 0 ? 1 : 0
        wrongPixels += r.wrong
    }

    mutating func countBench(out: Data, cpu: [UInt8], gpu: Data) {
        rounds += 1
        let r = out.withUnsafeBytes { ob in
            gpu.withUnsafeBytes { gb in CoherenceCheck.countBench(out: ob, cpu: cpu, gpu: gb) }
        }
        cpuStale += r.cpuStale
        stalePixels += r.stale
        staleRounds += r.stale > 0 ? 1 : 0
        wrongPixels += r.wrong
    }

    static func differing(_ bytes: [UInt8], from texel: [UInt8]) -> Int {
        ByteCompare.wordWrongCount(bytes, want: ByteCompare.word(texel[0], texel[1], texel[2], texel[3]))
    }

    func report(into metrics: inout Metrics) {
        metrics.set("\(arm)_stale_px", stalePixels)
        metrics.set("\(arm)_stale_rounds", staleRounds)
        metrics.set("\(arm)_wrong_px", wrongPixels)
        metrics.set("\(arm)_cpu_stale_px", cpuStale)
        if !failure.isEmpty { metrics.set("\(arm)_failure", String(failure.prefix(120))) }
    }
}
