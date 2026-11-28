import Darwin
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared

/// Every process's largest colour target since boot as the debug push scan read it (TegraGM20BPathsABI.h's target
/// peaks, debug kernels) is one the GM20B can draw: no side past 16384. The scan reads most submissions on a thread call;
/// boot 22's read words a process had already written again and kept two method headers as a size
/// (iconservicesagent 2147484889x2147484890, backboardd 537002843x2621440).
struct PushScanTargetsTest: HWTest {
    let id = "gpu.push_scan_targets"
    let name = "The push scan's largest colour target of each process is one the GPU can draw"
    let subsystem = Subsystem.metal

    static let maxSide = 16384

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection", metrics) }
        let (kr, reply) = connection.call(
            GPUPaths.method, GPUConnection.words([PosterTargetsTest.targetPeaksOp, 0]), outputSize: GPUPaths.replySize
        )
        guard kr == KERN_SUCCESS else {
            let why =
                kr == GPUConnection.kIOReturnUnsupported
                    ? (KernelSysctl.isReleaseKernel()
                        ? "a release kernel keeps no target peaks"
                        : "no target peaks (build with --//bazel:prof_gpu)")
                    : "target peaks unreadable (\(GPUConnection.hex(kr)))"
            return .skip(why, metrics)
        }
        let peaks = TargetPeak.parse(String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self))
        let outside = peaks.filter { max($0.width, $0.height) > Self.maxSide }
        metrics.set("processes", peaks.count)
        if let largest = peaks.first { metrics.set("largest", largest.text) }
        metrics.set("outside", outside.isEmpty ? "-" : outside.map(\.text).joined(separator: ","))
        guard !peaks.isEmpty else { return .skip("no process has drawn since boot", metrics) }
        return .judged(
            [("every process's largest target has no side past \(Self.maxSide) (outside = -)", outside.isEmpty)],
            metrics
        )
    }
}
