import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Fetching draws of every kind of storage-form state keep Metal's order inside the pixel interlock on the driver: a
/// plain BGR5A1Unorm state (the default record's R16_UINT view), depth export and discard under each test class
/// (LessEqual with writes, Always, LessEqual read-only, LessEqual with writes and stencil IncrementClamp; the tests=N
/// variant records) and a 4x BGRA8Unorm_sRGB target with and without a 4x depth (StorageFormsCases). One draw a case of
/// N triangles each covering a 16x16 target cleared to (0.25, 0, 0.5, 1), triangle t nearer than the one before, whose
/// fragment writes (dst.r / 2 + k[t], dst.g + 1/31, fract(3 dst.b + k[t]), 1 - dst.a), so a pixel's colour names the
/// order its triangles ran in. Expected colours are an M4 Max's (c38-audit/storage-forms/bgr5a1_probe.mm, probe-out.txt):
/// BGR5A1 texel 0x1030 at N 1, 0x9855 at 2; depth export bgra 207,64,39,255 with depth writes, 202,128,66,255 without;
/// discard 202,128,66,255 where (x ^ y) is even and 124,88,64,0 where odd; 4x sRGB samples 0 and 3 read as linear x 255,
/// N 1/2/8/16 36,8,134,0 / 50,17,181,255 / 44,66,8,255 / 66,131,57,255, with discard on odd pixels 36,8,134,0 /
/// 36,8,134,0 / 67,41,54,0 / 64,91,154,0. A loaded BGR5A1 texel (code c = i / 2 red, 31 - c green, 7c % 32 blue, alpha
/// i % 2 at pixels 0-63) read as float4 and as half4 and written back unchanged gives half(c / 31) in a colour(1) copy
/// (code 1 0x3d042000, 15 0x3ef7c000, 16 0x3f042000). Judged per case: every pixel Apple's (<case>_wrong=, <case>_saw=
/// the first pixel off), drawn on raw, and one fetch_interlocked and no fetch_sequential a draw (not_interlocked= the
/// cases that were not); no command buffer failed over the run, timing included (failed_cbs=0). Reported, not judged:
/// bgr5a1_overlap8 and bgr5a1_overlap16, the texel of 8 and 16 triangles, Apple 951d and a204, raw 951b and a209
/// (Apple's tile holds a half through the pass, raw stores a code at every write); gpu_us_storage_<kind> and
/// gpu_us_twin_<kind>, the fastest of five draws of 64 such triangles at 1280x720 forced to the storage form (probe 4)
/// and to the barrier twin a triangle at a time (probe 5), the variant kinds' twin the same scene with no depth
/// attachment; ratio_<kind> twin over storage, timing_twins= fetch_blend_twin draws.
struct StorageFormsTest: HWTest {
    typealias Cases = StorageFormsCases

    let id = "gpu.gm20b.storage_forms"
    let name = "Fetching draws of every storage-form kind keep Metal's order inside the interlock, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 180 }

