import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A state that fetches its own colour, by its storage form (the fetch, and the pipeline's blend where it has one, inside
/// the pixel interlock) against its barrier twin (the colour read through the texture unit behind the fetch barrier, the
/// ROP blending), both forced through the facade's probe channel (raw_fetch_rungs.h): the twin a triangle at a time
/// (twin1) and as one draw behind one barrier (twinW). Per state, keyed by its program's size: worst_<arm>_<scene> the
/// largest difference in codes from the storage form's colour, on convex quads (CAStateSweep's strip and rectangle), a
/// strip folded back over its first triangle and a quad crossing w = 0 under perspective; us_<arm>_<target>_<quads> one
/// draw's GPU time from the facade's stamps, `quads` overlapping full-target quads into 64x64 or 1280x720. twins= the
/// draws the facade counted on a twin. verdict=PASS: twin1 within the bound everywhere and twinW within it on the convex
/// quads, every forced draw counted; the folded quad's twinW difference is the measurement's proof that it sees a
/// draw's own triangles out of order (folded_seen). Debug builds only (the probe channel).
struct BlendTwinTest: HWTest {
    let id = "gpu.raw.blend_twin"
    let name = "Storage form against barrier twin: codes, ordering and GPU time of Core Animation's fetching states"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 600 }

    static let bound = 2
    static let draws = 8
    static let runs = 5

    /// The program sizes the comparison is about: a small pipeline-blended state (its colour matrix on the destination),
    /// the colour-matrix fragment blend, and harmony (iOS 17's QuartzCore only)
    static let states: [(label: String, key: String)] = [
        ("PBGRABdinXm_Tc3bA2Xhf_Idst", "small"), ("PBGRAXmw_TimgBvcmA3Xhf_Isrc", "vcm"),
        ("PBGRABaddXm_ThrmA2Xhf", "harmony"),
    ]
    static let arms: [(rung: Int, name: String)] = [
        (BlendTwinScene.twinTriangles, "twin1"), (BlendTwinScene.twinWhole, "twinW"),
    ]
    static let sizes: [(name: String, width: Int, height: Int, quads: [Int])] = [
        ("64", 64, 64, [1, 4, 16]), ("720p", 1280, 720, [1]),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else { return .skip("the default device is not the raw facade") }
        guard let statesURL = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
            let text = try? String(contentsOf: statesURL, encoding: .utf8)
        else { return .fail("ca_states.log is not in the app's bundle") }
        let keys = Dictionary(uniqueKeysWithValues: Self.states.map { ($0.label, $0.key) })
        let lines = CAStateSweep.states(text, everyState: true).filter { keys[$0.shortLabel] != nil }
        if lines.isEmpty { return .skip("this QuartzCore makes none of the states") }
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        var metrics = Metrics()
        var failures: [String] = []
        var unseen: [String] = []
        let before = RawFacade.counters(of: gpu.device)["fetch_blend_twin"] ?? 0
        var forcedDraws = 0
        for line in lines {
            let key = keys[line.shortLabel]!
            guard let colour = line.colors.sorted(by: { $0.index < $1.index }).firstIndex(where: { $0.writeMask != 0 })
            else { continue }
            let scenes: [(name: String, draws: [[CAState.Vertex]])] = [
                ("convex", BlendTwinScene.convex()), ("folded", BlendTwinScene.folded()),
                ("perspective", BlendTwinScene.perspectiveCrossing()),
            ]
            for scene in scenes {
                let draw = {
                    try await BlendTwinScene.bytes(line, colour: colour, draws: scene.draws, library: library, gpu: gpu)
                }
                guard let storage = try await FetchRungScene.forcing(BlendTwinScene.storage, gpu.device, draw) else {
                    return .skip("the facade has no probe channel (a release build)")
                }
                for arm in Self.arms {
                    guard let twin = try await FetchRungScene.forcing(arm.rung, gpu.device, draw) else { continue }
                    forcedDraws += scene.draws.count
                    let (worst, differing) = FetchRungScene.difference(storage.1, twin.1)
                    metrics.set("worst_\(arm.name)_\(scene.name)_\(key)", worst)
                    metrics.set("differing_\(arm.name)_\(scene.name)_\(key)", differing)
                    let mustHold = arm.rung == BlendTwinScene.twinTriangles || scene.name == "convex"
                    if mustHold && worst > Self.bound { failures.append("\(key) \(arm.name) \(scene.name)") }
                    if arm.rung == BlendTwinScene.twinWhole && scene.name == "folded" && worst <= Self.bound {
                        unseen.append(key)
                    }
                }
            }
            for size in Self.sizes {
                for quads in size.quads {
                    for (rung, arm) in [(BlendTwinScene.storage, "storage")] + Self.arms.map({ ($0.rung, $0.name) }) {
                        let us = try await FetchRungScene.forcing(rung, gpu.device) {
                            try await BlendTwinScene.microseconds(
                                line, quads: quads, width: size.width, height: size.height, library: library, gpu: gpu,
                                draws: Self.draws, runs: Self.runs)
                        }
                        if rung != BlendTwinScene.storage { forcedDraws += Self.draws * (Self.runs + 1) }
                        metrics.set("us_\(arm)_\(size.name)_\(quads)_\(key)", (us ?? nil) ?? 0, digits: 1)
                    }
                }
            }
        }
        let twins = (RawFacade.counters(of: gpu.device)["fetch_blend_twin"] ?? 0) - before
        metrics.set("twins", twins)
        metrics.set("forced_draws", forcedDraws)
        metrics.set("states", lines.count)
        metrics.set("folded_seen", unseen.isEmpty ? "all" : "not:" + unseen.joined(separator: ","))
        let counted = twins >= forcedDraws
        let verdict = failures.isEmpty && unseen.isEmpty && counted ? "PASS" : "FAIL"
        metrics.set("verdict", verdict)
        if !failures.isEmpty { metrics.set("beyond", failures.joined(separator: ",")) }
        if verdict == "PASS" { return .pass(metrics, proof: gpu.proof) }
        let why =
            !failures.isEmpty
            ? "a twin beyond the bound"
            : !unseen.isEmpty
                ? "the folded quad showed no difference on the whole twin draw"
                : "the facade drew \(twins) of \(forcedDraws) forced draws on a twin"
        return .fail(why, metrics)
    }
}
