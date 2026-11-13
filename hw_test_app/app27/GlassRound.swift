import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import UIKit

/// The glass stage of ui.glass_first, ui.glass_motion and ui.glass_ab: stripes on the left half, flat grey on the
/// right, a UIGlassEffect band across the middle half.
@MainActor
struct GlassStage {
    let root: UIView
    let stripes: UIView
    let glass: UIView

    /// Glass hierarchies the tests built in this process: the render server always has others (SpringBoard's, this
    /// app's navigation bar), so 0 only means no test built one yet.
    static var built = 0

    init(_ stage: UIView) {
        let screen = UIScreen.main.bounds
        root = UIView(frame: Snapshots.aligned(screen))
        stripes = BackdropStripes(frame: CGRect(x: -64, y: 0, width: screen.width / 2 + 64, height: screen.height))
        root.addSubview(stripes)
        let grey = UIView(frame: CGRect(x: screen.width / 2, y: 0, width: screen.width / 2, height: screen.height))
        grey.backgroundColor = UIColor(white: 0.5, alpha: 1)
        root.addSubview(grey)
        glass = UIVisualEffectView(effect: UIGlassEffect(style: .regular))
        glass.frame = Snapshots.aligned(
            CGRect(x: 0, y: screen.height / 4, width: screen.width, height: screen.height / 2))
        root.addSubview(glass)
        stage.addSubview(root)
        Self.built += 1
    }

    /// The mean of R + G + B over the grey half inside the glass, away from its rim (0 to 3).
    nonisolated static func level(_ composited: Raster) -> Float {
        let inset = composited.height / 16
        let (top, height, half) =
            (composited.height / 4 + inset + 16, composited.height / 2 - 2 * inset - 16, composited.width / 2)
        return Backdrop.overUniform(
            composited.cropped(x: half + inset, y: top, width: half - 2 * inset, height: height)
        ).mean.prefix(3).reduce(0, +)
    }

    nonisolated static func black(_ level: Float) -> Bool { level <= 0.12 }
}

/// One snapshot of a new glass stage, as ui.glass_first takes it: built, settled, `before` run on it, composited by the
/// render server with its command buffers recorded (GlassRecording, with the draw lines when `lines`), the stage
/// cleared.
struct GlassRound {
    let level: Float
    let recording: GlassRecording
    let picture: Raster
    /// No test built a glass hierarchy in this process before this one.
    let firstInProcess: Bool

    var black: Bool { GlassStage.black(level) }

    static func run(
        _ context: TestContext, lines: Bool = false, before: @escaping @MainActor (GlassStage) -> Void = { _ in }
    ) async -> GlassRound {
        let first = await MainActor.run { GlassStage.built == 0 }
        let stage = await context.onStage(fullScreen: true) { GlassStage($0) }
        await Snapshots.settle()
        let (picture, recording) = await MainActor.run { () -> (Raster, GlassRecording) in
            before(stage)
            return GlassRecording.around(lines: lines) { Snapshots.composited(stage.root) }
        }
        await context.clearStage()
        return GlassRound(
            level: GlassStage.level(picture), recording: recording, picture: picture, firstInProcess: first)
    }
}
