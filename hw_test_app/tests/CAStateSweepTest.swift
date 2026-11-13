import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Core Animation's states, one per class of the raw pack's corpus (gpu/CAStateSweep.swift), each drawn by the raw
/// facade and compared with the same state drawn by this IPSW's QuartzCore on a Mac's GPU (core/CAGoldens.swift,
/// hw_test_app/goldens). exact= drew the Mac's bytes; close= every 8x8 block's mean within a few steps of the Mac's
/// (another GPU's filtering and rounding); wrong= anything else. A state whose command buffer the facade sent to m2v
/// proves nothing about raw and is counted apart (on_m2v=), as is one the reference lacks (no_golden=: run
/// //hw_test_app:ca_goldens after a harvest). The wrong states' PNGs and every verdict land in
/// Documents/gpu/gpu.ca.sweep/. HWTEST_SWEEP_ALL=1 in the app's environment draws every corpus state.
public struct CAStateSweepTest: HWTest {
    public let id = "gpu.ca.sweep"
    public let name = "Raw facade against a Mac's GPU: Core Animation's corpus states"
    public let subsystem = Subsystem.metal
    public var timeout: TimeInterval { 900 }

    public init() {}

    enum Verdict: String {
        case exact, close, wrong
    }

    public func run(_ context: TestContext) async throws -> Outcome {
        let raw = try GPU.make()
        guard RawFacade.kind(of: raw.device) == "raw" else { return .skip("the default device is not the raw facade") }
        guard let statesURL = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
            let text = try? String(contentsOf: statesURL, encoding: .utf8),
            let goldensURL = Bundle.main.url(forResource: "ca_goldens", withExtension: "txt"),
            let goldenText = try? String(contentsOf: goldensURL, encoding: .utf8)
        else { return .fail("ca_states.log or ca_goldens.txt is not in the app's bundle") }
        let goldens = CAGoldens(goldenText)
        let everyState = ProcessInfo.processInfo.environment["HWTEST_SWEEP_ALL"] == "1"
        let states = CAStateSweep.states(text, everyState: everyState)
        let library = try raw.device.makeLibrary(URL: CAState.library)
        let directory = context.documents.appendingPathComponent("gpu").appendingPathComponent(id)
        try? FileManager.default.removeItem(at: directory)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var counts: [String: Int] = [:]
        var wrongStates: [String] = []
        var listing = ""
        for line in states {
            let verdict = await judge(line, library: library, gpu: raw, goldens: goldens, directory: directory)
            counts[String(verdict.prefix { $0 != " " && $0 != ":" }), default: 0] += 1
            if verdict.hasPrefix(Verdict.wrong.rawValue) { wrongStates.append(line.shortLabel) }
            listing += "\(verdict) \(line.shortLabel) \(line.vertex.name) \(line.fragment.name)\n"
        }
        try? listing.write(to: directory.appendingPathComponent("states.txt"), atomically: true, encoding: .utf8)
        var metrics = Metrics()
        metrics.set("states", states.count)
        for key in ["exact", "close", "wrong", "on_m2v", "no_golden", "skipped", "failed"] {
            metrics.set(key, counts[key] ?? 0)
        }
        metrics.set("golden_states", goldens.states.count)
        if !wrongStates.isEmpty { metrics.set("wrong_states", wrongStates.prefix(12).joined(separator: ",")) }
        let checked = (counts["exact"] ?? 0) + (counts["close"] ?? 0) + (counts["wrong"] ?? 0)
        let proof = raw.proof + " reference: " + goldens.header
        if checked == 0 { return .fail("no state was drawn by the raw path against a reference", metrics) }
        if !wrongStates.isEmpty || counts["failed", default: 0] > 0 {
            let failed = counts["failed", default: 0]
            return .fail(
                "\(wrongStates.count) states drew other pixels than the Mac's, \(failed) failed", metrics, proof: proof)
        }
        return .pass(metrics, proof: proof)
    }

    /// One state's verdict line: exact, close, wrong[: why], "on_m2v <path>", no_golden, "skipped <why>" or
    /// "failed <why>".
    func judge(
        _ line: PipelineLine, library: MTLLibrary, gpu: GPU, goldens: CAGoldens, directory: URL
    ) async -> String {
        do {
            let drawn = try await CAStateSweep.draw(line, library: library, gpu: gpu)
            if drawn.path != "raw" { return "on_m2v \(drawn.path)" }
            let (width, height) = (CAStateSweep.width, CAStateSweep.height)
            for (color, bytes) in zip(line.colors.sorted(by: { $0.index < $1.index }), drawn.attachments)
            where !Signature.padUntouched(
                bytes, metalFormat: color.format, width: width, height: height, sentinel: CAStateSweep.sentinel)
            {
                return "wrong: the blit wrote past plane 0 of c\(color.index)"
            }
            guard let golden = goldens.states[CAStateSweep.key(line)] else { return "no_golden" }
            var verdict = Verdict.exact
            for (color, bytes) in zip(line.colors.sorted(by: { $0.index < $1.index }), drawn.attachments) {
                let ours = Signature(
                    bytes: bytes, metalFormat: color.format, width: CAStateSweep.width, height: CAStateSweep.height)
                switch GoldenVerdict.of(ours, against: golden[color.index], metalFormat: color.format) {
                case .exact: continue
                case .close: verdict = .close
                case .missing: return "no_golden"
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

    /// The raw path's attachment, as a PNG where its format decodes, else its bytes.
    func save(_ line: PipelineLine, color: PipelineLine.Color, bytes: [UInt8], directory: URL) {
        let url = directory.appendingPathComponent("\(line.shortLabel)-c\(color.index)-raw")
        let (width, height) = (CAStateSweep.width, CAStateSweep.height)
        if let format = Signature.format(metal: color.format),
            let png = format.raster(
                Signature.blitted(bytes, metalFormat: color.format, width: width, height: height), width: width,
                height: height
            ).png()
        {
            try? png.write(to: url.appendingPathExtension("png"))
        } else {
            try? Data(bytes).write(to: url.appendingPathExtension("bin"))
        }
    }
}
