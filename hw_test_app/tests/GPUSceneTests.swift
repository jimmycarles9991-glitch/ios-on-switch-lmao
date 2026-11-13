import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A GPU scene (gpu/scenes) as one test, judged on the process's default device alone: its CPU checks, each picture it
/// has no CPU model of against the same picture drawn on a Mac's GPU (core/SceneGoldens.swift; golden_<picture>=exact,
/// close, wrong or missing, or blind: the golden is the cleared target, which fails), and the scene's own judge. Fields carry the device's prefix (raw_ on the facade): checks,
/// failed, wrong pixels, the scene's own measurements, and paths= (every command buffer's path as the facade reported
/// it). raw_cbs=, m2v_cbs=, split_cbs= and m2v_why= are the facade's process-wide counters over the run, framework
/// command buffers included. <prefix>_gpu_clock=host: some of the run's GPU times are the host's window, the facade
/// having read no GPU stamp for them. While the facade still wraps an m2v device (it answers switchM2VDevice) the scene
/// also runs there as a measurement that judges nothing: its m2v_ fields, and m2v_same= the pictures it drew in raw's
/// bytes. Failed comparisons leave got, want and diff PNGs in Documents/gpu/<id>/.
public struct GPUSceneTest: HWTest {
    let scene: any GPUScene
    public let subsystem = Subsystem.metal

    public init(scene: any GPUScene) { self.scene = scene }

    public var id: String { scene.id }
    public var name: String { scene.name }
    public var timeout: TimeInterval { scene.timeout * 2 }

    static let goldens: SceneGoldens? = Bundle.main.url(forResource: "scene_goldens", withExtension: "txt")
        .flatMap { try? String(contentsOf: $0, encoding: .utf8) }.map(SceneGoldens.init)

    public func run(_ context: TestContext) async throws -> Outcome {
        let primary = try GPU.make()
        let kind = RawFacade.kind(of: primary.device)
        var metrics = Metrics()
        metrics.set("dev", kind)
        var reasons: [String] = []
        var notes: [String] = []
        var saved = 0
        var pictures: [ScenePicture] = []
        do {
            let result = try await measure(primary, kind, &metrics)
            reasons += result.reasons().map { "\(kind): \($0.prefix(300))" }
            notes += result.notes.map { "\(kind): \($0)" }
            saved += save(result, kind, context.documents)
            reasons += hold(result.pictures, kind, &metrics, context.documents, &saved)
            pictures = result.pictures
        } catch {
            reasons.append("\(kind): \(error)")
            metrics.set(kind + "_threw", true)
        }
        if let m2v = RawFacade.m2vDevice(of: primary.device) {
            do {
                let result = try await measure(try GPU.make(device: m2v), "m2v", &metrics)
                let same = pictures.filter { ours in
                    result.pictures.contains { $0.label == ours.label && $0.signature.hash == ours.signature.hash }
                }
                if !pictures.isEmpty { metrics.set("m2v_same", "\(same.count)/\(pictures.count)") }
            } catch {
                metrics.set("m2v_threw", true)
            }
        }
        reasons += scene.judge(&metrics)
        if saved > 0 { metrics.set("pngs", saved) }
        if reasons.isEmpty { return .pass(metrics, proof: primary.proof, detail: LogLine.fitted(notes)) }
        return .fail(LogLine.fitted(reasons), metrics, proof: primary.proof)
    }

    /// One run on `gpu`, its fields under `prefix`.
    func measure(_ gpu: GPU, _ prefix: String, _ metrics: inout Metrics) async throws -> SceneResult {
        let before = RawFacade.counters(of: gpu.device)
        let result = try await scene.run(gpu)
        let after = RawFacade.counters(of: gpu.device)
        metrics.set(prefix + "_checks", result.checks.count)
        metrics.set(prefix + "_failed", result.failed.count + result.problems.count)
        metrics.set(prefix + "_wrong", result.wrongPixels)
        metrics.merge(result.metrics, prefix: prefix + "_")
        if gpu.hostClockedRuns > 0 { metrics.set(prefix + "_gpu_clock", "host") }
        metrics.set(prefix + "_paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        if !after.isEmpty { metrics.merge(PathTally.counterMetrics(before: before, after: after)) }
        return result
    }

    /// Each picture against the Mac's golden: a reason for each one wrong or without a golden, its PNG saved.
    func hold(
        _ pictures: [ScenePicture], _ prefix: String, _ metrics: inout Metrics, _ documents: URL, _ saved: inout Int
    ) -> [String] {
        guard !pictures.isEmpty else { return [] }
        guard let goldens = Self.goldens else { return ["scene_goldens.txt is not in the app's bundle"] }
        var reasons: [String] = []
        for picture in pictures {
            let golden = goldens.golden(scene: scene.id, picture: picture.golden)
            if let golden, let blank = picture.blank,
                GoldenVerdict.of(blank, against: golden, metalFormat: picture.metalFormat).holds
            {
                metrics.set("golden_" + picture.label, "blind")
                reasons.append("\(picture.label): blind, the Mac's golden is the cleared target, so no draw changes it")
                continue
            }
            let verdict = GoldenVerdict.of(picture.signature, against: golden, metalFormat: picture.metalFormat)
            metrics.set("golden_" + picture.label, verdict.rawValue)
            if verdict.holds { continue }
            reasons.append(
                verdict == .missing
                    ? "\(picture.label): no Mac golden (bazel run //hw_test_app:scene_goldens)"
                    : "\(picture.label): drew other pixels than the Mac's (\(goldens.header))")
            if save(picture, prefix, documents) { saved += 1 }
        }
        return reasons
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

    func save(_ picture: ScenePicture, _ prefix: String, _ documents: URL) -> Bool {
        let directory = documents.appendingPathComponent("gpu").appendingPathComponent(scene.id)
        let bytes = Signature.blitted(
            picture.bytes, metalFormat: picture.metalFormat, width: picture.width, height: picture.height)
        guard let format = Signature.format(metal: picture.metalFormat),
            let png = format.raster(bytes, width: picture.width, height: picture.height).png()
        else { return false }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return (try? png.write(to: directory.appendingPathComponent("\(prefix)-\(picture.label).png"))) != nil
    }
}
