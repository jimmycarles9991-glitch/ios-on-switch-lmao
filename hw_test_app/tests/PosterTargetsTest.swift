import Darwin
import HWTestCore
import HWTestGPU
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
            GPUPaths.method, GPUConnection.words([Self.targetPeaksOp, 0]), outputSize: 512)
        guard kr == KERN_SUCCESS else {
            let why =
                kr == GPUConnection.kIOReturnUnsupported
                ? "a release kernel keeps no target peaks" : "target peaks unreadable (\(GPUConnection.hex(kr)))"
            return .skip(why, metrics)
        }
        let peaks = TargetPeak.parse(String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self))
        if let largest = peaks.first { metrics.set("largest", largest.text) }
        let posters = peaks.filter(\.isPoster)
        metrics.set("posters", posters.map(\.text).joined(separator: ","))
        guard !posters.isEmpty else { return .skip("no poster has drawn since boot", metrics) }
        return .judged(
            posters.map {
                (
                    "\($0.owner) drew a \($0.width)x\($0.height) image, past the panel",
                    $0.fits(width: width, height: height)
                )
            }, metrics)
    }
}
