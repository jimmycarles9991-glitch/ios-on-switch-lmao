import Foundation

/// The long forms Run All leaves out: the barrier probes' full trial targets (bound95 about 0.002 a cell) and the
/// timings that are reported, not judged. On from the main screen's Long runs switch (kept in the app's defaults) or
/// with HWTEST_LONG=1 in the app's environment.
public enum LongRuns {
    public static let defaultsKey = "HWTestLongRuns"

    public static var on: Bool {
        ProcessInfo.processInfo.environment["HWTEST_LONG"] == "1" || UserDefaults.standard.bool(forKey: defaultsKey)
    }
}
