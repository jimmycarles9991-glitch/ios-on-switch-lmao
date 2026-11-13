import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import UIKit

/// A Liquid Glass panel (UIGlassEffect, regular) across the middle of vertical stripes and of flat grey. Outside it the
/// composited picture must match the CPU's. Inside, away from the lensing at its edges, each column over the stripes
/// stays constant down its length (no streak or block) and the grey stays flat and not black. The stripes' remaining
/// contrast is reported (contrast=) but not judged: how much glass blurs is UIKit's choice, not the GPU's. Apple's own
/// render of this scene (the iOS 27 simulator, iPad Pro 11-inch: out/agent-scratch/x1-glass/ref) measures
/// column_spread=8 contrast=29 (reference=). Each arm stages the scene again under the raw path's debug switches, in
/// every raw process, and reports column_spread_<arm>= and contrast_<arm>=, not judged:
///   whole     every draw a whole program (whole-programs)
///   plain     textures, backings and shadows made during the arm on plain memory (plain-memory), its glass 2 points
///             shorter so the render server makes its intermediates again
///   halfview  a kernel writing a half-float texture through m2v's view sent to m2v, as before T-v2 (half-view-refuse)
///   noprobe   the glass probe disarmed in every process (glass-probe-off, glass-mem-watch-off)
/// The first run arms the render server's split capture (split-capture: `raw: splitcap` lines of the glass shape's
/// draws, tools/benchloop/capture.py). A failed run saves every picture (composited.png, composited-<arm>.png).
struct UIGlassTest: HWTest {
    let id = "ui.glass"
    let name = "UIGlassEffect over stripes and flat grey"
    let subsystem = Subsystem.compositor

    struct Arm {
        let name: String
        let on: [String]
        let off: [String]
        var shorter: CGFloat = 0
    }

    static let reference = (columnSpread: 8, contrast: 29)
    static let arms = [
        Arm(name: "whole", on: ["whole-programs-on"], off: ["whole-programs-off"]),
        Arm(name: "plain", on: ["plain-memory-on"], off: ["plain-memory-off"], shorter: 2),
        Arm(name: "halfview", on: ["half-view-refuse-on"], off: ["half-view-refuse-off"]),
        Arm(name: "noprobe", on: ["glass-probe-off", "glass-mem-watch-off"], off: []),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        GlassProbe.post("split-capture")
        let (outside, inside) = await Self.snapshot(context)
        GlassProbe.post("split-capture-off")
        var metrics = Metrics()
        metrics.set("reference", "\(Self.reference.columnSpread)/\(Self.reference.contrast)")
        var reasons: [String] = []
        outside.judge(&metrics, &reasons, "outside")
        let (stripes, grey) = Self.measure(inside)
        metrics.set("column_spread", Int((stripes.columnSpread * 255).rounded()))
        metrics.set("contrast", Int((stripes.contrast * 255).rounded()))
        metrics.set("grey_spread", Int((grey.spread * 255).rounded()))
        metrics.set("grey_mean", grey.mean.map { String(Int(($0 * 255).rounded())) }.joined(separator: ","))
        var pictures = [("composited.png", inside)]
        for arm in Self.arms {
            arm.on.forEach(GlassProbe.post)
            let (_, armed) = await Self.snapshot(context, shorter: arm.shorter)
            arm.off.forEach(GlassProbe.post)
            let measured = Self.measure(armed).0
            metrics.set("column_spread_" + arm.name, Int((measured.columnSpread * 255).rounded()))
            metrics.set("contrast_" + arm.name, Int((measured.contrast * 255).rounded()))
            pictures.append(("composited-\(arm.name).png", armed))
        }
        let checks: [(String, Bool)] = [
            ("each column constant down the glass (spread <= 6)", stripes.columnSpread <= 6.0 / 255),
            ("grey stays flat (spread <= 6)", grey.spread <= 6.0 / 255),
            ("grey not black", grey.mean.prefix(3).reduce(0, +) > 0.12),
        ]
        reasons += checks.filter { !$0.1 }.map(\.0)
        if !reasons.isEmpty {
            let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            for (file, picture) in pictures { try? picture.png()?.write(to: directory.appendingPathComponent(file)) }
        }
        return reasons.isEmpty ? .pass(metrics) : .fail(LogLine.fitted(reasons), metrics)
    }

    static func measure(_ inside: Raster) -> (Backdrop.Stripes, (spread: Float, mean: [Float])) {
        let inset = inside.height / 16
        let (top, height, half) = (inside.height / 4 + inset, inside.height / 2 - 2 * inset, inside.width / 2)
        return (
            Backdrop.overStripes(inside.cropped(x: inset, y: top, width: half - 2 * inset, height: height)),
            Backdrop.overUniform(inside.cropped(x: half + inset, y: top, width: half - 2 * inset, height: height))
        )
    }

