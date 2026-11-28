import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal

/// What a render state the pack holds costs to make: 300 of Core Animation's states (the last of ca_asked.log, which
/// the Core Animation tests after this one make too), each made twice in this process, as Core Animation makes a key
/// in one context and then in another. The facade answers the work of this thread's last creation
/// (switchMetalLastCreation, debug builds: TegraGM20BMetalStatesCost.mm) and what opening the pack took at the
/// process's device open (switchMetalPackOpen). The pack is the device's disk cache of finished states, so a creation
/// in the driver is a hash, one probe and one object: no search of the pack, kernel call, lock, key text, or program
/// copied or imported, at most 100 us at p99 the first time and 50 us the second; and the open reads its header alone.
/// The call's own time, which includes making the descriptor and its functions, is reported beside.
struct PackHitCostTest: HWTest {
    let id = "gpu.pack.hit_cost"
    let name = "A state the pack holds is made in microseconds, with no kernel call, lock, copy or import"
    var subsystem: Subsystem { .metal }
    var timeout: TimeInterval { 120 }

    static let states = 300
    static let ceilingsUs = [100.0, 50.0]
    static let packOpenCeilingUs = 2000
    static let rounds = ["first", "repeat"]
    /// What a creation must not do at all
    static let forbidden = ["lookups", "kernel_calls", "locks", "copied_bytes", "pieces", "text_keys"]
    static let counted = forbidden + ["lock_waits", "builds", "variants"]

    struct Made {
        var callUs: Double
        var cost: [String: NSNumber]

        var driverUs: Double { cost["wall_us"]?.doubleValue ?? 0 }
        func count(_ key: String) -> Int { cost[key]?.intValue ?? 0 }
    }

    struct Round {
        var made: [Made] = []
        var failed: [String] = []
        var missed: [String] = []

        var driver: [Double] { made.map(\.driverUs) }
        var calls: [Double] { made.map(\.callUs) }
    }

    static func ask(_ device: MTLDevice, _ name: String) -> [String: NSNumber]? {
        let selector = NSSelectorFromString(name)
        guard let object = device as? NSObject, object.responds(to: selector) else { return nil }
        return object.perform(selector)?.takeUnretainedValue() as? [String: NSNumber]
    }

    /// Each state of `lines` made once: the driver's time and work, and the call's time less the driver's probes; nil:
    /// a release runtime
    static func make(_ lines: [PipelineLine], _ library: MTLLibrary, _ device: MTLDevice) -> Round? {
        var round = Round()
        for line in lines {
            let began = DispatchTime.now().uptimeNanoseconds
            guard let state = try? CAState.pipeline(line, library: library, device: device) else {
                round.failed.append(line.shortLabel)
                continue
            }
            let call = Double(DispatchTime.now().uptimeNanoseconds - began) / 1000
            guard let cost = ask(device, "switchMetalLastCreation") else { return nil }
            if GM20BFacade.packHit(state) == false { round.missed.append(line.shortLabel) }
            round.made.append(Made(callUs: max(0, call - (cost["probe_us"]?.doubleValue ?? 0)), cost: cost))
        }
        return round
    }

    static func report(_ rounds: [Round], _ metrics: inout Metrics) {
        for (round, name) in zip(rounds, Self.rounds) {
            metrics.set("\(name)_p50_us", Stats.percentile(round.driver, 0.5), digits: 1)
            metrics.set("\(name)_p99_us", Stats.percentile(round.driver, 0.99), digits: 1)
            metrics.set("\(name)_max_us", round.driver.max() ?? 0, digits: 1)
            metrics.set("\(name)_call_p50_us", Stats.percentile(round.calls, 0.5), digits: 1)
            metrics.set("\(name)_call_p99_us", Stats.percentile(round.calls, 0.99), digits: 1)
        }
        let all = rounds.flatMap(\.made)
        for key in counted {
            let total = all.reduce(0) { $0 + $1.count(key) }
            metrics.set(key, total)
            metrics.set("\(key)_per_hit", all.isEmpty ? 0 : Double(total) / Double(all.count), digits: 2)
        }
        metrics.set("fresh", rounds[0].made.filter { $0.count("builds") > 0 }.count)
        let probes = all.map { $0.cost["probe_us"]?.doubleValue ?? 0 }
        metrics.set("probe_p50_us", Stats.percentile(probes, 0.5), digits: 1)
    }

    /// What of the end state the rounds and the open miss: a ceiling passed, or work a creation did that it must not
    static func shortfalls(_ rounds: [Round], packOpenUs: Int) -> [String] {
        var out: [String] = []
        for (round, (name, ceiling)) in zip(rounds, zip(Self.rounds, ceilingsUs)) {
            let p99 = Stats.percentile(round.driver, 0.99)
            if p99 > ceiling { out.append("\(name) p99 \(Int(p99)) us > \(Int(ceiling))") }
        }
        let all = rounds.flatMap(\.made)
        for key in forbidden {
            let total = all.reduce(0) { $0 + $1.count(key) }
            if total != 0 { out.append("\(key) \(total)") }
        }
        if packOpenUs > packOpenCeilingUs { out.append("pack open \(packOpenUs) us > \(packOpenCeilingUs)") }
        return out
    }

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        guard let url = Bundle.main.url(forResource: "ca_asked", withExtension: "log"),
              let text = try? String(contentsOf: url, encoding: .utf8)
        else { return .fail("ca_asked.log is not in the app's bundle") }
        let lines = Array(text.split(separator: "\n").compactMap { PipelineLine(String($0)) }.suffix(Self.states))
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        var rounds: [Round] = []
        for _ in Self.rounds {
            guard let round = Self.make(lines, library, gpu.device) else {
                let why = KernelSysctl.isReleaseKernel()
                    ? GM20BFacade.missing("switchMetalLastCreation", on: gpu.device)
                    : "no creation profile (build with --//bazel:prof_gpu)"
                return .skip(why)
            }
            rounds.append(round)
        }
        var metrics = Metrics()
        metrics.set("states", lines.count)
        Self.report(rounds, &metrics)
        let packOpenUs = Self.ask(gpu.device, "switchMetalPackOpen")?["load_us"]?.intValue ?? 0
        metrics.set("pack_open_us", packOpenUs)
        let missed = rounds[0].missed, failed = rounds[0].failed
        metrics.set("pack_misses", missed.count)
        metrics.set("failed", failed.count)
        if !missed.isEmpty { metrics.set("missed_states", missed.prefix(12).joined(separator: ",")) }
        if !failed.isEmpty { metrics.set("failed_states", failed.prefix(12).joined(separator: ",")) }
        if !failed.isEmpty || !missed.isEmpty {
            return .fail("\(missed.count) states the pack lacks, \(failed.count) not made", metrics)
        }
        let shortfalls = Self.shortfalls(rounds, packOpenUs: packOpenUs)
        return shortfalls.isEmpty ? .pass(metrics) : .fail(shortfalls.joined(separator: "; "), metrics)
    }
}
