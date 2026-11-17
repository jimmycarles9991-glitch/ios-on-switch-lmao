import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.zeta_colour judges the raw path's stencil plane route (drivers/TegraGM20BMetal/raw/runtime/raw_zeta_colour.h):
/// one draw of the colour ROP writes a zeta's stencil plane, RG32Uint over Depth32Float_Stencil8 and R8Uint over
/// Stencil8, through a pool image the copy engine fills from the zeta and copies back, and the zeta's depth and stencil
/// tests and its copies by aspect then read what it wrote. ZetaColourCells writes each zeta a pattern, every stencil value
/// and a depth of its own a texel:
///  - resolve: a 4x zeta resolved by sample 0 into a one-sample one holding another pattern, its depth by the depth
///    resolve, its stencil by the route (Depth32Float_Stencil8: word G alone, the depth kept);
///  - turn: a zeta beside a drawable of rotation 1, moved into the pass's plane-shaped stand-in and back by the route's
///    turn copies (the whole texel), its stencil tested and inverted and its depth made nearer in between;
/// Depth32Float_Stencil8 and Stencil8 at 90x60, 96x64 and 1280x720, each zeta then read back by aspect and tested as a
/// zeta (EQUAL against each stencil value and against the pattern's depth). The arms: bits (raw_stencil_bits.h's eight
/// draws, switchRawProbeStencilBits: on, the control) and colour (the build's route); each also runs gpu.raw.stored_msaa
/// (every stencil resolve pick) and gpu.x1.rotated_target's keptStencil at rotations 1 to 4. <arm>_depth, _stencil and
/// _tested count texels off the pattern, _msaa the scene's failures, _kept the drawable's pixels off Metal's store,
/// _errors the parts that threw (a command buffer the GPU failed), _bit_draws and _rop_draws the stencil plane draws of
/// each route, _cells_ms the arm's time over every part (the parts alike but for the route). INCONCLUSIVE where either
/// arm drew a plane by the other's route or none by its own, the colour arm left a turn cell out, or the control is off
/// where the colour arm is; FAIL where the colour arm alone is off. The route holds with claim_verdict=PASS, reopens=0,
/// gpu_after=true and no "fault", "ROP", "LTC" or "TRAP" kernel line in the run; off the facade the arm is the device's
/// own (the test's references held to Metal). Then a diagnostic outside the claim, after gpu_after
/// (ZetaColourDirect.swift): the colour ROP straight over the zeta's own pages.
struct ZetaColourTest: HWTest {
    let id = "gpu.raw.zeta_colour"
    let name = "Stencil planes by the colour ROP over the zeta's memory, read back and tested as a zeta"
    var subsystem: Subsystem { .metal }
    var timeout: TimeInterval { 300 }
    var runsWhenGPUWedged: Bool { true }

    static let sizes = [(90, 60), (96, 64), (1280, 720)]
    static let formats: [MTLPixelFormat] = [.depth32Float_stencil8, .stencil8]

    enum Arm: String {
        case bits, colour, device
    }

    /// The facade's switchRawProbe<name>: set to `on`; false where it has none
    static func probe(_ device: MTLDevice, _ name: String, _ on: Bool) -> Bool {
        let selector = NSSelectorFromString("switchRawProbe\(name):")
        guard let object = device as? NSObject, object.responds(to: selector) else { return false }
        _ = object.perform(selector, with: NSNumber(value: on))
        return true
    }

