import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.invalid_requests's arms of requests Metal's validation stops (gpu/MirroredRequests.swift), each in a counter
/// window of its own. units, which the driver does not mirror: RGBA16Float copied 16x16 into an RGBA8Unorm of 0x22
/// bytes, formats of other copy units, between two pattern copies: neighbours 0, the destination as it was, skipped
/// call.blit 1 (an M4 Max under macOS 27.2.1 writes ff 00 ff ff there at these 64-texel sides and other bytes at
/// others, by size and by whether the CPU or the GPU wrote the source: the units-zeta probe). The rest run as the M4
/// Max runs them with validation off (each scene run on it, three processes alike): indices_off: an indirect command's
/// indexed draw of UInt32 indices {0, 1, 4, 5, 15, 15} at offset 2 and of UInt16 ones at offset 3 reads them from the
/// offset rounded down to its type: each image equals the same draw's at offset 0 and 2 (indices_off_u32_differ,
/// _u16_differ 0; the M4 Max covers 120 and 136 pixels: _u32_red, _u16_red, reported), skipped call.icb 2 (the controls
/// none). resolve_off: four timestamp samples resolved at offset 2 of a buffer of 0x22 bytes land as the same resolve
/// at offset 0 does, two bytes on, bytes 0-1 and from 34 on as they were, skipped call.counter 1 (resolve_off_stamps:
/// the resolved words that hold a sample, 2 on the M4 Max, reported). range_off: an execute of four draws (draw i in
/// quadrant i) whose range {0, 4} lies at byte 0 of a 4096-byte buffer, read at offset 2: the M4 Max reads it from the
/// offset rounded down to 4 bytes, so the command buffer completes with no error, all four commands draw
/// (range_off_drawn 0xf) and the blits before and after it write their 4096 bytes; skipped call.icb 1. range_past: the
/// same execute reading a 4096-byte buffer of zeros at offset 4092, 4 bytes of the range inside it: the M4 Max reads
/// what lies past the buffer, here a zeroed placement heap's (a range of no command), and draws nothing (0); the raw
/// path reads nothing past a buffer and runs none, skipped call.icb 1, the rest as range_off. layers_past: a clear-only
/// pass of renderTargetArrayLength 3 into a BGRA8Unorm array of 3 layers beside an A1BGR5Unorm array of 2 (a format the
/// raw path renders through a stand-in), run twice: both raw, the layers both arrays have cleared, skipped pass.level
/// 2: the M4 Max clears each attachment's own layers, the driver the layers every attachment has, so the BGRA8's
/// third holds its clear on the M4 Max and its fill on raw (layers_past_bgra8_l2: 1 cleared, 0 its fill, reported).
/// layers_plain: the BGRA8Unorm array alone from slice 1: slices 1 and 2 cleared, slice 0 its fill, skipped none.
extension InvalidRequestsTest {
    func mirrored(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async -> [FailedAttribution] {
        var attributions: [FailedAttribution] = []
        let arms: [(String, [String: Int], [String: Int], Arm)] = [
            ("units", [:], ["call.blit": 1], Self.units),
            ("indices_off", [:], ["call.icb": 2], Self.indicesOff),
            ("resolve_off", [:], ["call.counter": 1], Self.resolveOff),
            ("range_off", [:], ["call.icb": 1], Self.rangeOff),
            ("range_past", [:], ["call.icb": 1], Self.rangePast),
            ("layers_past", [:], ["pass.level": 2], Self.layersPast),
            ("layers_plain", [:], [:], Self.layersPlain),
        ]
        for (key, expected, skipped, arm) in arms {
            let before = GM20BFacade.counters(of: gpu.device)
            let paths = gpu.paths
            do {
                try await arm(gpu, &metrics, &checks)
            } catch {
                metrics.set(key + "_path", "failed")
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
            let counts = FailedCounts(before: before, after: GM20BFacade.counters(of: gpu.device))
            attributions.append(
                Self.judge(
                    key, counts, own: gpu.paths.since(paths), expecting: expected, skipped: skipped, &metrics, &checks
                )
            )
        }
        return attributions
    }

    typealias Arm = (GPU, inout Metrics, inout [(String, Bool)]) async throws -> Void

    static func units(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let r = try await MirroredRequests.units(gpu)
        let side = MirroredRequests.side
        let off = (0 ..< side * side).filter { i in r.texels[(4 * i) ..< (4 * i + 4)].contains { $0 != 0x22 } }.count
        metrics.set("units_off", off)
        report(
            "units", path: r.path, neighbours: r.neighbours, untouched: off == 0,
            label: "its destination as it was (units_off=\(off))", &metrics, &checks
        )
    }

    static func indicesOff(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        for (name, type, offset, control) in [("u32", MTLIndexType.uint32, 2, 0), ("u16", .uint16, 3, 2)] {
            let odd = try await MirroredRequests.indexed(gpu, type, offset: offset)
            let aligned = try await MirroredRequests.indexed(gpu, type, offset: control)
            let differ = zip(odd.red, aligned.red).filter { $0 != $1 }.count
            let key = "indices_off_" + name
            metrics.set(key + "_path", String(odd.path.prefix(80)))
            metrics.set(key + "_red", odd.redPixels)
            metrics.set(key + "_differ", differ)
            checks.append(("\(key): both draws on the driver", odd.path == "gm20b" && aligned.path == "gm20b"))
            checks.append(("\(key): drew a triangle (\(odd.redPixels) pixels)", odd.redPixels > 0))
            checks.append(("\(key): \(differ) pixels off the draw at offset \(control)", differ == 0))
        }
    }

    static func resolveOff(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let r = try await MirroredRequests.resolveOff(gpu)
        let moved = Array(r.atTwo[2 ..< 34]) == Array(r.atZero[0 ..< 32])
        let kept = r.atTwo[0 ..< 2].allSatisfy { $0 == 0x22 } && r.atTwo[34...].allSatisfy { $0 == 0x22 }
        let stamps = stride(from: 0, to: 32, by: 8).filter { k in r.atZero[k ..< (k + 8)].contains { $0 != 0 } }.count
        metrics.set("resolve_off_path", r.paths.allSatisfy { $0 == "gm20b" } ? "gm20b" : r.paths.joined(separator: ","))
        metrics.set("resolve_off_moved", moved)
        metrics.set("resolve_off_kept", kept)
        metrics.set("resolve_off_stamps", stamps)
        checks.append(("resolve_off: every command buffer raw", r.paths.allSatisfy { $0 == "gm20b" }))
        checks.append(("resolve_off: bytes 2 to 33 the resolve at offset 0's 32", moved))
        checks.append(("resolve_off: bytes 0-1 and from 34 on as they were", kept))
    }

    static func rangeOff(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        executed("range_off", try await MirroredRequests.rangeOff(gpu), drawn: 0xf, &metrics, &checks)
    }

    static func rangePast(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        executed("range_past", try await MirroredRequests.rangePast(gpu), drawn: 0, &metrics, &checks)
    }

    /// Completed with no error on the driver, the commands that drew `drawn`, both blits' 4096 bytes written
    static func executed(
        _ key: String, _ r: MirroredRequests.Executed, drawn: Int, _ metrics: inout Metrics,
        _ checks: inout [(String, Bool)]
    ) {
        let hex = { (mask: Int) in "0x" + String(mask, radix: 16) }
        metrics.set(key + "_path", r.path)
        metrics.set(key + "_error", r.error)
        metrics.set(key + "_drawn", hex(r.drawn))
        metrics.set(key + "_before", r.beforeWritten)
        metrics.set(key + "_after", r.afterWritten)
        let completed = Int(MTLCommandBufferStatus.completed.rawValue)
        checks.append(("\(key): status \(r.status), error \(r.error)", r.status == completed && r.error == "none"))
        checks.append(("\(key): on the \(r.path) path", r.path == "gm20b"))
        checks.append(("\(key): drew \(hex(r.drawn)), not \(hex(drawn))", r.drawn == drawn))
        let all = MirroredRequests.bytes
        checks.append(
            (
                "\(key): the blits around it wrote \(r.beforeWritten) and \(r.afterWritten) of \(all)",
                r.beforeWritten == all && r.afterWritten == all
            )
        )
    }

    static func layersPast(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let r = try await MirroredRequests.layered(
            gpu, [(.bgra8Unorm, 3, 4), (.a1bgr5Unorm, 2, 2)], slice: 0, runs: 2
        )
        metrics.set("layers_past_path", r.paths.allSatisfy { $0 == "gm20b" } ? "gm20b" : r.paths.joined(separator: ","))
        metrics.set("layers_past_bgra8_l2", r.cleared[0][2])
        checks.append(("layers_past: both passes raw", r.paths.allSatisfy { $0 == "gm20b" }))
        let drawnLayers = r.cleared[0][0 ..< 2].allSatisfy { $0 == 1 }
        checks.append(("layers_past: BGRA8 layers 0-1 cleared (\(r.cleared[0]))", drawnLayers))
        checks.append(("layers_past: A1BGR5 layers cleared (\(r.cleared[1]))", r.cleared[1] == [1, 1]))
    }

    static func layersPlain(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let r = try await MirroredRequests.layered(gpu, [(.bgra8Unorm, 3, 4)], slice: 1, runs: 1)
        metrics.set(
            "layers_plain_path",
            r.paths.allSatisfy { $0 == "gm20b" } ? "gm20b" : r.paths.joined(separator: ",")
        )
        checks.append(("layers_plain: the pass raw", r.paths.allSatisfy { $0 == "gm20b" }))
        checks.append(("layers_plain: slices 1-2 cleared, 0 kept (\(r.cleared[0]))", r.cleared[0] == [0, 1, 1]))
    }
}
