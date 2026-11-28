import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Slots 5 and 6 whose Metal addresses the vertex function compares (gpu/BytesAddress.swift), as Weather's VFX reads
/// buffer 5's: two setVertexBytes bindings give slot 5's magenta (`bytes`), one buffer bound to both gives black
/// (`same`), as on an M4 (host_gpu_tests/BytesAddressTests.swift); only real addresses give both. Each
/// draw writes a bytes binding's upload copy into its SlotGuestBase hole (the driver's Pipeline::guestBases); the record
/// carries the holes once the pack build presents such a slot, until then the state is refused (MissingGuestAddress):
/// `<arm>_pixel=` the bytes read, `<arm>_path=`.
struct BytesAddressTest: HWTest {
    let id = "gpu.gm20b.bytes_address"
    let name = "A vertex function comparing two setVertexBytes bindings' addresses sees two addresses"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        for (arm, same, want) in [("bytes", false, [UInt8]([255, 0, 255, 255])), ("same", true, [0, 0, 0, 0])] {
            let ran: (pixel: [UInt8], path: String)
            do {
                ran = try await BytesAddress.run(gpu, same: same)
            } catch {
                metrics.set("\(arm)_error", String(describing: error).prefix(100).description)
                wrong.append("\(arm)_refused")
                continue
            }
            metrics.set("\(arm)_pixel", ran.pixel.map { String($0) }.joined(separator: "."))
            metrics.set("\(arm)_path", ran.path)
            if ran.pixel != want || ran.path != "gm20b" { wrong.append("\(arm)_pixel") }
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
