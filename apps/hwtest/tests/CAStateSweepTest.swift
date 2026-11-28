import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Core Animation's states, one per class of the pack's corpus (gpu/CAStateSweep.swift), each drawn by the raw
/// facade and compared with the same state drawn by this IPSW's QuartzCore on a Mac's GPU (core/CAGoldens.swift,
/// apps/hwtest/goldens). exact= drew the Mac's bytes; close= every 8x8 block's mean within one code of the Mac's
/// (another GPU's filtering and rounding); coarse= close but some block more than one code off; wrong= anything else.
/// coarse and wrong fail: a specialised state draws the Mac's picture within one code. Each class's state is also
/// timed as one full-screen 1280x720 pass (ms720= in states.txt; pass720_* metrics: the median, the costliest and
/// its label), unless every state is drawn. A state whose command buffer left the driver failed
/// (failed=, its path in states.txt); one the reference lacks is counted apart (no_golden=: redraw the reference with
/// //apps/hwtest:ca_goldens after a harvest), and one whose reference line names no signature fails
/// (malformed_golden=). A reference drawn on another GPU than CAGoldens.referenceGPU fails the test before any draw. The wrong states' PNGs and every verdict land in
/// Documents/gpu/gpu.ca.sweep/. HWTEST_SWEEP_ALL=1 in the app's environment draws every corpus state.
public struct CAStateSweepTest: HWTest {
    public let id = "gpu.ca.sweep"
    public let name = "The facade against a Mac's GPU: Core Animation's corpus states"
    public let subsystem = Subsystem.metal
    public var timeout: TimeInterval { 900 }

    public init() {}

    enum Verdict: String {
        case exact, close, coarse, wrong
    }

    public func run(_ context: TestContext) async throws -> Outcome {
        let raw = try GPU.make()
        guard GM20BFacade.kind(of: raw.device) == "gm20b" else { return .skip("the default device is not the facade") }
        guard let statesURL = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
              let text = try? String(contentsOf: statesURL, encoding: .utf8),
              let goldensURL = Bundle.main.url(forResource: "ca_goldens", withExtension: "txt"),
              let goldenText = try? String(contentsOf: goldensURL, encoding: .utf8)
        else { return .fail("ca_states.log or ca_goldens.txt is not in the app's bundle") }
        let goldens = CAGoldens(goldenText)
        guard goldens.gpu == CAGoldens.referenceGPU else {
            return .fail(
                "the reference was drawn on \(goldens.gpu ?? "no named GPU") (\(goldens.header)), not "
                    + "\(CAGoldens.referenceGPU): another GPU moves a third of the states by a code"
            )
        }
        let everyState = ProcessInfo.processInfo.environment["HWTEST_SWEEP_ALL"] == "1"
        let states = CAStateSweep.states(text, everyState: everyState)
        let library = try raw.device.makeLibrary(URL: CAState.library)
        let directory = context.documents.appendingPathComponent("gpu").appendingPathComponent(id)
        try? FileManager.default.removeItem(at: directory)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var counts: [String: Int] = [:]
        var wrongStates: [String] = []
        var coarseStates: [String] = []
        var costs: [(label: String, ms: Double)] = []
        var listing = ""
        for line in states {
            var verdict = await judge(line, library: library, gpu: raw, goldens: goldens, directory: directory)
            let kind = String(verdict.prefix { $0 != " " && $0 != ":" })
            counts[kind, default: 0] += 1
            if kind == Verdict.wrong.rawValue { wrongStates.append(line.shortLabel) }
            if kind == Verdict.coarse.rawValue { coarseStates.append(line.shortLabel) }
            if !everyState, kind != "skipped", kind != "failed" {
                do {
                    let ms = try await CAStateSweep.passMilliseconds(line, library: library, gpu: raw)
                    costs.append((line.shortLabel, ms))
                    verdict += String(format: " ms720=%.3f", ms)
                } catch {
                    counts["failed", default: 0] += 1
                    verdict += " ms720=failed \(error)"
                }
            }
            listing += "\(verdict) \(line.shortLabel) \(line.vertex.name) \(line.fragment.name)\n"
        }
        try? Data(listing.utf8).place(at: directory.appendingPathComponent("states.txt"))
        var metrics = Metrics()
        metrics.set("states", states.count)
        for key in ["exact", "close", "coarse", "wrong", "no_golden", "malformed_golden", "skipped", "failed"] {
            metrics.set(key, counts[key] ?? 0)
        }
        metrics.set("golden_states", goldens.states.count)
        if !wrongStates.isEmpty { metrics.set("wrong_states", wrongStates.prefix(12).joined(separator: ",")) }
        if !coarseStates.isEmpty { metrics.set("coarse_states", coarseStates.prefix(12).joined(separator: ",")) }
        if let worst = costs.max(by: { $0.ms < $1.ms }) {
            let sorted = costs.map(\.ms).sorted()
            metrics.set("pass720_states", costs.count)
            metrics.set("pass720_median_ms", String(format: "%.3f", sorted[sorted.count / 2]))
            metrics.set("pass720_max_ms", String(format: "%.3f", worst.ms))
            metrics.set("pass720_worst", worst.label)
        }
        let checked = ["exact", "close", "coarse", "wrong"].reduce(0) { $0 + (counts[$1] ?? 0) }
        let proof = raw.proof + " reference: " + goldens.header
        if checked == 0 { return .fail("no state was drawn by the driver against a reference", metrics) }
        let malformed = counts["malformed_golden", default: 0]
        if !wrongStates.isEmpty || !coarseStates.isEmpty || counts["failed", default: 0] > 0 || malformed > 0 {
            let failed = counts["failed", default: 0]
            return .fail(
                "\(wrongStates.count) states drew other pixels than the Mac's, \(coarseStates.count) more than one "
                    + "code off it, \(failed) failed, \(malformed) golden lines name no signature",
                metrics, proof: proof
            )
        }
        return .pass(metrics, proof: proof)
    }

