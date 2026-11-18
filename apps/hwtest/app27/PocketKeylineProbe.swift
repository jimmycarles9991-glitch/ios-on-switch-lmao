import HWTestCore
import HWTestKit
import SwiftUI
import UIKit

/// The hard scroll edge effect's layers as UIKit makes them in this process (a container named "v8: ...", the backdrop
/// layers "Replay" and "Pocket", and "Keyline": a plain layer filled with a translucent white at the pocket's bottom
/// edge), and the snapshot's keyline row with each one changed, which tells the draw that makes the purple row:
/// keyline= the keyline layer's fill (r,g,b,a and its colour space), keyline_px= its rows in the picture, opacity and
/// compositing filter; pocket_filters= the Pocket layer's filter types; container= the container's name. keyline_want=
/// is the fill UIKit gives J181 in the view's interface style (keyline_style=), keyline_verdict= want (J181's fill in
/// either style: UIKit takes the style from its own input), magenta (UIKit's fill for a display scale, keyline_scale=,
/// that its keyline has no row for) or other; a fill other than J181's fails the test. Then, the row at the keyline's
/// last pixel row (<run>_rgb) and the picture's purple rows (<run>_at) with: the keyline hidden (nokeyline), filled
/// opaque red, green, blue and grey (red, green, blue, grey), the Pocket layer without its colour matrix and SDR
/// filters (nomatrix), and the Pocket layer hidden (nopocket).
enum PocketKeylineProbe {
    struct Layers {
        let keyline: CALayer
        let pocket: CALayer?
        let container: CALayer?
        /// The keyline's fill, R, G, B, A, and J181's, the view's interface style's first
        let fill: [Double]?
        let wants: [[Double]]
    }

    /// UIKit 27's keyline at scales 1 and 2: the separator alpha of its pocket constants (0.1 in both styles) over
    /// sRGB white in dark and black in light
    static func wants(dark: Bool) -> [[Double]] {
        let (white, black): ([Double], [Double]) = ([1, 1, 1, 0.1], [0, 0, 0, 0.1])
        return dark ? [white, black] : [black, white]
    }

    /// Why the keyline's fill fails the test, nil when it is J181's
    static func fillProblem(_ layers: Layers) -> String? {
        switch KeylineClaim.fillVerdict(layers.fill, wants: layers.wants) {
        case "want": return nil
        case "magenta": return "UIKit filled the keyline layer opaque magenta"
        default: return "the keyline layer's fill is not J181's"
        }
    }

    static func run(
        _ context: TestContext, _ metrics: inout Metrics, _ reasons: inout [String]
    ) async
        -> [(String, Raster)]
    {
        let host = await context.onStage(fullScreen: true) { stage -> UIHostingController<PocketList> in
            let host = UIHostingController(rootView: PocketList(hidden: false, style: .hard))
            host.view.frame = Snapshots.aligned(UIScreen.main.bounds)
            stage.addSubview(host.view)
            return host
        }
        await Snapshots.settle()
        await MainActor.run {
            guard let scroll = PocketStripTest.first(UICollectionView.self, in: host.view) else { return }
            scroll.contentOffset.y = 400 - scroll.adjustedContentInset.top
        }
        await Snapshots.settle()
        guard let (layers, row, found) = await MainActor.run(body: { describe(host.view) }) else {
            metrics.set("keyline", "none")
            return []
        }
        metrics.merge(found)
        if let problem = fillProblem(layers) { reasons.append(problem) }
        let original = await MainActor.run { layers.keyline.backgroundColor }
        let filters = await MainActor.run { layers.pocket?.filters }
        let opaque = { (r: CGFloat, g: CGFloat, b: CGFloat) in CGColor(srgbRed: r, green: g, blue: b, alpha: 1) }
        let runs: [(String, @MainActor () -> Void)] = [
            ("nokeyline", { layers.keyline.isHidden = true }),
            ("red", { layers.keyline.backgroundColor = opaque(1, 0, 0) }),
            ("green", { layers.keyline.backgroundColor = opaque(0, 1, 0) }),
            ("blue", { layers.keyline.backgroundColor = opaque(0, 0, 1) }),
            ("grey", { layers.keyline.backgroundColor = opaque(0.5, 0.5, 0.5) }),
            ("nomatrix", { layers.pocket?.filters = filters?.filter { !matrixFilter($0) } }),
            ("nopocket", { layers.pocket?.isHidden = true }),
        ]
        var saved: [(String, Raster)] = []
        for (name, change) in runs {
            await MainActor.run {
                CATransaction.begin()
                CATransaction.setDisableActions(true)
                change()
                CATransaction.commit()
            }
            await Snapshots.settle()
            let settled = await MainActor.run { Snapshots.composited(host.view) }
            let rows = PocketStripTest.purpleRows(settled)
            metrics.set(name + "_at", PocketStripTest.ranges(rows))
            if row < settled.height {
                metrics.set(name + "_rgb", PocketStripTest.bytes(settled, settled.width / 2, row))
            }
            if !rows.isEmpty { saved.append(("keyline_" + name, settled)) }
            await MainActor.run {
                CATransaction.begin()
                CATransaction.setDisableActions(true)
                layers.keyline.isHidden = false
                layers.keyline.backgroundColor = original
                layers.pocket?.filters = filters
                layers.pocket?.isHidden = false
                CATransaction.commit()
            }
        }
        return saved
    }

