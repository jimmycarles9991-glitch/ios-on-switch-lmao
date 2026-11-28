import Foundation
import HWTestCore

/// The display held on the copy route while `body` runs (debug kernels' PORTRAIT, SwitchLandscapeRunABI.h): with V3's
/// column scan shipped, a landscape video goes to window A, and a test of windows B and C's own paths (VIC's composite,
/// the media completion flip) holds the copy route that puts it there. The hold must be armed before `body` runs.
public enum CopyRouteHold {
    public static func during<R>(_ body: () async throws -> R) async throws -> R {
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

/// Every UI frame through the GPU's present copy while `body` runs (debug kernels' COPY, SwitchLandscapeRunABI.h):
/// direct scanout flips a frame from its own surface with no copy at all, so a test of that copy holds it off.
/// `body` gets whether the kernel took the hold.
public enum GPUCopyHold {
    public static func during<R>(_ body: (Bool) async throws -> R) async throws -> R {
        let session = try? VICKernelTest.Session.open().get()
        defer { session?.close() }
        let held =
            (try? session?.call(
                LandscapeSample.request(op: LandscapeSample.copy, value: LandscapeSample.portraitFrames),
                replyBytes: LandscapeSample.size
            ).get()) != nil
        defer {
            if held {
                _ = session?.call(
                    LandscapeSample.request(op: LandscapeSample.copy, value: 0), replyBytes: LandscapeSample.size
                )
            }
        }
        return try await body(held)
    }
}
