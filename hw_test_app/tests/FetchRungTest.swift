import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Fetch rungs 2 and 3 (drivers/TegraGM20BMetal/raw/runtime/raw_fetch_free.h) on the chip: Core Animation states that
/// blend by fetching their colour, each drawn by its storage form (rung 4: the fetch and the blend inside the pixel
/// interlock) and by its fetch-free form (rung 2: the ROP's 8-bit blender with the form's factors; rung 3: the iterated
/// blend unit running the form's program, the colour targets narrowed to the blended one), both forced through the
/// facade's probe channel; the colour matrix (vcm) takes its program and constants from the GPU's macro each draw. Per
/// state, keyed by its fragment blend: worst_<blend> the largest difference in codes between the two routes' first
/// attachments (rule fetch.blend allows 2; rung 3's programs are exact in QuartzCore's arithmetic, so 2 too),
/// us_rung4_<blend> and us_rung<N>_<blend> one full-screen draw's GPU time each way, us_pass_<blend> the fetch-free
/// form's time per pass of its program. r<N>= the draws the facade's counter says took the fetch-free form. Core
/// Animation's clear (clr, clr_m1 with its memoryless image) loads no colour, so it has no storage or fetch-free form
/// and draws plain: fetched_<blend> the fetching draws the facade counted over its draws (0), path_<blend> its path.
/// verdict=PASS: every state within the bound and faster on its fetch-free form, with every forced draw counted, and
/// every plain state drawn raw with no fetch.
/// proven= the programs every state of which was within the bound and faster, the list raw_chip_facts.h
/// kIteratedProven takes for rung 3: a program is taken by its own measured cost, not by its pass count. A state this
/// IPSW's QuartzCore lacks is skipped. Rung 3 programs a unit no other driver here drives: a wrong setup kills the channel
/// (class errors 0x59, 0x5c, 0x5d in the kernel log), so it runs last.
struct FetchRungTest: HWTest {
    let id: String
    let rung: Int
    var name: String { "Fetch rung \(rung) against the storage form: codes and GPU time, Core Animation's states" }
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 300 }

    static let bound = 2
    static let draws = 10
    static let runs = 5

    /// The states with a fetch-free form of each rung (raw/pack/ca/fetchfree.py; corpus lines of each IPSW's
    /// QuartzCore, tagged os=24B119 and os=21H461), each with the key its metrics take (its fragment blend) and its
    /// program's passes (raw/pack/iterated/table.py; vcm's macro at its longest, which CAStateSweep's matrix selects)
    static let states: [Int: [(label: String, key: String, passes: Int)]] = [
        2: [("PBGRAXm_TimgBaddA2Xhf_Isrc", "add", 0)],
        3: [
            ("PBGRAXm_TimgBmpyA2Xhf_Isrc", "mpy", 2), ("PBGRAXm_TimgBpdiA2Xhf_Isrc", "pdi", 2),
            ("PBGRAXm_TimgBlbnA2Xhf_Isrc", "lbn", 4), ("PBGRAXm_TimgBpldA2Xhf_Isrc", "pld", 5),
            ("PBGRAXm_TimgBdrkA2Xhf_Isrc", "drk", 5), ("PBGRAXm_TimgBltnA2Xhf_Isrc", "ltn", 5),
            ("PBGRAXm_TimgBsdsA2Xhf_Isrc", "sds", 5), ("PBGRAXm_TimgBdifA2Xhf_Isrc", "dif", 8),
            ("PBGRAXm_TimgBlltA2Xhf_Isrc", "llt", 9), ("PBGRAXm_TimgBovlA2Xhf_Isrc", "ovl", 9),
            ("PBGRAXm_TimgBpinA2Xhf_Isrc", "pin", 9), ("PBGRAXm_TimgBsbtA2Xhf_Isrc", "sbt", 9),
            ("PBGRAXm_TimgBhltA2Xhf_Isrc", "hlt", 10), ("PBGRAXm_TimgBsltA2Xhf_Isrc", "slt", 11),
            ("PBGRAXm_TimgBdsoA2Xhf_Isrc", "dso", 13),
            ("PBGRAXm_TimgBlsoA2Xhf_Isrc", "lso", 14), ("PBGRAXm_TimgBdlcA2Xhf_Isrc", "dlc", 28),
            ("PBGRAXmw_TimgBvcmA3Xhf_Isrc", "vcm", 38), ("PBGRAXm_TimgBlumA2Xhf_Isrc", "lum", 42),
            ("PBGRAXm_TimgBcliA2Xhf_Isrc", "cli", 54), ("PBGRAXm_TimgBsatA2Xhf_Isrc", "sat", 64),
        ],
    ]

    static let plain: [Int: [(label: String, key: String)]] = [
        2: [("PBGRAXm_TimgBclrA2Xhf_Isrc", "clr"), ("PBGRAXm_TimgBclrA2Xhf_IsrcM1", "clr_m1")]
    ]
    static let fetchCounters = ["fetch_sequential", "fetch_interlocked", "fetch_rung2", "fetch_rung3"]

    /// The iterated program a state's key names on this IPSW, as raw_chip_facts.h kIteratedProven takes it: QuartzCore
    /// 21H461's colour matrix has no clamp pair, so its form is vcm.rows (raw/pack/fetchfree_link.py)
    static func program(_ key: String) -> String {
        key == "vcm" && ProcessInfo.processInfo.operatingSystemVersion.majorVersion < 27 ? "vcm.rows" : key
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else { return .skip("the default device is not the raw facade") }
        guard let statesURL = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
            let text = try? String(contentsOf: statesURL, encoding: .utf8)
        else { return .fail("ca_states.log is not in the app's bundle") }
        let keys = Dictionary(uniqueKeysWithValues: (Self.states[rung] ?? []).map { ($0.label, ($0.key, $0.passes)) })
        let plainKeys = Dictionary(uniqueKeysWithValues: (Self.plain[rung] ?? []).map { ($0.label, $0.key) })
        let all = CAStateSweep.states(text, everyState: true)
        let lines = all.filter { keys[$0.shortLabel] != nil }
        let plainLines = all.filter { plainKeys[$0.shortLabel] != nil }
        if lines.isEmpty && plainLines.isEmpty { return .skip("this QuartzCore makes none of the rung's states") }
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        var metrics = Metrics()
        var beyond: [String] = []
        var slower: [String] = []
        var outcomes: [(program: String, ok: Bool)] = []
        let key = "fetch_rung\(rung)"
        let before = RawFacade.counters(of: gpu.device)[key] ?? 0
        var forcedDraws = 0
        for line in lines {
            let (label, passes) = keys[line.shortLabel]!
            guard
                let storage = try await FetchRungScene.forcing(
                    4, gpu.device,
                    {
                        try await FetchRungScene.bytes(line, library: library, gpu: gpu)
                    }),
                let free = try await FetchRungScene.forcing(
                    rung, gpu.device,
                    {
                        try await FetchRungScene.bytes(line, library: library, gpu: gpu)
                    })
            else { return .skip("the facade has no probe channel (a release build)") }
            forcedDraws += 2
            let (worst, differing) = FetchRungScene.difference(storage.1, free.1)
            metrics.set("worst_\(label)", worst)
            metrics.set("differing_\(label)", differing)
            metrics.set("path_\(label)", "\(storage.0)/\(free.0)")
            let within = worst <= Self.bound && storage.0 == "raw" && free.0 == "raw"
            if !within { beyond.append(line.shortLabel) }
            var us: [Double] = []
            for (forced, suffix) in [(4, "rung4"), (rung, "rung\(rung)")] {
                let timed = try await FetchRungScene.forcing(forced, gpu.device) {
                    try await FetchRungScene.microseconds(
                        line, library: library, gpu: gpu, draws: Self.draws, runs: Self.runs)
                }
                us.append(timed?.0 ?? 0)
                metrics.set("us_\(suffix)_\(label)", us.last!, digits: 1)
            }
            let faster = us[1] > 0 && us[1] < us[0]
            if !faster { slower.append(label) }
            if passes > 0 { metrics.set("us_pass_\(label)", us[1] / Double(passes), digits: 2) }
            outcomes.append((Self.program(label), within && faster))
            forcedDraws += Self.draws * (Self.runs + 1)
        }
        var fetching: [String] = []
        let fetchedDraws = {
            let counters = RawFacade.counters(of: gpu.device)
            return Self.fetchCounters.reduce(0) { $0 + (counters[$1] ?? 0) }
        }
        for line in plainLines {
            let label = plainKeys[line.shortLabel]!
            let start = fetchedDraws()
            let (path, _) = try await FetchRungScene.bytes(line, library: library, gpu: gpu)
            let fetched = fetchedDraws() - start
            metrics.set("fetched_\(label)", fetched)
            metrics.set("path_\(label)", path)
            if fetched != 0 || path != "raw" { fetching.append(label) }
        }
        let taken = (RawFacade.counters(of: gpu.device)[key] ?? 0) - before
        metrics.set("r\(rung)", taken)
        metrics.set("forced_draws", forcedDraws)
        metrics.set("states", lines.count + plainLines.count)
        let failing = Set(outcomes.filter { !$0.ok }.map(\.program))
        let proven = Set(outcomes.map(\.program)).subtracting(failing).sorted()
        metrics.set("proven", proven.isEmpty ? "-" : proven.joined(separator: ","))
        let counted = taken >= forcedDraws
        let verdict = beyond.isEmpty && slower.isEmpty && counted && fetching.isEmpty ? "PASS" : "FAIL"
        metrics.set("verdict", verdict)
        if !beyond.isEmpty { metrics.set("beyond", beyond.joined(separator: ",")) }
        if !fetching.isEmpty { metrics.set("fetching", fetching.joined(separator: ",")) }
        if !slower.isEmpty { metrics.set("slower", slower.joined(separator: ",")) }
        if verdict == "PASS" { return .pass(metrics, proof: gpu.proof) }
        let why =
            !beyond.isEmpty
            ? "beyond the bound"
            : !counted
                ? "the facade drew \(taken) of the forced draws on rung \(rung)"
                : !fetching.isEmpty ? "a state that loads no colour fetched" : "slower than the storage form"
        return .fail(why, metrics)
    }
}
