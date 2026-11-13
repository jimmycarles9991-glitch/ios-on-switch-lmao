import Foundation
import HWTestCore
import HWTestGPU
import Metal
import os

/// The raw facade's probe calls beyond RawMethods (drivers/TegraGM20BMetal/raw/runtime/RawMetalProbe.mm, debug
/// builds): a command buffer's barrier forms (raw_probe_forms.h), where they took effect, 2D-engine work at an open blit
/// encoder (raw_probe_ops.h), compressible private targets, and what the facade holds of a texture.
enum RawProbe {
    static let formsSelector = NSSelectorFromString("switchRawProbeForms:")
    static let takenSelector = NSSelectorFromString("switchRawProbeTaken")
    static let engineSelector = NSSelectorFromString("switchRawProbeEngine:")
    static let compressSelector = NSSelectorFromString("switchRawProbeCompressNext")
    static let factsSelector = NSSelectorFromString("switchRawProbeFacts:")

    static func call(_ object: AnyObject, _ selector: Selector, _ argument: Any? = nil) throws -> String? {
        guard let object = object as? NSObject, object.responds(to: selector) else {
            throw TestFailure(
                "the facade has no \(NSStringFromSelector(selector)) (a release plug-in or another device)")
        }
        let answer = argument == nil ? object.perform(selector) : object.perform(selector, with: argument)
        return answer?.takeUnretainedValue() as? String
    }

    /// The barrier forms of `commands` ("pass=pixel entry=planner ..."), before it commits.
    static func forms(_ commands: MTLCommandBuffer, _ forms: String) throws {
        if let refusal = try call(commands, formsSelector, forms) { throw TestFailure(refusal) }
    }

    /// Where the forms took effect once `commands` completed: passes, entries, firsts, switches, flushes, resolves, ops.
    static func taken(_ commands: MTLCommandBuffer) throws -> [String: Int] {
        fields(try call(commands, takenSelector) ?? "")
    }

    /// 2D-engine work at the blit encoder open on `commands` (RawMetalProbe.mm switchRawProbeEngine: keys).
    static func engine(_ commands: MTLCommandBuffer, _ op: [String: Any]) throws {
        if let refusal = try call(commands, engineSelector, op as NSDictionary) { throw TestFailure(refusal) }
    }

    /// The next private render target `device` makes binds compressible memory of its own.
    static func compressNext(_ device: MTLDevice) throws {
        _ = try call(device, compressSelector)
    }

    /// raw, comptags, block_linear, pitch of `texture`; tiled_cache and compression of the device.
    static func facts(_ device: MTLDevice, _ texture: MTLTexture) throws -> [String: Int] {
        fields(try call(device, factsSelector, texture) ?? "")
    }

    static func fields(_ line: String) -> [String: Int] {
        var out: [String: Int] = [:]
        for field in line.split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1)
            if pair.count == 2, let value = Int(pair[1]) { out[String(pair[0])] = value }
        }
        return out
    }
}

/// One arm's run of a batch: its GPU time, the facade's path and where its forms took effect.
struct ArmBatch {
    var gpuMilliseconds = 0.0
    var path = ""
    var taken: [String: Int] = [:]
}

extension GPU {
    /// One command buffer under `forms`, awaited: its GPU time, path and the forms' effect.
    func run(forms: String, _ encode: (MTLCommandBuffer) throws -> Void) async throws -> ArmBatch {
        var buffer: MTLCommandBuffer?
        let timing = try await run { commands in
            buffer = commands
            if !forms.isEmpty { try RawProbe.forms(commands, forms) }
            try encode(commands)
        }
        let taken = try buffer.map(RawProbe.taken) ?? [:]
        return ArmBatch(gpuMilliseconds: timing.gpuMilliseconds, path: timing.path, taken: taken)
    }
}

extension GPU {
    /// Command buffers under `forms` committed back to back and awaited together, so each starts as the one before
    /// it ends: the last one's GPU time, the first path off the raw path (else raw), and the forms' effect in all.
    func runBackToBack(forms: String, _ encodes: [(MTLCommandBuffer) throws -> Void]) throws -> ArmBatch {
        let buffers = try encodes.map { encode in
            try commit { commands in
                if !forms.isEmpty { try RawProbe.forms(commands, forms) }
                try encode(commands)
            }
        }
        try finish(buffers)
        var batch = ArmBatch()
        if let last = buffers.last { batch.gpuMilliseconds = max(0, last.gpuEndTime - last.gpuStartTime) * 1000 }
        batch.path = buffers.map(RawFacade.path(of:)).first { $0 != "raw" } ?? "raw"
        for buffer in buffers {
            for (key, value) in try RawProbe.taken(buffer) { batch.taken[key, default: 0] += value }
        }
        return batch
    }
}

