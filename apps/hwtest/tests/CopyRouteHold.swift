import Foundation
import HWTestCore

/// The display held on the copy route while `body` runs (debug kernels' PORTRAIT, SwitchLandscapeRunABI.h): with V3's
/// column scan shipped, a landscape video goes to window A, and a test of windows B and C's own paths (VIC's composite,
/// the media completion flip) holds the copy route that puts it there. The hold must be armed before `body` runs.
enum CopyRouteHold {
    static func during<R>(_ body: () async throws -> R) async throws -> R {
        let session = try VICKernelTest.Session.open().get()
        defer { session.close() }
        _ = try session.call(
            LandscapeSample.request(op: LandscapeSample.portrait, value: LandscapeSample.portraitFrames),
            replyBytes: LandscapeSample.size
        ).get()
        defer {
            _ = session.call(
                LandscapeSample.request(op: LandscapeSample.portrait, value: 0), replyBytes: LandscapeSample.size
            )
        }
        return try await body()
    }
}