    /// One arm's counts: texels off the pattern, the scene's failures, keptStencil's pixels off Metal's store, the turn
    /// cells that ran (none where the device has no rotation SPI) and the parts that threw (a command buffer the GPU
    /// failed among them)
    struct ArmResult {
        var count = ZetaColourCells.Count()
        var msaa = 0, kept = 0, turned = 0, errors = 0
        var firstError: String?
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        let raw = kind == "raw"
        if raw && !Self.probe(gpu.device, "StencilBits", false) {
            return .skip("the facade has no eight-draw comparison arm (a release build)")
        }
        defer {
            _ = Self.probe(gpu.device, "StencilBits", false)
            _ = Self.probe(gpu.device, "ZetaDirect", false)
        }
        let cells = ZetaColourCells(gpu: gpu, rotated: try RotatedStates(gpu))
        let before = RawFacade.counters(of: gpu.device)
        var counts: [Arm: [String: Int]] = [:]
        var metrics = Metrics()
        var notes: [String] = []
        var unengaged: [String] = []
        for arm in raw ? [Arm.bits, .colour] : [.device] {
            _ = Self.probe(gpu.device, "StencilBits", arm == .bits)
            let start = RawFacade.counters(of: gpu.device)
            let started = Date()
            let result = await armRun(gpu, cells, seed: arm == .colour ? 40 : 10)
            metrics.set("\(arm)_cells_ms", Date().timeIntervalSince(started) * 1000, digits: 0)
            let end = RawFacade.counters(of: gpu.device)
            let delta = { (key: String) in (end[key] ?? 0) - (start[key] ?? 0) }
            let count: [String: Int] = [
                "depth": result.count.depth, "stencil": result.count.stencil, "tested": result.count.tested,
                "msaa": result.msaa, "kept": result.kept, "errors": result.errors,
            ]
            counts[arm] = count
            for (key, value) in count.sorted(by: { $0.key < $1.key }) { metrics.set("\(arm)_\(key)", value) }
            metrics.set("\(arm)_bit_draws", delta("stencil_bit_draws"))
            metrics.set("\(arm)_rop_draws", delta("zeta_colour_draws"))
            metrics.set("\(arm)_turn_cells", result.turned)
            let found = [result.firstError, result.count.first].compactMap { $0 }.map { "\(arm) \($0)" }
            notes = arm == .colour ? found + notes : notes + found
            if arm == .bits {
                if delta("stencil_bit_draws") == 0 { unengaged.append("the control drew no plane by the eight draws") }
                if delta("zeta_colour_draws") != 0 { unengaged.append("the control drew planes by the colour ROP") }
            } else if arm == .colour {
                if delta("zeta_colour_draws") == 0 { unengaged.append("no stencil plane drawn by the colour ROP") }
                if delta("stencil_bit_draws") != 0 { unengaged.append("stencil planes drawn by the eight draws") }
                if result.turned != Self.formats.count * Self.sizes.count { unengaged.append("turn cells left out") }
            }
        }
        var (verdict, why) =
            raw
            ? Self.verdict(colour: counts[.colour] ?? [:], control: counts[.bits] ?? [:])
            : (ClaimVerdict.inconclusive, "off the facade: the device's own arm")
        let offPath = gpu.paths.paths.keys.filter { $0 != "raw" }.sorted()
        if verdict == .pass && !offPath.isEmpty {
            verdict = .inconclusive
            why = "not on the raw path: " + offPath.joined(separator: ",")
        } else if verdict != .inconclusive && !unengaged.isEmpty {
            verdict = .inconclusive
            why = "arms off their routes: " + unengaged.joined(separator: ", ")
        }
        var base = Metrics()
        base.set("dev", kind)
        base.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let claimed = RawFacade.counters(of: gpu.device)
        base.set("reopens", (claimed["reopens"] ?? 0) - (before["reopens"] ?? 0))
        base.set("gpu_after", await GPUProbe.answers())
        if raw && Self.probe(gpu.device, "ZetaDirect", true) {
            _ = Self.probe(gpu.device, "StencilBits", false)
            let direct = await ZetaColourDirect.run(gpu, cells)
            direct.report(into: &metrics)
            if let first = direct.first { notes.append("direct \(first)") }
            let after = RawFacade.counters(of: gpu.device)
            metrics.set("direct_reopens", (after["reopens"] ?? 0) - (claimed["reopens"] ?? 0))
            metrics.set("direct_gpu_after", await GPUProbe.answers())
        }
        let finding = ProbeFinding(
            verdict, metrics, note: ([why] + notes).filter { !$0.isEmpty }.joined(separator: "; "))
        return finding.outcome(probe: "rop.zeta_colour", base: base, proof: gpu.proof)
    }

    /// The claim from the colour arm's counts against the control's (the eight draws over the same cells): FAIL where
    /// the colour arm is off and the control is not, INCONCLUSIVE where both are (a failure of the cells, not of the
    /// colour ROP), PASS where the colour arm holds
    static func verdict(colour: [String: Int], control: [String: Int]) -> (ClaimVerdict, String) {
        let off = colour.filter { $0.value != 0 }.keys.sorted()
        guard !off.isEmpty else {
            let controlOff = control.filter { $0.value != 0 }.keys.sorted()
            return (.pass, controlOff.isEmpty ? "" : "the control is off: " + controlOff.joined(separator: ","))
        }
        let alone = off.filter { (control[$0] ?? 0) == 0 }
        if !alone.isEmpty { return (.fail, "the colour arm alone is off: " + alone.joined(separator: ",")) }
        return (.inconclusive, "the colour arm and the control are off: " + off.joined(separator: ","))
    }

    /// One arm: every cell, gpu.raw.stored_msaa and keptStencil at each rotation, each part on its own
    private func armRun(_ gpu: GPU, _ cells: ZetaColourCells, seed: UInt32) async -> ArmResult {
        var arm = ArmResult()
        var n: UInt32 = 0
        for format in Self.formats {
            for (w, h) in Self.sizes {
                n += 1
                let at = seed + n, name = "\(ZetaColourCells.name(format))_\(w)x\(h)"
                if let c = await attempt("resolve_\(name)", &arm, { try await cells.resolve(format, w, h, seed: at) }) {
                    arm.count.add(c)
                }
                if let t = await attempt("turn_\(name)", &arm, { try await cells.turn(format, w, h, seed: at + 100) }),
                    let t
                {
                    arm.count.add(t)
                    arm.turned += 1
                }
            }
        }
        arm.msaa =
            await attempt("stored_msaa", &arm) {
                guard let scene = Scenes.all.first(where: { $0.id == "gpu.raw.stored_msaa" }) else {
                    throw TestFailure("no gpu.raw.stored_msaa scene")
                }
                let result = try await scene.run(gpu)
                return result.failed.count + result.problems.count
            } ?? 0
        for turn in 1...4 {
            let rotated = RotatedCells(
                gpu: gpu, states: cells.rotated, turn: turn, width: 96, height: 64, aligned: true)
            arm.kept +=
                await attempt("kept_r\(turn)", &arm) {
                    guard let plane = try rotated.makePlane() else { return 0 }
                    return try await rotated.run(.keptStencil, into: plane).wrong
                } ?? 0
        }
        return arm
    }

    private func attempt<T>(_ part: String, _ arm: inout ArmResult, _ body: () async throws -> T) async -> T? {
        do {
            return try await body()
        } catch {
            arm.errors += 1
            if arm.firstError == nil { arm.firstError = "\(part): \(error)" }
            return nil
        }
    }
}
