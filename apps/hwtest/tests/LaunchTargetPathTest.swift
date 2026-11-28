import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// 22 Nov, boot B: Photos' launch made a 1792x1472 RGBA16F attachment in backboardd, a block-linear chunk of its own
/// of the 24 MiB class, which the kernel kept no zeroed spare of: 17.3 ms of allocation, zero-fill and clean on the
/// render thread. That class is now kept ready from boot (drivers/TegraGM20B/SwitchGpuSpares.h), and an app's chunks
/// are the kernel's spares too (drivers/TegraGM20BMetal/memory.cpp). The same target made here, cleared and freed,
/// twice, the second after its class's spare had time to come back from the copy engine: neither making imports
/// anything, and each finishes under the ceiling a zero-fill of its 24 MiB on this thread cannot meet. A miss is
/// named in the debug kernel's spare-misses line (another process may have just taken the class's spare).
struct LaunchTargetPathTest: GPUPathTest {
    let id = "gpu.path.launch_target"
    let name = "A launch-sized render target is a ready spare of its class, nothing zeroed on the caller"
    let counters: [String] = []
    static let ceilingUs = 1500, width = 1792, height = 1472, rounds = 2, refillNs: UInt64 = 200_000_000

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let before = GM20BFacade.memory(of: gpu.device)
        guard before["imports_made"] != nil else {
            throw TestFailure(GM20BFacade.missing("imports_made", on: gpu.device))
        }
        var slowestUs = 0
        for round in 0 ..< Self.rounds {
            if round > 0 { try await Task.sleep(nanoseconds: Self.refillNs) }
            let start = DispatchTime.now().uptimeNanoseconds
            let target = try gpu.texture(.rgba16Float, Self.width, Self.height)
            slowestUs = max(slowestUs, Int((DispatchTime.now().uptimeNanoseconds - start) / 1000))
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
        }
        let imports = (GM20BFacade.memory(of: gpu.device)["imports_made"] ?? 0) - (before["imports_made"] ?? 0)
        metrics.set("slowest_us", slowestUs)
        metrics.set("imports", imports)
        var failures: [String] = []
        if imports > 0 { failures.append("making the target imported \(imports) times") }
        if slowestUs > Self.ceilingUs {
            failures.append(
                "a \(Self.width)x\(Self.height) RGBA16F target took \(slowestUs) us, over \(Self.ceilingUs): its "
                    + "memory was made on the calling thread (the debug kernel's spare-misses line says why)"
            )
        }
        return failures
    }
}
