import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import SwiftUI
import UIKit

/// SwitchHWTest's own screen in small (an inset grouped List in a NavigationStack under a large title and a toolbar
/// button), scrolled so rows sit under the bar, composited by the render server (drawHierarchy(afterScreenUpdates:),
/// which draws the bar's scroll edge effect, its variable blur and pocket mask, as backboardd does). It runs with the
/// top edge effect shown and again hidden (.scrollEdgeEffectHidden). shown_rows= and hidden_rows= count the rows most
/// of whose pixels are purple (R and B each over G by 40 levels: magenta, and a magenta line blended over the list or
/// the bar), shown_at= and hidden_at= list them ("-" for none);
/// bar= is the bar's bottom edge in pixels. pocket= is the mean difference x 255 between the two pictures from the top
/// to 40 points below the bar: 0 means the snapshot holds no edge effect, and without purple rows the test skips.
/// Any purple row fails it. The list holds greys, white and system blue, none of which passes.
///
/// With the edge effect shown, a debug kernel's scanout read-back (ScanoutReadback) also gives the frame's
/// framebuffer (what backboardd rendered for the panel, before the display's copy) and its scanout buffer (what window A
/// scans), judged the same way: framebuffer_rows/at, scanout_rows/at. stage= is the first of render, framebuffer and
/// scanout with purple rows; "none" puts a strip the panel shows after the scanout buffer (the display controller's
/// colour pipeline or window B, or the panel). copy_rows= counts the rows the display's copy changed (it moves bytes:
/// any is a failure); direct=1 when window A scans the framebuffer itself. readback= is ok or why there is none.
struct PocketStripTest: HWTest {
    let id = "display.pocket_strip"
    let name = "Navigation bar scroll edge effect: purple rows, shown and hidden"
    let subsystem = Subsystem.display

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var reasons: [String] = []
        var pictures: [Raster] = []
        var saved: [(String, Raster)] = []
        var bar = 0
        for (name, hidden) in [("shown", false), ("hidden", true)] {
            let (picture, barBottom, view) = await Self.snapshot(context, hidden: hidden)
            let rows = Self.purpleRows(picture)
            metrics.set(name + "_rows", rows.count)
            metrics.set(name + "_at", Self.ranges(rows))
            if !rows.isEmpty { reasons.append("\(rows.count) purple rows with the edge effect \(name)") }
            pictures.append(picture)
            saved.append((name, picture))
            bar = barBottom
            if !hidden { saved += await Self.readBack(view, renderRows: rows, &metrics, &reasons) }
        }
        metrics.set("bar", bar)
        let scale = await MainActor.run { UIScreen.main.scale }
        let band = min(pictures[0].height, pictures[1].height, bar + Int(40 * scale))
        let pocket = Self.meanDifference(pictures[0], pictures[1], rows: band)
        metrics.set("pocket", Int((pocket * 255).rounded()))
        if !reasons.isEmpty {
            let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            for (name, picture) in saved {
                try? picture.png()?.write(to: directory.appendingPathComponent("\(name).png"))
            }
            return .fail(LogLine.fitted(reasons), metrics)
        }
        if pocket * 255 < 0.5 { return .skip("the snapshots hold no scroll edge effect", metrics) }
        return .pass(metrics)
    }

    /// The framebuffer and scanout buffer of a frame of the list on stage, judged as its render was; the pictures to
    /// save on a failure.
    static func readBack(
        _ view: UIView, renderRows: [Int], _ metrics: inout Metrics, _ reasons: inout [String]
    ) async
        -> [(String, Raster)]
    {
        let result = await ScanoutReadback.capture(nudge: { nudge(view) })
        guard case .frame(let frame) = result else {
            if case .unavailable(let why) = result { metrics.set("readback", why) }
            if !renderRows.isEmpty { metrics.set("stage", "render") }
            return []
        }
        metrics.set("readback", "ok")
        metrics.set("direct", frame.direct)
        metrics.set("copy_rows", frame.copyRows)
        var stage = renderRows.isEmpty ? nil : "render"
        let images = [("framebuffer", frame.framebuffer), ("scanout", frame.scanout)]
        for (name, image) in images {
            let rows = purpleRows(image)
            metrics.set(name + "_rows", rows.count)
            metrics.set(name + "_at", ranges(rows))
            if !rows.isEmpty {
                reasons.append("\(rows.count) purple rows in the \(name)")
                stage = stage ?? name
            }
        }
        metrics.set("stage", stage ?? "none")
        if !frame.direct && frame.copyRows > 0 { reasons.append("the display's copy changed \(frame.copyRows) rows") }
        return images
    }

    /// A grey dot in the corner, toggled: the render server draws a frame for it, which the display then presents.
    @MainActor
    static func nudge(_ view: UIView) {
        let tag = 0x5242
        let dot =
            view.viewWithTag(tag)
            ?? {
                let dot = UIView(frame: CGRect(x: view.bounds.maxX - 4, y: view.bounds.maxY - 4, width: 2, height: 2))
                dot.tag = tag
                dot.backgroundColor = .gray
                view.addSubview(dot)
                return dot
            }()
        dot.alpha = dot.alpha == 1 ? 0.9 : 1
    }

    /// The composited picture, the navigation bar's bottom edge in its pixels, and the view on stage.
    static func snapshot(_ context: TestContext, hidden: Bool) async -> (Raster, Int, UIView) {
        let host = await context.onStage(fullScreen: true) { stage -> UIHostingController<PocketList> in
            let host = UIHostingController(rootView: PocketList(hidden: hidden))
            host.view.frame = Snapshots.aligned(UIScreen.main.bounds)
            stage.addSubview(host.view)
            return host
        }
        await Snapshots.settle()
        await MainActor.run {
            guard let scroll = first(UICollectionView.self, in: host.view) ?? first(UIScrollView.self, in: host.view)
            else { return }
            scroll.contentOffset.y = 400 - scroll.adjustedContentInset.top
        }
        await Snapshots.settle()
        return await MainActor.run {
            let view = host.view!
            let bar = first(UINavigationBar.self, in: view).map { $0.convert($0.bounds, to: view).maxY } ?? 0
            return (Snapshots.composited(view), Int((bar * (view.window?.screen.scale ?? 1)).rounded()), view)
        }
    }

    @MainActor
    static func first<T: UIView>(_ type: T.Type, in view: UIView) -> T? {
        if let match = view as? T { return match }
        for subview in view.subviews {
            if let match = first(type, in: subview) { return match }
        }
        return nil
    }

    static func purpleRows(_ picture: Raster) -> [Int] {
        let margin: Float = 40.0 / 255
        return (0..<picture.height).filter { y in
            let purple = (0..<picture.width).filter { x in
                let green = picture[x, y, 1]
                return picture[x, y, 0] - green > margin && picture[x, y, 2] - green > margin
            }
            return purple.count * 2 > picture.width
        }
    }

    /// "412-413,500"; "-" for none.
    static func ranges(_ rows: [Int]) -> String {
        var runs: [(Int, Int)] = []
        for row in rows {
            if let last = runs.last, last.1 == row - 1 { runs[runs.count - 1].1 = row } else { runs.append((row, row)) }
        }
        return runs.isEmpty ? "-" : runs.map { $0.0 == $0.1 ? "\($0.0)" : "\($0.0)-\($0.1)" }.joined(separator: ",")
    }

    static func meanDifference(_ a: Raster, _ b: Raster, rows: Int) -> Float {
        let width = min(a.width, b.width)
        guard rows > 0, width > 0 else { return 0 }
        var sum: Float = 0
        for y in 0..<rows {
            for x in 0..<width {
                for c in 0..<3 { sum += abs(a[x, y, c] - b[x, y, c]) }
            }
        }
        return sum / Float(rows * width * 3)
    }
}

struct PocketList: View {
    let hidden: Bool

    var body: some View {
        NavigationStack {
            List {
                ForEach(0..<6) { section in
                    Section("Section \(section)") {
                        ForEach(0..<8) { row in
                            Label("Row \(section).\(row)  Wi-Fi  Bluetooth  General", systemImage: "circle.fill")
                        }
                    }
                }
            }
            .listStyle(.insetGrouped)
            .scrollEdgeEffectHidden(hidden, for: .top)
            .navigationTitle("Hardware tests")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) { Button("Run All") {} }
            }
        }
    }
}