    static func matrixFilter(_ filter: Any) -> Bool {
        guard let type = (filter as? NSObject)?.value(forKey: "type") as? String else { return false }
        return type == "colorMatrix" || type == "sdrNormalize"
    }

    /// The layers, the keyline's last pixel row in the picture and their metrics; nil without a keyline layer.
    @MainActor
    static func describe(_ view: UIView) -> (Layers, Int, Metrics)? {
        guard let keyline = find(view.layer, { $0.name == "Keyline" }) else { return nil }
        var metrics = Metrics()
        let container = keyline.superlayer
        let pocket = container?.sublayers?.first { $0.name == "Pocket" }
        let scale = view.window?.screen.scale ?? 1
        let frame = keyline.convert(keyline.bounds, to: view.layer)
        let (top, bottom) = (frame.minY * scale, frame.maxY * scale)
        let dark = view.traitCollection.userInterfaceStyle == .dark
        let layers = Layers(
            keyline: keyline, pocket: pocket, container: container, fill: rgba(keyline.backgroundColor),
            wants: wants(dark: dark))
        metrics.set("keyline", fill(keyline.backgroundColor))
        metrics.set("keyline_want", text(layers.wants[0]) + "/SRGB")
        metrics.set("keyline_verdict", KeylineClaim.fillVerdict(layers.fill, wants: layers.wants))
        metrics.set("keyline_scale", Double(view.traitCollection.displayScale))
        metrics.set("keyline_style", dark ? "dark" : "light")
        metrics.set("keyline_px", String(format: "%.2f-%.2f", top, bottom))
        metrics.set("keyline_opacity", Double(keyline.opacity))
        metrics.set("keyline_filter", (keyline.compositingFilter as? NSObject)?.description ?? "-")
        metrics.set("pocket_filters", pocket.map(filterTypes) ?? "-")
        metrics.set("container", (container?.name ?? "-").replacingOccurrences(of: " ", with: "_"))
        let row = max(Int(bottom.rounded(.up)) - 1, 0)
        metrics.set("keyline_row", row)
        return (layers, row, metrics)
    }

    static func fill(_ color: CGColor?) -> String {
        guard let color, let components = color.components else { return "-" }
        let space = (color.colorSpace?.name as String?)?.replacingOccurrences(of: "kCGColorSpace", with: "") ?? "-"
        return text(components.map(Double.init)) + "/" + space
    }

    /// R, G, B, A of an RGB colour, nil for any other model
    static func rgba(_ color: CGColor?) -> [Double]? {
        guard let color, color.colorSpace?.model == .rgb, let components = color.components, components.count == 4
        else { return nil }
        return components.map(Double.init)
    }

    static func text(_ components: [Double]) -> String {
        components.map { String(format: "%.3f", $0) }.joined(separator: ",")
    }

    static func filterTypes(_ layer: CALayer) -> String {
        let types = (layer.filters ?? []).compactMap { ($0 as? NSObject)?.value(forKey: "type") as? String }
        return types.isEmpty ? "-" : types.joined(separator: "+")
    }

    static func find(_ layer: CALayer, _ match: (CALayer) -> Bool) -> CALayer? {
        if match(layer) { return layer }
        for sublayer in layer.sublayers ?? [] {
            if let found = find(sublayer, match) { return found }
        }
        return nil
    }
}
