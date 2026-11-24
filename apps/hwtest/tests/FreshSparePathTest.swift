import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The kernel makes a new spare without zeroing it on the CPU: its pages keep what their last owner left until the
/// copy engine has zeroed them, and only then is it ready (drivers/TegraGM20B/TegraGM20BUserClientSpares.cpp). A
/// 1792x1472 RGBA16F target takes the 24 MiB class's spare made at boot; while it is held, the refill makes the next one
/// from fresh pages, and a 24 MiB shared buffer made after it had time to come back from the copy engine is that spare:
/// every byte of it zero, made under the ceiling a zero-fill of 24 MiB on this thread cannot meet. A miss is named in
/// the debug kernel's spare-misses line (another process may have just taken the class's spare).
struct FreshSparePathTest: GPUPathTest {
    let id = "gpu.path.fresh_spare"
    let name = "A spare made from fresh pages is handed out zeroed by the copy engine, nothing zeroed on the caller"
    let counters: [String] = []
    static let ceilingUs = 1500, width = 1792, height = 1472, bytes = 24 << 20
    static let refillNs: UInt64 = 200_000_000

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let held = try gpu.texture(.rgba16Float, Self.width, Self.height)
        try await Task.sleep(nanoseconds: Self.refillNs)
        let start = DispatchTime.now().uptimeNanoseconds
        let buffer = try gpu.buffer(Self.bytes)
        let madeUs = Int((DispatchTime.now().uptimeNanoseconds - start) / 1000)
        let words = buffer.contents().bindMemory(to: UInt64.self, capacity: Self.bytes / 8)
        var nonzero = 0
        for i in 0 ..< Self.bytes / 8 where words[i] != 0 { nonzero += 1 }
        withExtendedLifetime(held) {}
        metrics.set("made_us", madeUs)
        metrics.set("nonzero_words", nonzero)
        var failures: [String] = []
        if nonzero > 0 {
            failures.append(
                "a new 24 MiB buffer held \(nonzero) nonzero 8-byte words: a spare was handed out before the "
                    + "copy engine zeroed it"
            )
        }
        if madeUs > Self.ceilingUs {
            failures.append(
                "a 24 MiB buffer took \(madeUs) us, over \(Self.ceilingUs): no spare of its class was ready (the "
                    + "debug kernel's spare-misses line says why)"
            )
        }
        return failures
    }
}