    static func snapshot(_ context: TestContext, shorter: CGFloat = 0) async -> (CompositeCheck, Raster) {
        await UISceneTests.staged(context) { stage, screen -> UIView in
            let root = UIView(frame: Snapshots.aligned(screen))
            root.addSubview(BackdropStripes(frame: CGRect(x: 0, y: 0, width: screen.width / 2, height: screen.height)))
            let grey = UIView(frame: CGRect(x: screen.width / 2, y: 0, width: screen.width / 2, height: screen.height))
            grey.backgroundColor = UIColor(white: 0.5, alpha: 1)
            root.addSubview(grey)
            let glass = UIVisualEffectView(effect: UIGlassEffect(style: .regular))
            glass.frame = Snapshots.aligned(
                CGRect(x: 0, y: screen.height / 4, width: screen.width, height: screen.height / 2 - shorter))
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
    }
}

/// ui.glass while the stripes under the glass and the glass itself move, so the render server captures and blurs a new
/// backdrop for each frame. In every one of the composited frames the glass over the grey must not be black. Each black
/// frame is listed (black=) with its time from the first frame's snapshot (black_at_ms=, frames_ms= the last one's), so
/// it can be found among the render server's command buffers, and saved as black-<frame>.png with the frames either
/// side (frame-<n>.png). encodings= is each frame's encoding (GlassRecording: A regenerated the backdrop pyramid, B
/// reused it, - unrecorded) and black_sig= the first black frame's command buffers (draws/encoders[Llevels]).
struct UIGlassMotionTest: HWTest {
    let id = "ui.glass_motion"
    let name = "UIGlassEffect while its backdrop and the glass move"
    let subsystem = Subsystem.compositor
    static let frames = 24

    func run(_ context: TestContext) async throws -> Outcome {
        let stage = await context.onStage(fullScreen: true) { GlassStage($0) }
        await Snapshots.settle()
        var black: [Int] = []
        var blackAt: [Int] = []
        var darkest: Float = 3
        var encodings = ""
        var blackSignature: String?
        var saved: [String] = []
        var previous: Raster?
        var saveNext = false
        let started = Date()
        let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
        for frame in 0..<Self.frames {
            let (composited, recording) = await MainActor.run { () -> (Raster, GlassRecording) in
                stage.stripes.transform = CGAffineTransform(translationX: CGFloat(frame % 8) * 8, y: 0)
                stage.glass.transform = CGAffineTransform(translationX: 0, y: CGFloat(frame % 4) * 4)
                return GlassRecording.around { Snapshots.composited(stage.root) }
            }
            encodings += recording.encoding
            let level = GlassStage.level(composited)
            darkest = min(darkest, level)
            let isBlack = GlassStage.black(level)
            if isBlack || saveNext {
                try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            }
            if isBlack {
                black.append(frame)
                blackAt.append(Int(Date().timeIntervalSince(started) * 1000))
                blackSignature = blackSignature ?? recording.signature
                if let previous, frame > 0, !black.contains(frame - 1) {
                    saved.append(Self.save(previous, "frame-\(frame - 1).png", directory))
                }
                saved.append(Self.save(composited, "black-\(frame).png", directory))
            } else if saveNext {
                saved.append(Self.save(composited, "frame-\(frame).png", directory))
            }
            saveNext = isBlack
            previous = composited
        }
        await context.clearStage()
        var metrics = Metrics()
        metrics.set("black_frames", black.count)
        metrics.set("frames", Self.frames)
        metrics.set("first_black", black.first ?? -1)
        metrics.set("black", black.isEmpty ? "-" : black.map(String.init).joined(separator: ","))
        metrics.set("encodings", encodings)
        metrics.set("black_sig", blackSignature ?? "-")
        metrics.set("black_at_ms", blackAt.isEmpty ? "-" : blackAt.map(String.init).joined(separator: ","))
        metrics.set("frames_ms", Int(Date().timeIntervalSince(started) * 1000))
        metrics.set("darkest", Int((darkest / 3 * 255).rounded()))
        metrics.set("pngs", saved.isEmpty ? "-" : String(saved.joined(separator: ",").prefix(200)))
        return black.isEmpty
            ? .pass(metrics)
            : .fail("\(black.count) of \(Self.frames) frames show the glass over the grey black", metrics)
    }

    static func save(_ picture: Raster, _ name: String, _ directory: URL) -> String {
        try? picture.png()?.write(to: directory.appendingPathComponent(name))
        return name
    }
}

/// ui.glass_motion's black frames came first after the stage was built: each round here builds a new glass hierarchy,
/// waits as ui.glass_motion does and composites it once. Black in every round makes the first frame of a new glass layer
/// the trigger; black in none puts it on what moves. encodings= is each round's encoding (GlassRecording: A regenerated
/// the backdrop pyramid, B reused it) and sigs= each round's render server command buffers (draws/encoders[Llevels]);
/// each black round is saved as black-<round>.png (pngs=).
struct UIGlassFirstFrameTest: HWTest {
    let id = "ui.glass_first"
    let name = "UIGlassEffect's first composited frame, over a newly built hierarchy each round"
    let subsystem = Subsystem.compositor
    static let rounds = 6

    func run(_ context: TestContext) async throws -> Outcome {
        var black: [Int] = []
        var darkest: Float = 3
        var encodings = ""
        var signatures: [String] = []
        var saved: [String] = []
        let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
        for round in 0..<Self.rounds {
            let result = await GlassRound.run(context)
            darkest = min(darkest, result.level)
            encodings += result.recording.encoding
            signatures.append(result.recording.signature)
            if result.black {
                black.append(round)
                try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                saved.append(UIGlassMotionTest.save(result.picture, "black-\(round).png", directory))
            }
        }
        var metrics = Metrics()
        metrics.set("black_rounds", black.count)
        metrics.set("rounds", Self.rounds)
        metrics.set("black", black.isEmpty ? "-" : black.map(String.init).joined(separator: ","))
        metrics.set("encodings", encodings)
        metrics.set("darkest", Int((darkest / 3 * 255).rounded()))
        metrics.set("sigs", String(signatures.joined(separator: ";").prefix(480)))
        metrics.set("pngs", saved.isEmpty ? "-" : saved.joined(separator: ","))
        return black.isEmpty
            ? .pass(metrics)
            : .fail("\(black.count) of \(Self.rounds) first frames show the glass over the grey black", metrics)
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
