import Foundation
import HWTestCore

/// One deterministic GPU scene: it draws offscreen on the GPU it is given, reads every result back and compares it
/// with a reference worked out on the CPU. The app runs it on the process's default device and on the m2v device the
/// raw facade wraps; the host test runs it on the Mac's GPU, which is what keeps the references honest.
public protocol GPUScene: Sendable {
    var id: String { get }
    var name: String { get }
    /// The longest one run may take on one device.
    var timeout: TimeInterval { get }
    func run(_ gpu: GPU) async throws -> SceneResult
}

extension GPUScene {
    public var timeout: TimeInterval { 60 }
}

public struct SceneCheck: Sendable {
    public let label: String
    public let got: Raster
    public let want: Raster
    public let diff: RasterDiff

    public init(label: String, got: Raster, want: Raster, diff: RasterDiff) {
        self.label = label
        self.got = got
        self.want = want
        self.diff = diff
    }

    public var line: String { "\(label): \(diff.summary)" }
}

/// What one run of a scene found: each comparison, extra measurements (GPU times, readback agreement), notes on
/// what was left out (a format the device lacks) and the run's own failures that are not picture comparisons.
public struct SceneResult: Sendable {
    public private(set) var checks: [SceneCheck] = []
    public private(set) var problems: [String] = []
    public var metrics = Metrics()
    public var notes: [String] = []

    public init() {}

    @discardableResult
    public mutating func compare(_ label: String, _ got: Raster, _ want: Raster, tolerance: Float) -> RasterDiff {
        let diff = RasterDiff.compare(got, want, tolerance: tolerance)
        checks.append(SceneCheck(label: label, got: got, want: want, diff: diff))
        return diff
    }

    /// Bytes read back against the bytes an exact GPU stores: equal bytes pass without decoding (the common case,
    /// cheap for large targets); otherwise both are decoded and compared within `tolerance`.
    @discardableResult
    public mutating func compare(
        _ label: String, bytes got: [UInt8], expected want: [UInt8], format: TexelFormat, width: Int, height: Int,
        tolerance: Float
    ) -> RasterDiff {
        if got == want {
            let empty = Raster(width: 0, height: 0, channels: format.channels, samples: [])
            let diff = RasterDiff.compare(empty, empty, tolerance: tolerance)
            checks.append(SceneCheck(label: label, got: empty, want: empty, diff: diff))
            return diff
        }
        return compare(
            label, format.raster(got, width: width, height: height), format.raster(want, width: width, height: height),
            tolerance: tolerance)
    }

    /// A failure that is no picture (a value out of range, a call that returned nil).
    public mutating func expect(_ condition: Bool, _ problem: @autoclosure () -> String) {
        if !condition { problems.append(problem()) }
    }

    public var failed: [SceneCheck] { checks.filter { !$0.diff.matches } }

    public var passed: Bool { failed.isEmpty && problems.isEmpty }

    public var wrongPixels: Int { checks.reduce(0) { $0 + $1.diff.wrong } }

    /// The first failures, most informative first: problems, then picture checks.
    public func reasons(limit: Int = 2) -> [String] {
        let all = problems + failed.map(\.line)
        let shown = Array(all.prefix(limit))
        return all.count > limit ? shown + ["\(all.count - limit) more"] : shown
    }
}
