import Foundation

/// gpu.path.cpu_locks_side_by_side: B's writer ends where A's starts, so B's lock must return ahead of A's by at least
/// half of A's GPU time. A's own GPU stamps measure it; the calibration render's estimate stands in only where the
/// buffer carries no stamps.
public enum LockAheadBound {
    public static func requiredMs(aGpuMs: Double, calibratedMs: Double) -> Double {
        (aGpuMs > 0 ? aGpuMs : calibratedMs) / 2
    }
}
