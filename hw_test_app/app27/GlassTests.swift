import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import UIKit

/// A Liquid Glass panel (UIGlassEffect, regular) across the middle of vertical stripes and of flat grey. Outside it the
/// composited picture must match the CPU's. Inside, away from the lensing at its edges, each column over the stripes
/// stays constant down its length (no streak or block) and the grey stays flat and not black. The stripes' remaining
/// contrast is reported (contrast=) but not judged: how much glass blurs is UIKit's choice, not the GPU's.
struct UIGlassTest: HWTest {
    let id = "ui.glass"
    let name = "UIGlassEffect over stripes and flat grey"
    let subsystem = Subsystem.compositor

    func run(_ context: TestContext) async throws -> Outcome {
        let (outside, inside) = await UISceneTests.staged(context) { stage, screen -> UIView in
            let root = UIView(frame: Snapshots.aligned(screen))
            root.addSubview(BackdropStripes(frame: CGRect(x: 0, y: 0, width: screen.width / 2, height: screen.height)))
            let grey = UIView(frame: CGRect(x: screen.width / 2, y: 0, width: screen.width / 2, height: screen.height))
            grey.backgroundColor = UIColor(white: 0.5, alpha: 1)
            root.addSubview(grey)
            let glass = UIVisualEffectView(effect: UIGlassEffect(style: .regular))
            glass.frame = Snapshots.aligned(
                CGRect(x: 0, y: screen.height / 4, width: screen.width, height: screen.height / 2))
            root.addSubview(glass)
            stage.addSubview(root)
            return root
        } _: { root -> (CompositeCheck, Raster) in
            let (composited, cpu) = (Snapshots.composited(root), Snapshots.cpu(root))
            let band = composited.height / 4
            return (
                CompositeCheck(
                    label: "outside the glass",
                    composited: composited.cropped(x: 0, y: 0, width: composited.width, height: band - 2),
                    cpu: cpu.cropped(x: 0, y: 0, width: cpu.width, height: band - 2)),
                composited
            )
        }
        var metrics = Metrics()
        var reasons: [String] = []
        outside.judge(&metrics, &reasons, "outside")
        let inset = inside.height / 16
        let (top, height, half) = (inside.height / 4 + inset, inside.height / 2 - 2 * inset, inside.width / 2)
        let stripes = Backdrop.overStripes(inside.cropped(x: inset, y: top, width: half - 2 * inset, height: height))
        let grey = Backdrop.overUniform(
            inside.cropped(x: half + inset, y: top, width: half - 2 * inset, height: height))
        metrics.set("column_spread", Int((stripes.columnSpread * 255).rounded()))
        metrics.set("contrast", Int((stripes.contrast * 255).rounded()))
        metrics.set("grey_spread", Int((grey.spread * 255).rounded()))
        metrics.set("grey_mean", grey.mean.map { String(Int(($0 * 255).rounded())) }.joined(separator: ","))
        let checks: [(String, Bool)] = [
            ("each column constant down the glass (spread <= 6)", stripes.columnSpread <= 6.0 / 255),
            ("grey stays flat (spread <= 6)", grey.spread <= 6.0 / 255),
            ("grey not black", grey.mean.prefix(3).reduce(0, +) > 0.12),
        ]
        reasons += checks.filter { !$0.1 }.map(\.0)
        if !reasons.isEmpty {
            let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try? inside.png()?.write(to: directory.appendingPathComponent("composited.png"))
        }
        return reasons.isEmpty ? .pass(metrics) : .fail(LogLine.fitted(reasons), metrics)
    }
}

/// pacing.blur_over and pacing.blur_under with Liquid Glass in the blur's place: iOS 27 draws glass under every bar,
/// button and sheet, so it must hold 60 Hz like any other scene.
enum GlassPacing {
    static let over = PacingTest(
        id: "pacing.glass_over", name: "Pacing: a full-screen layer animated over Liquid Glass", probeSize: 48
    ) { stage, screen in
        stage.addSubview(BackdropStripes(frame: screen))
        let glass = UIVisualEffectView(effect: UIGlassEffect(style: .regular))
        glass.frame = screen.insetBy(dx: 40, dy: 40)
        stage.addSubview(glass)
        let veil = UIView(frame: screen)
        stage.addSubview(veil)
        return { frame in
            veil.backgroundColor = UIColor(
                hue: CGFloat(frame % 120) / 120, saturation: 0.7, brightness: 0.9, alpha: 0.35)
            veil.transform = CGAffineTransform(translationX: CGFloat(frame % 40) - 20, y: 0)
        }
    }

    static let under = PacingTest(
        id: "pacing.glass_under", name: "Pacing: a backdrop moving under Liquid Glass", probeSize: 48
    ) { stage, screen in
        let stripes = BackdropStripes(frame: screen.insetBy(dx: -64, dy: 0))
        stage.addSubview(stripes)
        let glass = UIVisualEffectView(effect: UIGlassEffect(style: .regular))
        glass.frame = screen.insetBy(dx: 40, dy: 40)
        stage.addSubview(glass)
        return { frame in stripes.transform = CGAffineTransform(translationX: CGFloat(frame % 32) * 2 - 32, y: 0) }
    }
}
