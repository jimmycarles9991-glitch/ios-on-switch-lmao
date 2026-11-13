import HWTestCore
import HWTestGPU

/// A debug kernel's GM20B register dump before the first GPU scene draws (gpu/RegDump.swift), for the diff against
/// L4T's nvgpu at idle. Not a .metal test: a GPU test that times out gets GPUProbe's fill, which would then be the
/// process's first command buffer instead of the first scene's.
struct RegDumpQuietTest: HWTest {
    let id = "gpu.regdump_quiet"
    let name = "GPU registers at a quiet point, into the kernel log"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        let metrics = RegDump.capture(.quiet)
        switch metrics["regdump"]?.logText {
        case "quiet:ok":
            return .pass(metrics)
        case "quiet:release_kernel":
            return .skip("a release kernel has no register dump", metrics)
        case "quiet:already":
            return .skip("this boot's quiet dump is already in the kernel log", metrics)
        default:
            return .fail("the kernel did not dump the registers", metrics)
        }
    }
}
