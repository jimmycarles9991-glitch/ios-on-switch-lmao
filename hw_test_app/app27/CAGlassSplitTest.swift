import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit

struct CAGlassSplitTest: HWTest {
    let scene = Scenes.glassSplit
    let subsystem = Subsystem.metal
    var id: String { scene.id }
    var name: String { scene.name }
    var timeout: TimeInterval { scene.timeout * 2 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("requires the raw facade to exercise native whole and split programs")
        }
        guard RawFacade.counters(of: gpu.device)["split_prologues"] != nil else {
            return .skip("requires development split attribution; route coverage is unverified")
        }
        return try await GPUSceneTest(scene: scene).run(context)
    }
}
