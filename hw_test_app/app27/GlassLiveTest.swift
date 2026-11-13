import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import UIKit

/// Liquid Glass on screen while what is behind it moves, judged on what the display was handed rather than on a
/// snapshot: a UIGlassEffect band over white and grey stripes (correct glass there is never near black) and, for each
/// frame the display presents through the GPU, the scanout read-back (ScanoutSession) of the glass band's inside.
/// Variants: still (only a dot far from the glass changes, the control), appear (the glass shows at the first display
/// frame read, the dot toggling on: the frames ui.glass_first's unchanged-tree snapshot reuses the backdrop of), server
/// (Core Animation animates the stripes in the render server), scroll (the app scrolls the stripes every display
/// frame), glass (the glass moves over still stripes), slow (scroll every fourth frame), plain (scroll without the
/// read-back: the frame rate it costs), and appear_whole and glass_whole: appear and glass with every raw process drawing
/// whole programs (debug switch whole-programs), the control for the split draws. The switch stays on across the two
/// variants that share it: its -off and -on, posted back to back, may arrive in either order. After each
/// variant `<variant>_rest=<last>/<after>`: the glass left on screen when the motion stopped against the glass once the
/// backdrop under it is captured again.
///
/// Each frame is black (R, G, B <= 2 over 90% of the band), partial (over 2%), dark or good, at its present and, with a
/// live kernel, at the end of its time on screen (the next present reads it again); changed= counts frames whose pixels
/// changed while shown (an older kernel: between two reads of one present). The result line: black=, partial=,
/// changed=, then per variant frames/black/partial/changed, its black frames' sequence gaps (gap 2: every other frame)
/// and backdrop changed/still/unknown, its render server command buffers a second and how many regenerated the backdrop
/// pyramid. Every frame is in Documents/ui/ui.glass_live/ frames.json; the first black frame of a variant and its
/// neighbours are saved as PNGs there.
struct UIGlassLiveTest: HWTest {
    let id = "ui.glass_live"
    let name = "Liquid Glass on screen over a moving backdrop, read back per presented frame"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 150 }
    static let seconds: TimeInterval = 3.5

    func run(_ context: TestContext) async throws -> Outcome {
        let (opened, why) = ScanoutSession.open()
        guard let session = opened else { return .skip("no scanout read-back: \(why)") }
        defer { session.close() }
        let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var summaries: [LiveSummary] = []
        var pngs: [String] = []
        var stopped: String?
        let variants = LiveVariant.allCases
        for (index, variant) in variants.enumerated() where stopped == nil {
            let forced = variant.forced
            let next = index + 1 < variants.count ? variants[index + 1].forced : nil
            if let forced, index == 0 || variants[index - 1].forced != forced { GlassProbe.post(forced + "-on") }
            let scene = await context.onStage(fullScreen: true) { stage in LiveScene(stage, hidden: variant.appears)
            }
            await Snapshots.settle()
            let (reader, driver) = await MainActor.run {
                (
                    variant.readsBack ? LiveReader(session: session, directory: directory, variant: variant) : nil,
                    LiveDriver(scene: scene, variant: variant)
                )
            }
            GlassProbe.arm(lines: false)
            let start = Date()
            let recordStart = GlassProbe.count()?.count ?? 0
            let read = await LiveReader.run(reader, until: start.addingTimeInterval(Self.seconds), after: recordStart)
            await MainActor.run { driver.stop() }
            let rest = await Self.atRest(reader, scene)
            await context.clearStage()
            var summary = LiveSummary(
                name: variant.rawValue, frames: reader?.frames ?? [], recorded: read.recorded,
                regenerated: read.regenerated, seconds: Date().timeIntervalSince(start))
            summary.rest = rest
            summaries.append(summary)
            pngs += reader?.saved ?? []
            stopped = reader?.failure.map { "\(variant.rawValue):\($0)" }
            if let forced, next != forced || stopped != nil { GlassProbe.post(forced + "-off") }
        }
        return Self.outcome(summaries, live: session.live, stopped: stopped, pngs: pngs, directory: directory)
    }

    /// The glass the motion left on screen against the glass after the backdrop under it is captured again
    @MainActor static func atRest(_ reader: LiveReader?, _ scene: LiveScene) async -> String {
        guard let reader else { return "-" }
        let last = reader.frames.last?.atPresent ?? "-"
        try? await Task.sleep(nanoseconds: 500_000_000)
        let before = reader.frames.count
        scene.scroll.contentOffset.x += 1
        _ = await LiveReader.run(
            reader, until: Date().addingTimeInterval(0.4), after: GlassProbe.count()?.count ?? 0)
        return "\(last)/\(reader.frames.count > before ? reader.frames[before].atPresent : "-")"
    }

    static func outcome(
        _ summaries: [LiveSummary], live: Bool, stopped: String?, pngs: [String], directory: URL
    ) -> Outcome {
        var metrics = Metrics()
        let black = summaries.map { $0.black.count }.reduce(0, +)
        let partial = summaries.map(\.partial).reduce(0, +)
        let changed = summaries.map(\.changedOnScreen).reduce(0, +)
        metrics.set("black", black)
        metrics.set("partial", partial)
        metrics.set("changed", changed)
        metrics.set("frames", summaries.map { $0.frames.count }.reduce(0, +))
        metrics.set("live", live)
        if let stopped { metrics.set("stopped", stopped) }
        for s in summaries {
            metrics.set(s.name, s.field)
            if !s.black.isEmpty {
                metrics.set(s.name + "_gaps", String(s.gaps.prefix(32)))
                metrics.set(s.name + "_moved", s.coincidence)
                metrics.set(s.name + "_first", Int(s.black[0].sequence))
            }
            metrics.set(s.name + "_cbs", "\(Int(s.fps.rounded()))/\(s.regenerated)")
            metrics.set(s.name + "_rest", s.rest)
        }
        metrics.set("pngs", pngs.isEmpty ? "-" : String(pngs.prefix(4).joined(separator: ",").prefix(160)))
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try? encoder.encode(summaries.map(LiveSummary.Detail.init)).write(
            to: directory.appendingPathComponent("frames.json"))
        guard summaries.contains(where: { !$0.frames.isEmpty }) else {
            return .skip("the read-back caught no presented frame", metrics)
        }
        var reasons: [String] = []
        for s in summaries where !s.black.isEmpty {
            reasons.append("\(s.name): \(s.black.count) of \(s.frames.count) frames show the glass black")
        }
        if partial > 0 { reasons.append("\(partial) frames show the glass partly black") }
        if changed > 0 { reasons.append("\(changed) frames changed while on screen") }
        return reasons.isEmpty ? .pass(metrics) : .fail(LogLine.fitted(reasons, budget: 160), metrics)
    }
}

