import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal

/// The kernel makes its spares from fresh pages without zeroing them on the CPU: their pages keep what their last owner
/// left until the copy engine has zeroed them, and only then is a spare ready (drivers/TegraGM20B/TegraGM20BUserClientSpares.cpp).
/// A 24 MiB Shared buffer the GPU pool does not take is a system BO, write-combined, which only a write-combined spare
/// serves. That class has a spare from boot only when the last boot's demand record seeded it (the kernel's paths line
/// names each class it seeded): then the buffer is that spare, every byte of it zero, made under the ceiling a CPU
/// zero-fill of 24 MiB on this thread cannot meet. Without a record (the first boot after an install, or a boot that
/// went down without a sleep or shutdown) the ask misses and teaches the class, and the record the release sets carries
/// it to the next boot: the timing arm is skipped and says so.
struct FreshSparePathTest: GPUPathTest {
    let id = "gpu.path.fresh_spare"
    let name = "A spare made from fresh pages is handed out zeroed by the copy engine, nothing zeroed on the caller"
    let counters: [String] = []
    static let ceilingUs = 1500, bytes = 24 << 20
    static let seed = "spare_seed_wc_24576"

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        var failures: [String] = []
        let seeded: Int?
        switch GPUPaths.read(GPUConnection()) {
        case .counts(let paths): seeded = paths.counts[Self.seed] ?? 0
        case .unreadable: seeded = nil
        }
        let start = DispatchTime.now().uptimeNanoseconds
        let buffer = try gpu.buffer(Self.bytes)
        let madeUs = Int((DispatchTime.now().uptimeNanoseconds - start) / 1000)
        let words = buffer.contents().bindMemory(to: UInt64.self, capacity: Self.bytes / 8)
        var nonzero = 0
        for i in 0 ..< Self.bytes / 8 where words[i] != 0 { nonzero += 1 }
        metrics.set("made_us", madeUs)
        metrics.set("nonzero_words", nonzero)
        metrics.set("seeded", seeded ?? -1)
        if nonzero > 0 {
            failures.append(
                "a new 24 MiB buffer held \(nonzero) nonzero 8-byte words: a spare was handed out before the "
                    + "copy engine zeroed it"
            )
        }
        switch seeded {
        case nil:
            let why = KernelSysctl.isReleaseKernel()
                ? "no path counters: a release kernel"
                : "no path counters (build with --//bazel:stats_gpu)"
            PathParts.current?.skip(why, counters: [])
        case 0:
            PathParts.current?.skip(
                "no demand record seeded the write-combined 24 MiB class this boot (\(madeUs) us): this ask "
                    + "teaches it, and the next boot after a sleep or shutdown is seeded",
                counters: []
            )
        default:
            if madeUs > Self.ceilingUs {
                failures.append(
                    "a 24 MiB buffer took \(madeUs) us, over \(Self.ceilingUs), with the write-combined 24 MiB "
                        + "class seeded: its spare was not ready (the debug kernel's spare-misses line says why)"
                )
            }
        }
        return failures
    }
}
