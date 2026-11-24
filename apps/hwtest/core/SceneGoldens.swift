import Foundation

/// How a picture compares with the same picture drawn on a Mac's GPU. exact: the same bytes. close: every 8x8 block's
/// mean within three steps of the format (at least 0.004) plus 1% of its magnitude, which is what another GPU's
/// filtering and half-precision rounding move it by; a wrong blend factor, format, swizzle or texture moves it further.
/// malformed: the golden file has a line for the picture that names no signature (CAGoldens.malformed).
public enum GoldenVerdict: String, Sendable {
    case exact, close, wrong, missing, malformed

    public var holds: Bool { self == .exact || self == .close }

    public static func of(_ ours: Signature, against golden: Signature?, metalFormat: UInt) -> GoldenVerdict {
        guard let golden else { return .missing }
        if ours.hash == golden.hash { return .exact }
        let step = Signature.format(metal: metalFormat)?.step() ?? 0
        return ours.matches(golden, absolute: max(3 * step, 0.004), relative: 0.01) ? .close : .wrong
    }
}

/// The GPU scenes' pictures that have no CPU model, as this IPSW's QuartzCore draws them on a Mac's GPU
/// (apps/hwtest/goldens/scenes_<build>.txt, written by `bazel run --config=remote //apps/hwtest:scene_goldens`): CAGoldens' lines, each
/// keyed "<scene id>/<picture>" with the picture's Metal pixel format as its label.
public struct SceneGoldens: Sendable {
    public let goldens: CAGoldens

    public init(_ text: String) { goldens = CAGoldens(text) }

    public var header: String { goldens.header }

    public func golden(scene: String, picture: String) -> Signature? {
        goldens.states[Self.key(scene: scene, picture: picture)]?[0]
    }

    /// The picture's golden line where it names no signature
    public func malformed(scene: String, picture: String) -> String? {
        goldens.malformed[Self.key(scene: scene, picture: picture)]
    }

    public static func key(scene: String, picture: String) -> String { scene + "/" + picture }

    public static func line(scene: String, picture: String, metalFormat: UInt, _ signature: Signature) -> String {
        CAGoldens.line(
            key: key(scene: scene, picture: picture), label: "f\(metalFormat)", [(index: 0, signature: signature)]
        )
    }
}
