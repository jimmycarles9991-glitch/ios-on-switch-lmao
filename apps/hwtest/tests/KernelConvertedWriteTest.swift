import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// KernelConvertedWrites.swift on the driver: per view <view>_bytes_changed= surface bytes the dropped store moved,
/// <view>_reads_changed= texels a later read() reads otherwise, <view>_null_nonzero= bytes a read of a null binding
/// after it made non-zero, <view>_paths=; failed_cbs=. An M4 Max: 0, 0, 0 for both views.
struct KernelConvertedWriteTest: HWTest {
    let id = "gpu.gm20b.kernel_converted_write"
    let name = "A kernel's store into a converted YCbCr view is dropped, as Metal drops it, on raw"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for view in KernelConvertedWrites.views {
            do {
                let r = try await KernelConvertedWrites.run(gpu, view)
                metrics.set("\(view.key)_bytes_changed", r.bytesChanged)
                metrics.set("\(view.key)_reads_changed", r.readsChanged)
                metrics.set("\(view.key)_null_nonzero", r.nullNonZero)
                metrics.set("\(view.key)_paths", r.paths.map { String($0.prefix(60)) }.joined(separator: ","))
                checks.append(("\(view.key): the surface as it was", r.bytesChanged + r.readsChanged == 0))
                checks.append(("\(view.key): a null binding reads 0 after it", r.nullNonZero == 0))
                checks.append(("\(view.key): completed on raw", r.paths.allSatisfy { $0 == "gm20b" }))
            } catch {
                checks.append(("\(view.key): \(GPU.describe(error))", false))
            }
        }
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
