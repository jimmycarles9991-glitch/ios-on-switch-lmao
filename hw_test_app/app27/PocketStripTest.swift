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
/// strip_rgb and strip_range describe the first purple row; other_rgb samples the other run at that position;
/// strip_above_rgb and strip_below_rgb the rows on either side of it. snapshot= is the shown picture's size and
/// host_at= the list's origin in the window, in pixels.
/// More runs with the edge effect shown, each giving <run>_at= (its purple rows) and <run>_bar= (the bar's bottom edge
/// in that picture): soft and hard force the effect's style (.scrollEdgeEffectStyle), and a strip in hard alone is
/// the hard style's keyline; half and pixel move the list down by half a pixel and by a whole one inside the stage and
/// snapshot the stage, so a strip that comes and goes with the half-pixel move depends on how a fractional edge rounds.
/// PocketKeylineProbe adds the hard effect's layers, the keyline's fill against J181's (keyline_verdict=; any other
/// fails the test) and the keyline row with each of them changed.
///
/// With the edge effect shown, a debug kernel's scanout read-back (ScanoutReadback) also gives the frame's
/// framebuffer (what backboardd rendered for the panel, before the display's copy) and its scanout buffer (what window
/// A scans), judged the same way. Both are the panel's portrait buffer, in which the UI's rows run as columns, so each
/// is judged by rows and by columns: framebuffer_rows/at, framebuffer_cols/cols_at, scanout_rows/at,
/// scanout_cols/cols_at; framebuffer_line_rgb is the first purple line's colour at its middle and
/// framebuffer_beside_rgb the pixels either side of it there. stage= is the first of render, framebuffer and scanout
/// with a purple line; "none" puts a strip the panel shows after the scanout buffer (the display controller's colour
/// pipeline or window B, or the panel). copy_rows= counts the rows the display's copy changed (it moves bytes: any is a
/// failure); direct=1 when window A scans the framebuffer itself. readback= is ok or why there is none.
struct PocketStripTest: HWTest {
    let id = "display.pocket_strip"
    let name = "Navigation bar scroll edge effect: purple rows, shown and hidden"
    let subsystem = Subsystem.display
    var timeout: TimeInterval { 180 }

