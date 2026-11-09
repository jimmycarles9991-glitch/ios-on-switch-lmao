import Foundation
import HWTestCore
import HWTestGPU
import UIKit

/// A view's pixels two ways at the screen's scale: as the render server composited them on the GPU
/// (drawHierarchy(afterScreenUpdates:), which renders the window's layers there, backdrop blurs included), and as
/// Core Animation's CPU renderer draws the same layer tree in this process (layer.render(in:), no backdrop blur).
@MainActor
public enum Snapshots {
    public static func composited(_ view: UIView) -> Raster {
        raster(view) { _ in view.drawHierarchy(in: view.bounds, afterScreenUpdates: true) }
    }

    public static func cpu(_ view: UIView) -> Raster {
        raster(view) { view.layer.render(in: $0.cgContext) }
    }

    static func raster(_ view: UIView, _ draw: (UIGraphicsImageRendererContext) -> Void) -> Raster {
        let format = UIGraphicsImageRendererFormat()
        format.scale = view.window?.screen.scale ?? UIScreen.main.scale
        format.preferredRange = .standard
        format.opaque = true
        let image = UIGraphicsImageRenderer(size: view.bounds.size, format: format).image(actions: draw).cgImage!
        var bytes = [UInt8](repeating: 0, count: image.width * image.height * 4)
        let context = CGContext(
            data: &bytes, width: image.width, height: image.height, bitsPerComponent: 8, bytesPerRow: image.width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
        context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
        let opaque = Raster(unorm8: bytes, width: image.width, height: image.height, channels: 4)
        return Raster(width: opaque.width, height: opaque.height, channels: 3) { x, y, c in opaque[x, y, c] }
    }

    /// A frame whose edges fall on whole pixels, so both renderers place every layer alike.
    public static func aligned(_ rect: CGRect) -> CGRect {
        let scale = UIScreen.main.scale
        let snap = { (v: CGFloat) in (v * scale).rounded() / scale }
        return CGRect(x: snap(rect.minX), y: snap(rect.minY), width: snap(rect.width), height: snap(rect.height))
    }

    /// Waits for the render server to have the view's latest commit on screen.
    public static func settle() async {
        await MainActor.run { CATransaction.flush() }
        try? await Task.sleep(nanoseconds: 300_000_000)
    }
}

/// The composited picture against the CPU's: averaged over 4x4 blocks no block may be more than 24 levels off
/// (anti-aliasing the two renderers do differently averages out; noise, a stray triangle or a streak does not),
/// and no whole column or row may go wrong at full resolution.
public struct CompositeCheck {
    let label: String
    let composited: Raster
    let cpu: Raster

    public init(label: String, composited: Raster, cpu: Raster) {
        self.label = label
        self.composited = composited
        self.cpu = cpu
    }

    var blocks: RasterDiff {
        RasterDiff.compare(composited.boxDownsampled(by: 4), cpu.boxDownsampled(by: 4), tolerance: 24.0 / 255)
    }

    var pixels: RasterDiff { RasterDiff.compare(composited, cpu, tolerance: 48.0 / 255) }

    public func judge(_ metrics: inout Metrics, _ reasons: inout [String], _ prefix: String) {
        let (blocks, pixels) = (blocks, pixels)
        metrics.merge(blocks.metrics(prefix: prefix + "_4x_"))
        metrics.set(prefix + "_px_wrong", pixels.wrong)
        metrics.set(prefix + "_streaks", pixels.streakColumns + pixels.streakRows)
        if !blocks.matches { reasons.append("\(label): 4x4 blocks \(blocks.summary)") }
        if pixels.streakColumns + pixels.streakRows > 0 { reasons.append("\(label): pixels \(pixels.summary)") }
    }

    public func save(to documents: URL, id: String, stem: String) {
        let directory = documents.appendingPathComponent("ui").appendingPathComponent(id)
        try? SceneCheck(label: label, got: composited, want: cpu, diff: pixels).save(to: directory, stem: stem)
    }
}
