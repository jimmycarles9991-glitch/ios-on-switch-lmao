import Foundation
import HWTestCore
import HWTestGPU
import SwiftUI
import UIKit

public enum UISceneTests {
    static let all: [any HWTest] = [UITextTest(), UIBlurTest(), UIListTest(), UIRenderBoxTest(), CompressedImageTest()]

    /// Shows `build`'s views full screen, waits for them to reach the screen, and returns what `measure` makes of them.
    public static func staged<V, R>(
        _ context: TestContext, _ build: @escaping @MainActor (UIView, CGRect) -> V,
        _ measure: @escaping @MainActor (V) -> R
    ) async -> R {
        let views = await context.onStage(fullScreen: true) { stage in build(stage, UIScreen.main.bounds) }
        await Snapshots.settle()
        return await MainActor.run { measure(views) }
    }

    public static func outcome(
        _ checks: [(String, CompositeCheck)], _ context: TestContext, id: String, metrics: Metrics = Metrics()
    )
        -> Outcome
    {
        var metrics = metrics
        var reasons: [String] = []
        for (prefix, check) in checks {
            let before = reasons.count
            check.judge(&metrics, &reasons, prefix)
            if reasons.count > before { check.save(to: context.documents, id: id, stem: prefix) }
        }
        return reasons.isEmpty ? .pass(metrics) : .fail(LogLine.fitted(reasons), metrics)
    }
}

/// UILabels at every system text style, dark on light and light on dark, as the render server composites their
/// backing stores against the CPU's drawing of the same layers.
struct UITextTest: HWTest {
    let id = "ui.text"
    let name = "Text at every system text style, composited against the CPU"
    let subsystem = Subsystem.compositor

    static let styles: [UIFont.TextStyle] = [
        .largeTitle, .title1, .title2, .title3, .headline, .body, .callout, .subheadline, .footnote, .caption1,
        .caption2,
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let checks = await UISceneTests.staged(context) { stage, screen -> [UIView] in
            [(UIColor.white, UIColor.black), (UIColor(white: 0.11, alpha: 1), UIColor.white)].enumerated().map {
                index, colors in
                let width = (screen.width - 30) / 2
                let panel = UIView(
                    frame: Snapshots.aligned(
                        CGRect(x: 10 + CGFloat(index) * (width + 10), y: 10, width: width, height: screen.height - 20)))
                panel.backgroundColor = colors.0
                let row = panel.bounds.height / CGFloat(Self.styles.count)
                for (line, style) in Self.styles.enumerated() {
                    let label = UILabel(
                        frame: Snapshots.aligned(CGRect(x: 8, y: CGFloat(line) * row, width: width - 16, height: row)))
                    label.font = UIFont.preferredFont(forTextStyle: style)
                    label.textColor = line % 3 == 2 ? .systemBlue : colors.1
                    label.text = "Wi-Fi  Bluetooth  General 0123"
                    panel.addSubview(label)
                }
                stage.addSubview(panel)
                return panel
            }
        } _: { panels in
            zip(["light", "dark"], panels).map { name, panel in
                (name, CompositeCheck(label: name, composited: Snapshots.composited(panel), cpu: Snapshots.cpu(panel)))
            }
        }
        return UISceneTests.outcome(checks, context, id: id)
    }
}

/// A UIVisualEffectView blur across the middle of vertical stripes and of flat grey. Outside it the composited
/// picture must match the CPU's; inside, over the stripes, each column stays constant down its length (no streak or
/// block), neighbouring columns stay close (no sharp edge left) and the stripes lose most of their contrast; over the
/// grey it stays flat and not black.
struct UIBlurTest: HWTest {
    let id = "ui.blur"
    let name = "UIVisualEffectView blur over stripes and flat grey"
    let subsystem = Subsystem.compositor

