import Foundation
import HWTestCore

/// One deterministic GPU scene: it draws offscreen on the GPU it is given, reads every result back and compares it
/// with a reference worked out on the CPU, or, for a picture the CPU cannot model, with the same picture drawn on a
/// Mac's GPU (SceneResult.picture, core/SceneGoldens.swift). The app judges the process's default device alone; the
/// host test runs it on the Mac's GPU, which is what keeps the references honest.
public protocol GPUScene: Sendable {
    var id: String { get }
    var name: String { get }
    /// The longest one run may take on one device.
    var timeout: TimeInterval { get }
    func run(_ gpu: GPU) async throws -> SceneResult
    /// Failures found in the default device's metrics, which carry its prefix (raw_ on the facade).
    func judge(_ metrics: inout Metrics) -> [String]
}

extension GPUScene {
    public var timeout: TimeInterval { 60 }
    public func judge(_ metrics: inout Metrics) -> [String] { [] }
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
/// A picture with no CPU model, held to the Mac's golden of `golden` (its own label unless another case draws what it
/// must draw). What its target held before the draw, `before` (its bytes) or `cleared` (a clear colour, RGBA): a golden
/// equal to that shows the scene cannot tell a draw from none.
public struct ScenePicture: Sendable {
    public let label: String
    public let golden: String
    public let metalFormat: UInt
    public let signature: Signature
    public let bytes: [UInt8]
    public let width: Int
    public let height: Int
    public let before: [UInt8]?
    public let cleared: [Float]?

    /// The picture's target before the draw, nil when the scene names neither or its format does not decode.
    public var blank: Signature? {
        if let before { return Signature(bytes: before, metalFormat: metalFormat, width: width, height: height) }
        guard let cleared, let format = Signature.format(metal: metalFormat), metalFormat != Signature.twoPlaneXR
        else { return nil }
        let texel = format.encode(Array(cleared.prefix(format.channels)))
        let bytes = [UInt8]([[UInt8]](repeating: texel, count: width * height).joined())
        return Signature(bytes: bytes, metalFormat: metalFormat, width: width, height: height)
    }
}

public struct SceneResult: Sendable {
    public private(set) var checks: [SceneCheck] = []
    public private(set) var problems: [String] = []
    public private(set) var pictures: [ScenePicture] = []
    public var metrics = Metrics()
    public var notes: [String] = []

    public init() {}

    /// A read-back attachment the CPU cannot model; hash_<label>= is its bytes' FNV-1a.
    public mutating func picture(
        _ label: String, bytes: [UInt8], metalFormat: UInt, width: Int, height: Int, golden: String? = nil,
        before: [UInt8]? = nil, cleared: [Float]? = nil
    ) {
        let signature = Signature(bytes: bytes, metalFormat: metalFormat, width: width, height: height)
        pictures.append(
            ScenePicture(
                label: label, golden: golden ?? label, metalFormat: metalFormat, signature: signature, bytes: bytes,
                width: width, height: height, before: before, cleared: cleared))
        metrics.set("hash_" + label, String(signature.hash, radix: 16))
    }

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
