import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUModels
import HWTestKit
import Metal

/// Texture transfers wait for the GPU without their lock (gpu/TransferBesideReadback.swift): while another thread reads
/// level 1 of a 4096x4096 shared texture over and over by getBytes, each a copy-engine transfer the reader waits for,
/// replaceRegion of level 1 of a small shared texture goes ahead: the uploads' p99 stays under the reads' p50
/// (upload_p99_ms, read_p50_ms; a transfer that waited for the read in flight takes about a read). Reported: reads,
/// uploads, overlapped (uploads begun while a read was in flight), read_p99_ms, upload_p50_ms.
struct TransferBesideReadbackTest: HWTest {
    let id = "gpu.path.transfer_beside_readback"
    let name = "A small upload passes a large readback in flight instead of waiting for it"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let timed = try TransferBesideReadback.run(gpu.device)
        let readP50 = Stats.percentile(timed.readsMs, 0.5), uploadP99 = Stats.percentile(timed.uploadsMs, 0.99)
        var metrics = Metrics()
        metrics.set("reads", timed.readsMs.count)
        metrics.set("uploads", timed.uploadsMs.count)
        metrics.set("overlapped", timed.overlapped)
        metrics.set("read_p50_ms", readP50, digits: 2)
        metrics.set("read_p99_ms", Stats.percentile(timed.readsMs, 0.99), digits: 2)
        metrics.set("upload_p50_ms", Stats.percentile(timed.uploadsMs, 0.5), digits: 2)
        metrics.set("upload_p99_ms", uploadP99, digits: 2)
        return .judged(
            [
                ("\(timed.overlapped) uploads began while a read was in flight", timed.overlapped > 0),
                (
                    String(format: "uploads' p99 %.2f ms not under the reads' p50 %.2f ms", uploadP99, readP50),
                    uploadP99 < readP50
                ),
            ], metrics, proof: gpu.proof
        )
    }
}
