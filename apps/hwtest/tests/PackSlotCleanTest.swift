import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Program slots that run in place from the pack are cleaned from the CPU caches by the kernel at their grain's import
/// and never again (Device::placeInPlace): no process writes the pack, so a pack slot placed in a grain this process
/// bound already skips the clean it took before (~0.1 ms a state's first use). The shader compiler's store slots are
/// cleaned at each placement, since the compiler writes the store after a grain of it may be imported. Makes the Core
/// Animation corpus states (the pack's; this OS's line where both name a label), then reads this process's counters since
/// the device opened: every clean is a store slot's (slot_cleans = store_slots_placed), and some pack slot ran from a
/// bound grain (pack_slots_in_bound_grains).
struct PackSlotCleanTest: HWTest {
    let id = "gpu.raw.pack_slot_clean"
    let name = "Pack program slots in bound grains run with no repeat cache clean; store slots are cleaned"
    var subsystem: Subsystem { .metal }

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let before = RawFacade.counters(of: gpu.device)
        if before["state_segment_binds"] == nil { return .skip("no counters: a release runtime") }
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            return .skip("no QuartzCore library here (\(GPU.describe(error)))")
        }
        var made = 0
        var refused: [String] = []
        var labels = Set<String>()
        for line in CACorpus.all where labels.insert(line.shortLabel).inserted {
            do {
                _ = try CAState.pipeline(line, library: library, device: gpu.device)
                made += 1
            } catch {
                refused.append(line.shortLabel)
            }
        }
        let after = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        metrics.set("states_made", made)
        if !refused.isEmpty { metrics.set("refused", refused.joined(separator: ",")) }
        let keys = ["pack_slots_in_bound_grains", "store_slots_placed", "slot_cleans"]
        guard keys.allSatisfy({ after[$0] != nil }) else {
            return .fail("the runtime reports no program slot counters", metrics)
        }
        for key in keys {
            metrics.set(key, after[key] ?? 0)
            metrics.set(key + "_here", (after[key] ?? 0) - (before[key] ?? 0))
        }
        var failures: [String] = []
        if after["slot_cleans"] != after["store_slots_placed"] {
            failures.append(
                "cleans \(after["slot_cleans"] ?? 0) are not the store's \(after["store_slots_placed"] ?? 0)")
        }
        if (after["pack_slots_in_bound_grains"] ?? 0) == 0 {
            failures.append("no pack slot ran from a grain bound already")
        }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.joined(separator: "; "), metrics)
    }
}
