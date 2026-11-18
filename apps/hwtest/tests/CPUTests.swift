import Foundation
import HWTestCore

/// A fixed integer-and-float workload: a xorshift stream folded into a CRC-like hash, and a Mandelbrot row.
enum Workload {
    static func run(seed: UInt64, rounds: Int) -> UInt64 {
        var state = seed | 1
        var hash: UInt64 = 0xCBF2_9CE4_8422_2325
        for _ in 0..<rounds {
            state ^= state << 13
            state ^= state >> 7
            state ^= state << 17
            hash = (hash ^ state) &* 0x100_0000_01B3
        }
        var escaped: UInt64 = 0
        for column in 0..<(rounds / 64) {
            let (cx, cy) = (Double(column % 512) / 256 - 1.5, Double(seed % 256) / 128 - 1)
            var (x, y) = (0.0, 0.0)
            var iteration = 0
            while iteration < 64 && x * x + y * y < 4 {
                (x, y) = (x * x - y * y + cx, 2 * x * y + cy)
                iteration += 1
            }
            escaped &+= UInt64(iteration)
        }
        return hash ^ escaped
    }

    static let rounds = 8_000_000

    static func time(threads: Int) -> (seconds: Double, results: [UInt64]) {
        var results = [UInt64](repeating: 0, count: threads)
        let lock = NSLock()
        let start = CFAbsoluteTimeGetCurrent()
        DispatchQueue.concurrentPerform(iterations: threads) { index in
            let value = run(seed: UInt64(index + 1), rounds: rounds)
            lock.lock()
            results[index] = value
            lock.unlock()
        }
        return (CFAbsoluteTimeGetCurrent() - start, results)
    }
}

struct CPUSingleTest: HWTest {
    let id = "cpu.single"
    let name = "Single-core benchmark"
    let subsystem = Subsystem.cpu

    func run(_ context: TestContext) async throws -> Outcome {
        let first = Workload.time(threads: 1)
        let second = Workload.time(threads: 1)
        var metrics = Metrics()
        metrics.set("mops", Double(Workload.rounds) / min(first.seconds, second.seconds) / 1e6, digits: 1)
        metrics.set("cpu_ms", min(first.seconds, second.seconds) * 1000, digits: 1)
        return .judged([("the same input gives the same answer", first.results == second.results)], metrics)
    }
}

struct CPUMultiTest: HWTest {
    let id = "cpu.multi"
    let name = "Multi-core benchmark and scaling"
    let subsystem = Subsystem.cpu

    func run(_ context: TestContext) async throws -> Outcome {
        let cores = ProcessInfo.processInfo.activeProcessorCount
        let single = Workload.time(threads: 1)
        let multi = Workload.time(threads: cores)
        let speedup = Double(cores) * single.seconds / multi.seconds
        var metrics = Metrics()
        metrics.set("cores", cores)
        metrics.set("physical", DeviceInfo.sysctlInt("hw.physicalcpu"))
        metrics.set("speedup", speedup, digits: 2)
        metrics.set("mops", Double(Workload.rounds * cores) / multi.seconds / 1e6, digits: 1)
        return .judged(
            [
                ("every core active (\(cores) of hw.physicalcpu)", cores == DeviceInfo.sysctlInt("hw.physicalcpu")),
                ("scales to at least 75% of \(cores) cores", speedup >= 0.75 * Double(cores)),
                ("threads agree with one thread", multi.results.first == single.results.first),
            ], metrics)
    }
}