    /// One state's verdict line: exact, close, coarse, wrong[: why], no_golden, malformed_golden, "skipped <why>" or
    /// "failed <why>" (a command buffer off the driver among them).
    func judge(
        _ line: PipelineLine, library: MTLLibrary, gpu: GPU, goldens: CAGoldens, directory: URL
    ) async -> String {
        do {
            let drawn = try await CAStateSweep.draw(line, library: library, gpu: gpu)
            if drawn.path != "gm20b" { return "failed off the driver: \(drawn.path)" }
            let (width, height) = (CAStateSweep.width, CAStateSweep.height)
            for (color, bytes) in zip(line.colors.sorted(by: { $0.index < $1.index }), drawn.attachments)
                where !Signature.padUntouched(
                    bytes, metalFormat: color.format, width: width, height: height, sentinel: CAStateSweep.sentinel
                )
            {
                return "wrong: the blit wrote past plane 0 of c\(color.index)"
            }
            if goldens.malformed[CAStateSweep.key(line)] != nil { return "malformed_golden" }
            guard let golden = goldens.states[CAStateSweep.key(line)] else { return "no_golden" }
            var verdict = Verdict.exact
            for (color, bytes) in zip(line.colors.sorted(by: { $0.index < $1.index }), drawn.attachments) {
                let ours = Signature(
                    bytes: bytes, metalFormat: color.format, width: CAStateSweep.width, height: CAStateSweep.height
                )
                switch GoldenVerdict.of(ours, against: golden[color.index], metalFormat: color.format) {
                case .exact: continue
                case .close:
                    if let reference = golden[color.index],
                       !CAGoldens.withinOneCode(ours, against: reference, metalFormat: color.format)
                    {
                        verdict = .coarse
                    } else if verdict == .exact {
                        verdict = .close
                    }
                case .missing: return "no_golden"
                case .malformed: return "malformed_golden"
                case .wrong:
                    save(line, color: color, bytes: bytes, directory: directory)
                    return Verdict.wrong.rawValue
                }
            }
            return verdict.rawValue
        } catch let unsupported as CAStateSweep.Unsupported {
            return "skipped \(unsupported.description)"
        } catch {
            return "failed \(error)"
        }
    }

    /// The driver's attachment, as a PNG where its format decodes, else its bytes.
    func save(_ line: PipelineLine, color: PipelineLine.Color, bytes: [UInt8], directory: URL) {
        let url = directory.appendingPathComponent("\(line.shortLabel)-c\(color.index)-raw")
        let (width, height) = (CAStateSweep.width, CAStateSweep.height)
        if let format = Signature.format(metal: color.format),
           let png = format.raster(
               Signature.blitted(bytes, metalFormat: color.format, width: width, height: height), width: width,
               height: height
           ).png()
        {
            try? png.place(at: url.appendingPathExtension("png"))
        } else {
            try? Data(bytes).place(at: url.appendingPathExtension("bin"))
        }
    }
}
