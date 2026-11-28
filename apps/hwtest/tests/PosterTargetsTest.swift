import Darwin
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import UIKit

/// Lock screen posters and their snapshots render no target larger than the panel: the GM20B kernel's largest
/// colour-target image of each process since boot (TegraGM20BPathsABI.h's target peaks, debug kernels), so the poster
/// drawn at boot counts while this app is in front. A poster sizes its drawable from UIScreen.nativeScale, so an image
/// past the panel means a screen scale or size answer of ours is wrong. A multisampled target counts its pixels: 24B119's
/// Celosia poster draws 4x at 720x1280, which the push profile shows as its 1440x2560 sample grid (3686k).
struct PosterTargetsTest: HWTest {
    let id = "gpu.poster_targets"
    let name = "Lock screen posters render no target larger than the panel"
    let subsystem = Subsystem.metal

    static let targetPeaksOp: UInt32 = 1

    func run(_ context: TestContext) async throws -> Outcome {
        let panel = await MainActor.run { UIScreen.main.nativeBounds.size }
        let width = Int(panel.width)
        let height = Int(panel.height)
        var metrics = Metrics()
        metrics.set("panel", "\(width)x\(height)")
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection", metrics) }
        let (kr, reply) = connection.call(
            GPUPaths.method, GPUConnection.words([Self.targetPeaksOp, 0]), outputSize: GPUPaths.replySize
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
        if let largest = peaks.first { metrics.set("largest", largest.text) }
        metrics.set("owners", peaks.count)
        metrics.set("peaks", peaks.isEmpty ? "-" : peaks.map(\.text).joined(separator: ","))
        let posters = peaks.filter(\.isPoster)
        metrics.set("posters", posters.isEmpty ? "-" : posters.map(\.text).joined(separator: ","))
        guard !posters.isEmpty else {
            return .skip(
                "no poster process among the kernel's \(peaks.count) target peaks; its push scan reads one submission "
                    +
                    "in four per connection (TegraGM20BPushScanRun.cpp), so a poster that drew with few may be missing",
                metrics
            )
        }
        return .judged(
            posters.map {
                (
                    "\($0.owner) drew a \($0.width)x\($0.height) image, past the panel",
                    $0.fits(width: width, height: height)
                )
            }, metrics
        )
    }
}