enum LiveVariant: String, CaseIterable {
    case still, appear, server, scroll, glass, slow, plain
    case appearWhole = "appear_whole"
    case glassWhole = "glass_whole"

    var readsBack: Bool { self != .plain }
    var appears: Bool { self == .appear || self == .appearWhole }
    var moves: Bool { self == .glass || self == .glassWhole }

    /// The raw path's debug switch the variant runs under (RawMetalGlassProbe.mm)
    var forced: String? {
        switch self {
        case .appearWhole, .glassWhole: return "whole-programs"
        default: return nil
        }
    }
}

/// The stage of ui.glass_live, laid out symmetric about the screen's centre (LiveImage): stripes in a scroll view
/// across the screen, the glass band over its middle, a dot in a corner the still variant toggles.
@MainActor
struct LiveScene {
    let scroll: UIScrollView
    let stripes: UIView
    let glass: UIView
    let dot: UIView

    init(_ stage: UIView, hidden: Bool = false) {
        let screen = Snapshots.aligned(UIScreen.main.bounds)
        scroll = UIScrollView(frame: screen)
        scroll.showsHorizontalScrollIndicator = false
        stripes = UIView(frame: CGRect(x: 0, y: 0, width: screen.width * 3, height: screen.height))
        stripes.backgroundColor = UIColor(white: 0.45, alpha: 1)
        for x in stride(from: CGFloat(0), to: stripes.bounds.width, by: 48) {
            let stripe = UIView(frame: Snapshots.aligned(CGRect(x: x, y: 0, width: 24, height: screen.height)))
            stripe.backgroundColor = .white
            stripes.addSubview(stripe)
        }
        scroll.addSubview(stripes)
        scroll.contentSize = stripes.bounds.size
        scroll.contentOffset = CGPoint(x: screen.width, y: 0)
        stage.addSubview(scroll)
        glass = UIVisualEffectView(effect: UIGlassEffect(style: .regular))
        glass.frame = Snapshots.aligned(
            CGRect(
                x: screen.width * 0.2, y: screen.height * 0.3, width: screen.width * 0.6, height: screen.height * 0.4))
        glass.isHidden = hidden
        stage.addSubview(glass)
        dot = UIView(frame: CGRect(x: screen.maxX - 6, y: screen.maxY - 6, width: 4, height: 4))
        dot.backgroundColor = .gray
        stage.addSubview(dot)
    }
}

/// Moves the scene every display frame as its variant says.
@MainActor
final class LiveDriver: NSObject {
    let scene: LiveScene
    let variant: LiveVariant
    var link: CADisplayLink?
    var tick = 0

    init(scene: LiveScene, variant: LiveVariant) {
        self.scene = scene
        self.variant = variant
        super.init()
        if variant == .server {
            let move = CABasicAnimation(keyPath: "position.x")
            move.byValue = -scene.stripes.bounds.width / 6
            move.duration = 1
            move.autoreverses = true
            move.repeatCount = .infinity
            scene.stripes.layer.add(move, forKey: "live")
            return
        }
        let link = CADisplayLink(target: self, selector: #selector(step))
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    @objc func step() {
        tick += 1
        let width = scene.stripes.bounds.width / 3
        switch variant {
        case .still: scene.dot.alpha = tick % 2 == 0 ? 1 : 0.9
        case _ where variant.appears:
            scene.dot.alpha = tick % 2 == 0 ? 1 : 0.9
            if tick == 6 { scene.glass.isHidden = false }
        case .scroll, .plain:
            scene.scroll.contentOffset.x = width + CGFloat(tick * 4 % Int(width))
        case .slow:
            if tick % 4 == 0 { scene.scroll.contentOffset.x = width + CGFloat(tick % Int(width)) }
        case _ where variant.moves:
            scene.glass.transform = CGAffineTransform(translationX: 0, y: CGFloat(tick % 16) - 8)
        default: break
        }
    }

    func stop() {
        link?.invalidate()
        link = nil
        scene.stripes.layer.removeAllAnimations()
    }
}