    static let kept: Set<String> = Set(
        [
            "bgr5a1_overlap8", "bgr5a1_overlap16", "wrong_cases", "not_interlocked", "interlocked", "sequential",
            "failed_cbs",
        ]
            + Cases.timed.map { "ratio_" + $0.kind }
    )

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let scene = try await Cases.Scene(gpu)
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var routes = Routes()
        for c in Cases.cases {
            do {
                let start = GM20BFacade.counters(of: gpu.device)
                Self.judge(c, try await scene.observe(c), &metrics, &checks)
                routes.note(c.key, start, GM20BFacade.counters(of: gpu.device))
            } catch {
                checks.append(("\(c.key): \(GPU.describe(error))", false))
            }
        }
        for half in [false, true] {
            let key = half ? "bgr5a1_load_half" : "bgr5a1_load_float"
            do {
                let start = GM20BFacade.counters(of: gpu.device)
                Self.judge(key, try await scene.loaded(half: half), &metrics, &checks)
                routes.note(key, start, GM20BFacade.counters(of: gpu.device))
            } catch {
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("wrong_cases", routes.wrong(metrics))
        routes.report(&metrics, &checks)
        metrics.set("readback_paths", gpu.paths.isEmpty ? "-" : String(gpu.paths.text.prefix(80)))
        let judged = GM20BFacade.counters(of: gpu.device)
        await Self.time(scene, &metrics)
        let after = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: after))
        checks.append(FailedCounts.judged(before: before, after: after, own: gpu.paths, &metrics))
        metrics.set("timing_twins", (after["fetch_blend_twin"] ?? 0) - (judged["fetch_blend_twin"] ?? 0))
        metrics.set("host_clocked", gpu.hostClockedRuns)
        metrics.fitLog(within: 720, keeping: Self.kept)
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func judge(
        _ c: Cases.Case, _ seen: Cases.Observation, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]
    ) {
        if let error = seen.error {
            checks.append(("\(c.key): \(GPU.describe(error))", false))
            return
        }
        checks.append(("\(c.key): drawn on raw (\(seen.path))", seen.path == "gm20b"))
        if c.reported {
            let distinct = Set(seen.pixels)
            metrics.set(c.key, distinct.count == 1 ? shown(c, seen.pixels[0]) : "mixed_\(distinct.count)")
            return
        }
        let side = Cases.side
        let off = seen.pixels.indices.filter { seen.pixels[$0] != c.expected($0 % side, $0 / side) }
        metrics.set("\(c.key)_wrong", off.count)
        if let first = off.first {
            metrics.set("\(c.key)_saw", "\(first % side),\(first / side):\(shown(c, seen.pixels[first]))")
        }
        checks.append(("\(c.key): every pixel Apple's", off.isEmpty))
    }

    static func judge(_ key: String, _ seen: Cases.Loaded, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) {
        if let error = seen.error {
            checks.append(("\(key): \(GPU.describe(error))", false))
            return
        }
        checks.append(("\(key): drawn on raw (\(seen.path))", seen.path == "gm20b"))
        metrics.set("\(key)_wrong", seen.lanesOff + seen.texelsChanged)
        metrics.set("\(key)_lanes_off", seen.lanesOff)
        metrics.set("\(key)_texels_changed", seen.texelsChanged)
        checks.append(("\(key): every lane half(c / 31)", seen.lanesOff == 0))
        checks.append(("\(key): the written-back texels unchanged", seen.texelsChanged == 0))
    }

    static func shown(_ c: Cases.Case, _ pixel: [Int]) -> String {
        c.format == .bgr5A1Unorm ? String(format: "%04x", pixel[0]) : pixel.map(String.init).joined(separator: ".")
    }

    static func time(_ scene: Cases.Scene, _ metrics: inout Metrics) async {
        for (kind, storage, twin) in Cases.timed {
            do {
                guard
                    let storageUs = try await FetchRungScene.forcing(
                        4, scene.gpu.device, { try await scene.microseconds(storage) }
                    ),
                    let twinUs = try await FetchRungScene.forcing(
                        5, scene.gpu.device, { try await scene.microseconds(twin) }
                    )
                else {
                    metrics.set("gpu_us", "no_probe_channel")
                    return
                }
                metrics.set("gpu_us_storage_\(kind)", storageUs, digits: 1)
                metrics.set("gpu_us_twin_\(kind)", twinUs, digits: 1)
                metrics.set("ratio_\(kind)", storageUs > 0 ? twinUs / storageUs : 0, digits: 2)
            } catch {
                metrics.set("gpu_us_\(kind)", String(GPU.describe(error).prefix(80)))
            }
        }
    }

    struct Routes {
        var draws = 0, interlocked = 0, sequential = 0
        var off: [String] = []
        var keys: [String] = []

        mutating func note(_ key: String, _ before: [String: Int], _ after: [String: Int]) {
            let moved = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }
            draws += 1
            interlocked += moved("fetch_interlocked")
            sequential += moved("fetch_sequential")
            keys.append(key)
            if moved("fetch_interlocked") != 1 || moved("fetch_sequential") != 0 {
                off.append("\(key):i\(moved("fetch_interlocked"))s\(moved("fetch_sequential"))")
            }
        }

        func wrong(_ metrics: Metrics) -> String {
            let wrong = keys.filter { metrics["\($0)_wrong"].map { $0 != .integer(0) } ?? false }
            return wrong.isEmpty ? "-" : wrong.joined(separator: ",")
        }

        func report(_ metrics: inout Metrics, _ checks: inout [(String, Bool)]) {
            metrics.set("draws", draws)
            metrics.set("interlocked", interlocked)
            metrics.set("sequential", sequential)
            metrics.set("not_interlocked", off.isEmpty ? "-" : String(off.joined(separator: ",").prefix(160)))
            checks.append(("every draw on the storage form: one fetch_interlocked, no fetch_sequential", off.isEmpty))
        }
    }
}