    struct Shot {
        let picture: Raster
        let bar: Int
        let view: UIView
        let hostOrigin: String
    }

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var reasons: [String] = []
        var pictures: [Raster] = []
        var saved: [(String, Raster)] = []
        var firstRows: [Int?] = []
        var bar = 0
        for (name, hidden) in [("shown", false), ("hidden", true)] {
            let shot = await Self.snapshot(context, hidden: hidden)
            let rows = Self.purpleRows(shot.picture)
            firstRows.append(rows.first)
            metrics.set(name + "_rows", rows.count)
            metrics.set(name + "_at", Self.ranges(rows))
            if !rows.isEmpty { reasons.append("\(rows.count) purple rows with the edge effect \(name)") }
            let pattern = Self.patternPixels(shot.picture)
            metrics.set(name + "_pattern_px", pattern)
            if pattern > 0 { reasons.append("\(pattern) pattern pixels with the edge effect \(name)") }
            pictures.append(shot.picture)
            saved.append((name, shot.picture))
            bar = shot.bar
            if !hidden {
                metrics.set("snapshot", "\(shot.picture.width)x\(shot.picture.height)")
                metrics.set("host_at", shot.hostOrigin)
                saved += await Self.readBack(shot.view, renderRows: rows, &metrics, &reasons)
            }
        }
        metrics.set("bar", bar)
        if let run = firstRows.firstIndex(where: { $0 != nil }), let y = firstRows[run] {
            let (strip, other) = (pictures[run], pictures[1 - run])
            let x = strip.width / 2
            metrics.set("strip_rgb", Self.bytes(strip, x, y))
            metrics.set("strip_range", Self.range(strip, y))
            if y > 0 { metrics.set("strip_above_rgb", Self.bytes(strip, x, y - 1)) }
            if y + 1 < strip.height { metrics.set("strip_below_rgb", Self.bytes(strip, x, y + 1)) }
            if y < other.height { metrics.set("other_rgb", Self.bytes(other, min(strip.width, other.width) / 2, y)) }
        }
        let scale = await MainActor.run { UIScreen.main.scale }
        let band = min(pictures[0].height, pictures[1].height, bar + Int(40 * scale))
        let pocket = Self.meanDifference(pictures[0], pictures[1], rows: band)
        metrics.set("pocket", Int((pocket * 255).rounded()))
        let runs: [(String, ScrollEdgeEffectStyle?, CGFloat)] = [
            ("soft", .soft, 0), ("hard", .hard, 0), ("half", nil, 0.5 / scale), ("pixel", nil, 1 / scale),
        ]
        for (name, style, shift) in runs {
            let shot = await Self.snapshot(context, hidden: false, style: style, shift: shift)
            let rows = Self.purpleRows(shot.picture)
            metrics.set(name + "_at", Self.ranges(rows))
            metrics.set(name + "_bar", shot.bar)
            if !rows.isEmpty { saved.append((name, shot.picture)) }
        }
        saved += await PocketKeylineProbe.run(context, &metrics, &reasons)
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
            let (rows, columns) = (purpleRows(image), purpleColumns(image))
            metrics.set(name + "_rows", rows.count)
            metrics.set(name + "_at", ranges(rows))
            metrics.set(name + "_cols", columns.count)
            metrics.set(name + "_cols_at", ranges(columns))
            if rows.isEmpty && columns.isEmpty { continue }
            reasons.append("\(rows.count + columns.count) purple lines in the \(name)")
            stage = stage ?? name
            if name == "framebuffer" { describeLine(image, row: rows.first, column: columns.first, &metrics) }
        }
        metrics.set("stage", stage ?? "none")
        if !frame.direct && frame.copyRows > 0 { reasons.append("the display's copy changed \(frame.copyRows) rows") }
        return images
    }

    /// framebuffer_line_rgb at the middle of the first purple row or column, framebuffer_beside_rgb the pixels on each
    /// side of it across the line.
    static func describeLine(_ image: Raster, row: Int?, column: Int?, _ metrics: inout Metrics) {
        let points: [(Int, Int)]
        if let row {
            let x = image.width / 2
            points = [(x, row), (x, max(row - 1, 0)), (x, min(row + 1, image.height - 1))]
        } else if let column {
            let y = image.height / 2
            points = [(column, y), (max(column - 1, 0), y), (min(column + 1, image.width - 1), y)]
        } else {
            return
        }
        let colours = points.map { bytes(image, $0.0, $0.1) }
        metrics.set("framebuffer_line_rgb", colours[0])
        metrics.set("framebuffer_beside_rgb", colours[1] + "/" + colours[2])
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

    /// The composited picture (of the stage when the list is moved down by `shift` points, else of the list), the
    /// navigation bar's bottom edge in that picture's pixels, the list's view and its origin in the window in pixels.
    static func snapshot(
        _ context: TestContext, hidden: Bool, style: ScrollEdgeEffectStyle? = nil, shift: CGFloat = 0
    ) async -> Shot {
        let host = await context.onStage(fullScreen: true) { stage -> UIHostingController<PocketList> in
            let host = UIHostingController(rootView: PocketList(hidden: hidden, style: style))
            host.view.frame = Snapshots.aligned(UIScreen.main.bounds).offsetBy(dx: 0, dy: shift)
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
            let pictured = shift == 0 ? view : view.superview ?? view
            let scale = view.window?.screen.scale ?? 1
            let bar = first(UINavigationBar.self, in: view).map { $0.convert($0.bounds, to: pictured).maxY } ?? 0
            let origin = view.convert(CGPoint.zero, to: nil)
            let hostOrigin = [origin.x, origin.y].map { String(format: "%.2f", $0 * scale) }.joined(separator: ",")
            return Shot(
                picture: Snapshots.composited(pictured), bar: Int((bar * scale).rounded()), view: view,
                hostOrigin: hostOrigin)
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

    static func purple(_ picture: Raster, _ x: Int, _ y: Int) -> Bool {
        let margin: Float = 40.0 / 255
        let green = picture[x, y, 1]
        return picture[x, y, 0] - green > margin && picture[x, y, 2] - green > margin
    }

    /// Pixels of a new texture's pattern as Apple's GPUs read it (raw_fresh.h): opaque magenta, 254 where a filter mixed in
    /// 1/255 of a neighbour
    static func patternPixels(_ picture: Raster) -> Int {
        var count = 0
        for y in 0..<picture.height {
            for x in 0..<picture.width
            where byte(picture[x, y, 1]) == 0 && byte(picture[x, y, 0]) >= 254 && byte(picture[x, y, 2]) >= 254 {
                count += 1
            }
        }
        return count
    }

    static func purpleRows(_ picture: Raster) -> [Int] {
        (0..<picture.height).filter { y in
            (0..<picture.width).filter { purple(picture, $0, y) }.count * 2 > picture.width
        }
    }

    static func purpleColumns(_ picture: Raster) -> [Int] {
        (0..<picture.width).filter { x in
            (0..<picture.height).filter { purple(picture, x, $0) }.count * 2 > picture.height
        }
    }

    static func byte(_ value: Float) -> Int { Int((value * 255).rounded()) }

    static func bytes(_ picture: Raster, _ x: Int, _ y: Int) -> String {
        (0..<3).map { String(byte(picture[x, y, $0])) }.joined(separator: ",")
    }

    /// "234-255,44-55,241-255": each channel's least and greatest along row `y`.
    static func range(_ picture: Raster, _ y: Int) -> String {
        (0..<3).map { c in
            let values = (0..<picture.width).map { byte(picture[$0, y, c]) }
            let (least, greatest) = (values.min() ?? 0, values.max() ?? 0)
            return least == greatest ? "\(least)" : "\(least)-\(greatest)"
        }.joined(separator: ",")
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
    let style: ScrollEdgeEffectStyle?

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
            .scrollEdgeEffectStyle(style, for: .top)
            .navigationTitle("Hardware tests")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) { Button("Run All") {} }
            }
        }
    }
}
