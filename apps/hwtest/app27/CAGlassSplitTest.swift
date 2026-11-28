import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestScenes

struct CAGlassSplitTest: HWTest {
    let scene = Scenes.glassSplit
    let subsystem = Subsystem.metal
    var id: String { scene.id }
    var name: String { scene.name }
    var timeout: TimeInterval { scene.timeout * 2 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("requires the facade to exercise native whole and split programs")
        }
        guard GM20BFacade.counters(of: gpu.device)["split_prologues"] != nil else {
            return .skip(GM20BFacade.missing("split_prologues", on: gpu.device))
        }
        return try await GPUSceneTest(scene: scene).run(context)
    }
}