    func run(_ context: TestContext) async throws -> Outcome {
        let (outside, inside) = await UISceneTests.staged(context) { stage, screen -> UIView in
            let root = UIView(frame: Snapshots.aligned(screen))
            root.addSubview(BackdropStripes(frame: CGRect(x: 0, y: 0, width: screen.width / 2, height: screen.height)))
            let grey = UIView(frame: CGRect(x: screen.width / 2, y: 0, width: screen.width / 2, height: screen.height))
            grey.backgroundColor = UIColor(white: 0.5, alpha: 1)
            root.addSubview(grey)
            let blur = UIVisualEffectView(effect: UIBlurEffect(style: .regular))
            blur.frame = Snapshots.aligned(
                CGRect(x: 0, y: screen.height / 4, width: screen.width, height: screen.height / 2))
            root.addSubview(blur)
            stage.addSubview(root)
            return root
        } _: { root -> (CompositeCheck, Raster) in
            let (composited, cpu) = (Snapshots.composited(root), Snapshots.cpu(root))
            let band = composited.height / 4
            return (
                CompositeCheck(
                    label: "outside the blur",
                    composited: composited.cropped(x: 0, y: 0, width: composited.width, height: band - 2),
                    cpu: cpu.cropped(x: 0, y: 0, width: cpu.width, height: band - 2)),
                composited
            )
        }
        var metrics = Metrics()
        var reasons: [String] = []
        outside.judge(&metrics, &reasons, "outside")
        let inset = 40
        let (top, height, half) = (inside.height / 4 + inset, inside.height / 2 - 2 * inset, inside.width / 2)
        let stripes = Backdrop.overStripes(inside.cropped(x: inset, y: top, width: half - 2 * inset, height: height))
        let grey = Backdrop.overUniform(
            inside.cropped(x: half + inset, y: top, width: half - 2 * inset, height: height))
        metrics.set("column_spread", Int((stripes.columnSpread * 255).rounded()))
        metrics.set("step", Int((stripes.step * 255).rounded()))
        metrics.set("contrast", Int((stripes.contrast * 255).rounded()))
        metrics.set("grey_spread", Int((grey.spread * 255).rounded()))
        metrics.set("grey_mean", grey.mean.map { String(Int(($0 * 255).rounded())) }.joined(separator: ","))
        let checks: [(String, Bool)] = [
            ("each column constant down the blur (spread <= 6)", stripes.columnSpread <= 6.0 / 255),
            ("no edge left (step <= 64)", stripes.step <= 64.0 / 255),
            ("stripes blurred (contrast <= 70%)", stripes.contrast <= 0.7),
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

/// Vertical stripes 16 points wide, red and blue, drawn by Core Animation (no backing store).
public final class BackdropStripes: UIView {
    override public init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .blue
        for x in stride(from: CGFloat(0), to: frame.width, by: 32) {
            let stripe = UIView(frame: Snapshots.aligned(CGRect(x: x, y: 0, width: 16, height: frame.height)))
            stripe.backgroundColor = .red
            addSubview(stripe)
        }
    }

    required init?(coder: NSCoder) { nil }
}

/// A Settings-like inset grouped list (symbols, labels, disclosure indicators, separators) scrolled part way down.
struct UIListTest: HWTest {
    let id = "ui.list"
    let name = "Inset grouped list, composited against the CPU"
    let subsystem = Subsystem.compositor

    func run(_ context: TestContext) async throws -> Outcome {
        let check = await UISceneTests.staged(context) { stage, screen -> UITableView in
            let table = SettingsList.make(Snapshots.aligned(screen))
            stage.addSubview(table)
            table.layoutIfNeeded()
            table.contentOffset = CGPoint(x: 0, y: 400)
            return table
        } _: { table in
            CompositeCheck(label: "list", composited: Snapshots.composited(table), cpu: Snapshots.cpu(table))
        }
        return UISceneTests.outcome([("list", check)], context, id: id)
    }
}

/// The same SwiftUI picture twice: once through .drawingGroup(), which RenderBox renders with Metal in this process
/// (its pack states draw on the raw path: raw_cbs= says how many), once as plain Core Animation layers. Their
/// composited pictures must agree to within anti-aliasing.
struct UIRenderBoxTest: HWTest {
    let id = "ui.renderbox"
    let name = "SwiftUI drawingGroup (RenderBox in process) against plain layers"
    let subsystem = Subsystem.compositor

    func run(_ context: TestContext) async throws -> Outcome {
        let device = MTLCreateSystemDefaultDevice()
        let before = device.map(RawFacade.counters) ?? [:]
        let check = await UISceneTests.staged(context) {
            stage, screen -> [UIHostingController<RenderBoxPicture>] in
            [true, false].enumerated().map { index, grouped in
                let host = UIHostingController(rootView: RenderBoxPicture(grouped: grouped))
                host.view.frame = Snapshots.aligned(
                    CGRect(
                        x: 10 + CGFloat(index) * (screen.width / 2), y: 10, width: screen.width / 2 - 20,
                        height: screen.height - 20))
                stage.addSubview(host.view)
                return host
            }
        } _: { hosts in
            let views = hosts.map(\.view!)
            return CompositeCheck(
                label: "drawingGroup", composited: Snapshots.composited(views[0]), cpu: Snapshots.composited(views[1]))
        }
        var metrics = Metrics()
        if let device { metrics.merge(PathTally.counterMetrics(before: before, after: RawFacade.counters(of: device))) }
        return UISceneTests.outcome([("group", check)], context, id: id, metrics: metrics)
    }
}

struct RenderBoxPicture: View {
    let grouped: Bool

    var body: some View {
        if grouped { picture.drawingGroup() } else { picture }
    }

    var picture: some View {
        ZStack {
            LinearGradient(colors: [.orange, .purple], startPoint: .top, endPoint: .bottom)
            VStack(spacing: 12) {
                Text("Wi-Fi").font(.largeTitle.bold()).foregroundStyle(.white)
                Text("Bluetooth  General  0123").font(.body).foregroundStyle(.white)
                HStack(spacing: 8) {
                    ForEach(0..<6) { index in
                        RoundedRectangle(cornerRadius: 6).fill(
                            Color(hue: Double(index) / 6, saturation: 0.8, brightness: 0.9)
                        )
                        .frame(width: 28, height: 28)
                    }
                }
                Circle().stroke(.white, lineWidth: 3).frame(width: 60, height: 60)
            }
        }
    }
}
