import Foundation
import HWTestCore

/// The bench's stale cells in its own shape: a 64x64 BGRA8 target the GPU writes and the bench frees, then one of the
/// same size on the same span, filled by the CPU and blended into by a program the bench has placed before.
/// tools/benchloop/batches/fvt1.json's program and additive blend, its every column's colour 16/255: the first
/// experiment's texels are the GPU's write (its clear, 0, plus 16), the second's the CPU's fill plus 16.
enum CoherenceBench {
    static let sph =
        "YlQCAAAAAAAAAAAAAAAAAAAAAAAAAACQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADwAAAAAAAAA="
    static let code =
        "Agfg4QG8PwAE//cPh/8D4AQKRwCAAbBcBARHAAAASDgBB0DgALT/BwAEBwAQAJXvAgSHABAAle8PAAcAAAAA4+IHIPwAhB8AAA8HAAAAsFAADwcAAACwUAAPBwAAALBQ"
    // SET_BLEND_STATE_PER_TARGET, colour 0's ADD of ONE and ONE for colour and alpha, SET_BLEND(0)
    static let methods: [[Int]] = [
        [0x12e4, 1], [0x1e00, 1], [0x1e04, 0x8006], [0x1e08, 0x4001], [0x1e0c, 0x4001], [0x1e10, 0x8006],
        [0x1e14, 0x4001], [0x1e18, 0x4001], [0x1360, 1],
    ]
    static let restore: [[Int]] = [[0x1360, 0], [0x12e4, 1]]

    static func run(rounds: Int) throws -> CoherenceTally {
        var tally = CoherenceTally(arm: "bench")
        let runner: BenchRunner
        do {
            runner = try BenchRunner.open()
        } catch {
            tally.failure = "the bench did not open: \(error)"
            return tally
        }
        for round in 0..<rounds {
            let cpu = CoherenceRewriteTest.cpuBytes(round)
            let (write, fill) = try experiments(cpu)
            let wrote = runner.run(write)
            let filled = runner.run(fill)
            guard wrote.status == "ok", filled.status == "ok", let first = wrote.texels, let out = filled.texels,
                first.count == cpu.count, out.count == cpu.count
            else {
                tally.failure = "round \(round): \(wrote.status) \(wrote.why) / \(filled.status) \(filled.why)"
                return tally
            }
            let gpu = [UInt8](first)
            tally.cpuStale +=
                stride(from: 0, to: gpu.count, by: 4).filter { at in
                    !(0..<4).allSatisfy { abs(Int(gpu[at + $0]) - 16) <= 1 }
                }.count
            // the GPU's last write to the span: the first experiment's draw (D = its colour), or the empty clearing pass
            // the bench's liveness check ends a fill-less experiment with (D = 0)
            tally.count(
                [UInt8](out), cpu: cpu, added: { p, c in Int(gpu[p * 4 + c]) },
                older: [{ p, c in min(255, 2 * Int(gpu[p * 4 + c])) }, { p, c in Int(gpu[p * 4 + c]) }])
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

    /// `out` against the CPU's bytes plus `added`, and, where it misses, against each of `older`: what the draw makes of
    /// one of the GPU's own earlier writes
    mutating func count(_ out: [UInt8], cpu: [UInt8], added: (Int, Int) -> Int, older: [(Int, Int) -> Int]) {
        rounds += 1
        guard out.count == cpu.count else {
            wrongPixels += cpu.count / 4
            return
        }
        var stale = 0
        for p in 0..<(cpu.count / 4) {
            var fresh = true
            var old = [Bool](repeating: true, count: older.count)
            for c in 0..<4 {
                let v = Int(out[p * 4 + c])
                fresh = fresh && abs(v - min(255, Int(cpu[p * 4 + c]) + added(p, c))) <= 1
                for (k, earlier) in older.enumerated() { old[k] = old[k] && abs(v - earlier(p, c)) <= 1 }
            }
            if !fresh {
                if old.contains(true) { stale += 1 } else { wrongPixels += 1 }
            }
        }
        stalePixels += stale
        staleRounds += stale > 0 ? 1 : 0
    }

    static func differing(_ bytes: [UInt8], from texel: [UInt8]) -> Int {
        stride(from: 0, to: bytes.count, by: 4).filter { at in !(0..<4).allSatisfy { bytes[at + $0] == texel[$0] } }
            .count
    }

    func report(into metrics: inout Metrics) {
        metrics.set("\(arm)_stale_px", stalePixels)
        metrics.set("\(arm)_stale_rounds", staleRounds)
        metrics.set("\(arm)_wrong_px", wrongPixels)
        metrics.set("\(arm)_cpu_stale_px", cpuStale)
        if !failure.isEmpty { metrics.set("\(arm)_failure", String(failure.prefix(120))) }
    }
}