/// A probe's tallies by cell and arm, and the line fields they make: per cell <cell>=<H|F|B>:<trials>:<mismatched
/// trials> for each relaxed arm, the first difference anywhere, the arms' GPU time, and where the forms took effect.
struct ProbeLedger {
    var tallies: [String: [String: ArmTally]] = [:]
    var cells: [String] = []
    var taken: [String: [String: Int]] = [:]
    var offPath: [String] = []

    func tally(_ cell: String, _ arm: String) -> ArmTally {
        tallies[cell]?[arm] ?? ArmTally()
    }

    mutating func store(_ cell: String, _ arm: String, _ tally: ArmTally, _ batch: ArmBatch) {
        if !cells.contains(cell) { cells.append(cell) }
        var t = tally
        t.gpuMilliseconds += batch.gpuMilliseconds
        t.batches += 1
        tallies[cell, default: [:]][arm] = t
        for (key, value) in batch.taken { taken[arm, default: [:]][key, default: 0] += value }
        if batch.path != "raw" && !offPath.contains(batch.path) { offPath.append(batch.path) }
    }

    /// The fields for relaxed arms `relaxed` against the unguarded arm `none`, prefixed by each arm's name; the verdict
    /// is the first relaxed arm's (the planner's form).
    func finding(relaxed: [String], none: String?, control: String, takenKey: String, rule: String) -> ProbeFinding {
        var metrics = Metrics()
        metrics.set("rule", rule)
        var verdict = ClaimVerdict.inconclusive
        var note = ""
        for (index, arm) in relaxed.enumerated() {
            var verdicts: [CellVerdict] = []
            var cellsText: [String] = []
            var blindCells: [String] = []
            var minTrials = Int.max
            for cell in cells {
                guard let tally = tallies[cell]?[arm] else { continue }
                let v = CellVerdict.of(relaxed: tally, unguarded: none.flatMap { tallies[cell]?[$0] })
                verdicts.append(v)
                if v == .blind { blindCells.append(cell) }
                cellsText.append("\(cell):\(v.rawValue):\(tally.trials):\(tally.mismatchedTrials)")
                minTrials = min(minTrials, tally.trials)
                if let first = tally.first, metrics["\(arm)_first"] == nil {
                    metrics.set("\(arm)_first", "\(cell)@\(first)")
                }
            }
            let claim = CellVerdict.claim(verdicts)
            metrics.set("\(arm)_verdict", claim.rawValue)
            metrics.set("\(arm)_blind_cells", blindCells.isEmpty ? "-" : blindCells.joined(separator: ","))
            metrics.set("\(arm)_cells", cellsText.joined(separator: ","))
            metrics.set("\(arm)_held", verdicts.filter { $0 == .holds }.count)
            metrics.set("\(arm)_failed", verdicts.filter { $0 == .fails }.count)
            metrics.set("\(arm)_blind", verdicts.filter { $0 == .blind }.count)
            metrics.set("\(arm)_min_trials", minTrials == .max ? 0 : minTrials)
            metrics.set("\(arm)_bound95", ProbeConfidence.bound95(trials: minTrials == .max ? 0 : minTrials), digits: 5)
            metrics.set("\(arm)_\(takenKey)", taken[arm]?[takenKey] ?? 0)
            if index == 0 {
                verdict = claim
                if (taken[arm]?[takenKey] ?? 0) == 0 {
                    verdict = .inconclusive
                    note = "the \(arm) form never took effect (\(takenKey)=0)"
                }
            }
        }
        if let none {
            let unguarded = cells.compactMap { tallies[$0]?[none] }
            metrics.set("\(none)_mismatched_trials", unguarded.map(\.mismatchedTrials).reduce(0, +))
            if let first = cells.lazy.compactMap({ c in tallies[c]?[none]?.first.map { "\(c)@\($0)" } }).first {
                metrics.set("\(none)_first", first)
            }
        }
        for arm in [control] + relaxed + (none.map { [$0] } ?? []) {
            let all = cells.compactMap { tallies[$0]?[arm] }
            let ms = all.map(\.gpuMilliseconds).reduce(0, +), trials = all.map(\.trials).reduce(0, +)
            metrics.set("\(arm)_gpu_us_per_trial", trials > 0 ? ms * 1000 / Double(trials) : 0, digits: 1)
        }
        if !offPath.isEmpty {
            verdict = .inconclusive
            note = "not on the raw path: \(offPath.joined(separator: ","))"
        }
        return ProbeFinding(verdict, metrics, note: note)
    }
}
