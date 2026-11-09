import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A GPU scene (gpu/scenes) as one test, run on the process's default device and, where that is the raw facade, again
/// on the m2v device it wraps. Each run's fields carry its device's prefix (raw_, m2v_): checks, failed, wrong pixels,
/// the scene's own measurements, and paths= (every command buffer's path as the facade reported it). raw_cbs=,
/// m2v_cbs=, split_cbs= and m2v_why= are the facade's process-wide counters over its run, framework command buffers
/// included. A hash_* field the two runs disagree on fails the test: the two paths drew different pixels. Failed
/// comparisons leave got, want and diff PNGs in Documents/gpu/<id>/.
public struct GPUSceneTest: HWTest {
    let scene: any GPUScene
    public let subsystem = Subsystem.metal

    public init(scene: any GPUScene) { self.scene = scene }

    public var id: String { scene.id }
    public var name: String { scene.name }
    public var timeout: TimeInterval { scene.timeout * 2 }

    public func run(_ context: TestContext) async throws -> Outcome {
        let primary = try GPU.make()
        let kind = RawFacade.kind(of: primary.device)
        var runs = [(kind, primary)]
        if let m2v = RawFacade.m2vDevice(of: primary.device) { runs.append(("m2v", try GPU.make(device: m2v))) }
        var metrics = Metrics()
        metrics.set("dev", kind)
        var reasons: [String] = []
        var notes: [String] = []
        var hashes: [String: Set<String>] = [:]
        var saved = 0
        for (prefix, gpu) in runs {
            let before = RawFacade.counters(of: gpu.device)
            let result: SceneResult
            do {
                result = try await scene.run(gpu)
            } catch {
                reasons.append("\(prefix): \(error)")
                metrics.set(prefix + "_threw", true)
                continue
            }
            let after = RawFacade.counters(of: gpu.device)
            metrics.set(prefix + "_checks", result.checks.count)
            metrics.set(prefix + "_failed", result.failed.count + result.problems.count)
            metrics.set(prefix + "_wrong", result.wrongPixels)
            metrics.merge(result.metrics, prefix: prefix + "_")
            metrics.set(prefix + "_paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
            if !after.isEmpty { metrics.merge(PathTally.counterMetrics(before: before, after: after)) }
            for key in result.metrics.keys where key.hasPrefix("hash_") {
                hashes[key, default: []].insert(result.metrics[key]!.logText)
            }
            reasons += result.reasons().map { "\(prefix): \($0.prefix(300))" }
            notes += result.notes.map { "\(prefix): \($0)" }
            saved += save(result, prefix, context.documents)
        }
        for (key, values) in hashes.sorted(by: { $0.key < $1.key }) where values.count > 1 {
            reasons.append("raw and m2v drew different pixels (\(key))")
        }
        if saved > 0 { metrics.set("pngs", saved) }
        if reasons.isEmpty { return .pass(metrics, proof: primary.proof, detail: LogLine.fitted(notes)) }
        return .fail(LogLine.fitted(reasons), metrics, proof: primary.proof)
    }

    /// The first failed comparisons' PNGs; how many were written.
    func save(_ result: SceneResult, _ prefix: String, _ documents: URL) -> Int {
        let directory = documents.appendingPathComponent("gpu").appendingPathComponent(scene.id)
        var written = 0
        for (index, check) in result.failed.prefix(6).enumerated() where check.got.width > 0 {
            if (try? check.save(to: directory, stem: "\(prefix)-\(index)")) != nil { written += 1 }
        }
        if written > 0 {
            let listing = result.failed.map(\.line).joined(separator: "\n") + "\n"
            try? listing.write(to: directory.appendingPathComponent("\(prefix).txt"), atomically: true, encoding: .utf8)
        }
        return written
    }
}
