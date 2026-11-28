import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// learn/deko3d.md F4: what the 3D class's MME shadow RAM does with methods, before any driver path leans on it
/// (deko3d runs TRACK_WITH_FILTER and replays clear-time state with PASSTHROUGH then REPLAY). One blit encoder of raw
/// 3D-class methods (switchMetalProbeEngine:, unbracketed after the first, so no wait or invalidate of the driver's goes
/// through the shadow between them), observed through a probe syncpoint and one-word report-semaphore releases into a
/// write-combined buffer:
/// - filter_steps: the same INCREMENT_SYNC_POINT twice under TRACK_WITH_FILTER moves the syncpoint 1 (the repeat
///   dropped) or 2;
/// - replay: SET_REPORT_SEMAPHORE_A/B/C tracked at slot 1, written at slot 2 under PASSTHROUGH, then written at slot 3
///   under REPLAY before a release: "shadow" (slot 1: the replay sent the tracked values), "kept" (slot 2: the
///   hardware kept the passthrough ones) or "data" (slot 3: the replay sent its own data);
/// - later_exact: a release tracked afterwards at slot 4 lands as written.
/// It ends in TRACK, the mode the driver never changes. FAIL when a release lands nowhere or later state is not exact.
enum X1ShadowRamProbe {
    static let probe = X1Probe(
        id: "gpu.x1.shadow_ram", name: "MME shadow RAM: the filter, replay of tracked state, and state after it",
        probe: "f4.shadow_ram", risky: true, timeout: 60, body: run
    )

    static let shadowControl = 0x0124
    static let (track, trackWithFilter, passthrough, replay) = (0, 1, 2, 3)
    static let semaphoreA = 0x1b00, semaphoreB = 0x1b04, semaphoreC = 0x1b08, semaphoreD = 0x1b0c
    /// OPERATION RELEASE, PIPELINE_LOCATION ALL, REPORT NONE, STRUCTURE_SIZE ONE_WORD
    static let releaseOneWord = 0x1000_F000
    static let incrementSyncPoint = 0x02c8
    static let payloads: [UInt32] = [0x1111_1111, 0x2222_2222, 0x3333_3333, 0x4444_4444]

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        var metrics = Metrics()
        let connection = GPUConnection()
        guard let before = GM20BProbeSyncpoints.read(connection), before.ids.count >= 2 else {
            return GM20BProbeSyncpoints.absence(connection)
        }
        let syncpoint = before.ids[1]
        let slots = 4, rowBytes = 256
        guard let buffer = gpu.device.makeBuffer(
            length: slots * rowBytes, options: [.storageModeShared, .cpuCacheModeWriteCombined]
        ) else { throw TestFailure("no write-combined buffer") }
        memset(buffer.contents(), 0, buffer.length)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .r32Uint, width: rowBytes / 4, height: slots, mipmapped: false
        )
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderWrite]
        guard let written = buffer.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: rowBytes) else {
            throw TestFailure("no texture over the semaphore buffer")
        }
        let slot = { (i: Int) in buffer.gpuAddress + UInt64(i * rowBytes) }
        var methods: [(Int, UInt32)] = [(shadowControl, UInt32(trackWithFilter))]
        methods += [(incrementSyncPoint, syncpoint), (incrementSyncPoint, syncpoint)]
        methods += [(shadowControl, UInt32(track))] + address(slot(0)) + [(semaphoreC, payloads[0])]
        methods += [(shadowControl, UInt32(passthrough))] + address(slot(1)) + [(semaphoreC, payloads[1])]
        methods += [(shadowControl, UInt32(replay))] + address(slot(2)) + [(semaphoreC, payloads[2])]
        methods += [(shadowControl, UInt32(track)), (semaphoreD, UInt32(releaseOneWord))]
        methods += address(slot(3)) + [(semaphoreC, payloads[3]), (semaphoreD, UInt32(releaseOneWord))]
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            for (index, (method, value)) in methods.enumerated() {
                try GM20BProbe.engine(
                    commands,
                    ["kind": "method", "method": method, "value": value, "written": written, "unbracketed": index > 0]
                )
            }
            blit.endEncoding()
        }
        let after = GM20BProbeSyncpoints.read(connection)?.values[1] ?? before.values[1]
        let words = buffer.contents().assumingMemoryBound(to: UInt32.self)
        let landed = (0 ..< slots).map { words[$0 * rowBytes / 4] }
        let filterSteps = Int(after &- before.values[1])
        let replayed = landed[0] == payloads[0] ? "shadow"
            : landed[1] == payloads[1] ? "kept" : landed[2] == payloads[2] ? "data" : "none"
        metrics.set("syncpoint", Int(syncpoint))
        metrics.set("filter_steps", filterSteps)
        metrics.set("filter", filterSteps == 1 ? "dropped" : filterSteps == 2 ? "sent" : "other")
        metrics.set("replay", replayed)
        metrics.set("slots", landed.map { String($0, radix: 16) }.joined(separator: ","))
        metrics.set("later_exact", landed[3] == payloads[3])
        let wrong = [
            "replay_landed_nowhere": replayed == "none" ? 1 : 0,
            "later_not_exact": landed[3] == payloads[3] ? 0 : 1,
            "filter_steps_off": filterSteps == 1 || filterSteps == 2 ? 0 : 1,
        ]
        return ProbeFinding.counted(wrong, paths: gpu.paths.paths.keys.map { $0 }, metrics: metrics)
    }

    /// SET_REPORT_SEMAPHORE_A (address bits 39:32) and B (31:0)
    static func address(_ va: UInt64) -> [(Int, UInt32)] {
        [(semaphoreA, UInt32((va >> 32) & 0xff)), (semaphoreB, UInt32(truncatingIfNeeded: va))]
    }
}
