import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import SwiftUI
import UIKit

/// The hard scroll edge effect's keyline on SwitchHWTest's list (PocketKeylineProbe's layers: UIKit's translucent white
/// "Keyline" at the pocket's bottom pixel row, which Core Animation draws with PBGRABsovXmw_A3Xhfc), and which
/// explanation of its purple row the numbers support: claim_verdict= and claim_why= (core/KeylineClaim.swift).
/// line_rgb=, above_rgb= and below_rgb= are the composited picture's line row and the rows either side at its middle:
/// line_row= is the purple or fresh row within two of the keyline's last pixel row (keyline_row=), else that row, and
/// line_offset= the rows between them; line_range= each channel's least and greatest along it. Before that picture the
/// test arms the render server's keyline debug lines (armRawProbe). fresh_texels= counts the picture's
/// texels holding Metal's fresh texture texel (opaque magenta: what a new private texture reads where nothing wrote it)
/// and fresh_rows= the rows they lie on. Then with the keyline hidden (nokeyline), the Pocket hidden (nopocket), the
/// Pocket without its colour matrix and SDR filters (nomatrix), the keyline tinted at its own alpha (tint), grown three
/// pixels upwards (tall) and moved a pixel up (up): <run>_rgb at the line row and <run>_fresh, the fresh texels within
/// three rows of it. expect_rgb= is the keyline's own fill over nokeyline_rgb, source-over in unorm8; tint_want= the
/// tint's.
/// A debug kernel's read-back gives the framebuffer at the keyline (framebuffer_line_rgb=, framebuffer_line_at=) and its
/// fresh texels (framebuffer_fresh=, framebuffer_fresh_cols=, framebuffer_fresh_rows=); readback= is ok or why not.
/// PocketKeylineReplicaRun adds the keyline's state drawn offscreen. A keyline fill other than J181's
/// (keyline_verdict=), fresh texels in the render or the framebuffer (not counted against a magenta fill, which draws
/// the same value), or a purple keyline row fail the test.
struct PocketKeylineTest: HWTest {
    let id = "ui.pocket_keyline"
    let name = "Scroll edge keyline: its row rendered, in the framebuffer and in an offscreen replica of its state"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 180 }

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
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
        guard let (layers, row, found) = await MainActor.run(body: { PocketKeylineProbe.describe(host.view) }) else {
            metrics.set("keyline", "none")
            return .fail("UIKit made no Keyline layer for the hard scroll edge effect", metrics)
        }
        metrics.merge(found)
        Self.armRawProbe()
        try? await Task.sleep(nanoseconds: 100_000_000)
        let picture = await MainActor.run { Snapshots.composited(host.view) }
        guard let row = Self.lineRow(picture, keyline: row, &metrics) else {
            return .fail("the keyline row \(row) is outside the \(picture.height)-row picture", metrics)
        }
        let x = picture.width / 2
        let line = Self.rgb(picture, x, row)
        metrics.set("line_rgb", Self.text(line))
        metrics.set("above_rgb", PocketStripTest.bytes(picture, x, row - 1))
        metrics.set("below_rgb", PocketStripTest.bytes(picture, x, row + 1))
        metrics.set("line_range", PocketStripTest.range(picture, row))
        let fresh = Self.freshRows(picture)
        metrics.set("fresh_texels", fresh.values.reduce(0, +))
        metrics.set("fresh_rows", PocketStripTest.ranges(fresh.keys.sorted()))
        let alpha = await MainActor.run {
            (layers.keyline.backgroundColor?.alpha ?? 0) * CGFloat(layers.keyline.opacity)
        }
        let variants = await Self.variants(layers, row: row, alpha: alpha, view: host.view, &metrics)
        let backdrop = variants["nokeyline"]
        if let backdrop {
            let rgb = layers.fill.map { Array($0.prefix(3)) } ?? [1, 1, 1]
            metrics.set("expect_rgb", Self.text(KeylineClaim.expected(over: backdrop, alpha: Double(alpha), rgb: rgb)))
            let tint = KeylineClaim.expected(over: backdrop, alpha: Double(alpha), rgb: [1, 0.5, 0.25])
            metrics.set("tint_want", Self.text(tint))
        }
        let (framebuffer, images) = await Self.readBack(host.view, row: row, &metrics)
        let replica = await PocketKeylineReplicaRun.run(&metrics)
        let (claim, why) = KeylineClaim.verdict(
            KeylineClaim.Evidence(
                line: line, backdrop: backdrop, framebuffer: framebuffer, replicaWrong: replica, alpha: Double(alpha),
                fill: layers.fill))
        var reasons: [String] = []
        if let problem = PocketKeylineProbe.fillProblem(layers) { reasons.append(problem) }
        if !(layers.fill.map(KeylineClaim.magenta) ?? false) {
            if !fresh.isEmpty { reasons.append("\(fresh.values.reduce(0, +)) fresh-texture texels in the render") }
            if let count = metrics["framebuffer_fresh"], count != .integer(0) {
                reasons.append("fresh-texture texels in the framebuffer")
            }
        }
        if KeylineClaim.purple(line) { reasons.append("the keyline row is purple (\(Self.text(line)))") }
        var all = Metrics()
        all.set("claim_verdict", claim)
        all.set("claim_why", why.replacingOccurrences(of: " ", with: "_"))
        all.merge(metrics)
        guard !reasons.isEmpty else { return .pass(all) }
        let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for (name, image) in [("render", picture)] + images {
            try? image.png()?.write(to: directory.appendingPathComponent("\(name).png"))
        }
        return .fail(LogLine.fitted(reasons), all)
    }

    /// The render server's debug lines for its Core Animation draws and short writes, for 30 s from its notification
    /// handler (raw_gpu.h armKeylineProbe): their budgets were spent long before this test
    static func armRawProbe() {
        CFNotificationCenterPostNotification(
            CFNotificationCenterGetDarwinNotifyCenter(),
            CFNotificationName("com.brunostjohn.switch.debug.keyline-probe" as CFString), nil, nil, true)
    }

    /// The purple or fresh row nearest the keyline's last pixel row, within two rows of it (a fractional edge at the UI's
    /// scale can put the drawn row beside the one the layer's frame rounds to), else that row; line_row= and line_offset=
    static func lineRow(_ picture: Raster, keyline: Int, _ metrics: inout Metrics) -> Int? {
        guard keyline > 0, keyline + 1 < picture.height else { return nil }
        let x = picture.width / 2
        let near = [0, -1, 1, -2, 2].map { keyline + $0 }.filter { $0 > 0 && $0 + 1 < picture.height }
        let row =
            near.first { KeylineClaim.purple(rgb(picture, x, $0)) || KeylineClaim.fresh(rgb(picture, x, $0)) }
            ?? keyline
        metrics.set("line_row", row)
        metrics.set("line_offset", row - keyline)
        return row
    }

    @MainActor
    static func change(_ name: String, _ layers: PocketKeylineProbe.Layers, alpha: CGFloat, scale: CGFloat) {
        switch name {
        case "nokeyline": layers.keyline.isHidden = true
        case "nopocket": layers.pocket?.isHidden = true
        case "nomatrix":
            layers.pocket?.filters = layers.pocket?.filters?.filter { !PocketKeylineProbe.matrixFilter($0) }
        case "tint": layers.keyline.backgroundColor = CGColor(srgbRed: 1, green: 0.5, blue: 0.25, alpha: alpha)
        case "tall":
            var frame = layers.keyline.frame
            frame.origin.y -= 3 / scale
            frame.size.height += 3 / scale
            layers.keyline.frame = frame
        default: layers.keyline.frame = layers.keyline.frame.offsetBy(dx: 0, dy: -1 / scale)
        }
    }

    /// Each variant's keyline-row colour, by name, with <run>_rgb and <run>_fresh set
    static func variants(
        _ layers: PocketKeylineProbe.Layers, row: Int, alpha: CGFloat, view: UIView, _ metrics: inout Metrics
    ) async -> [String: [Int]] {
        let (color, filters, frame, scale) = await MainActor.run {
            (
                layers.keyline.backgroundColor, layers.pocket?.filters, layers.keyline.frame,
                view.window?.screen.scale ?? 1
            )
        }
        var found: [String: [Int]] = [:]
        for name in ["nokeyline", "nopocket", "nomatrix", "tint", "tall", "up"] {
            await MainActor.run {
                CATransaction.begin()
                CATransaction.setDisableActions(true)
                change(name, layers, alpha: alpha, scale: scale)
                CATransaction.commit()
            }
            await Snapshots.settle()
            let picture = await MainActor.run { Snapshots.composited(view) }
            if row < picture.height {
                found[name] = rgb(picture, picture.width / 2, row)
                metrics.set(name + "_rgb", text(found[name]!))
                metrics.set(name + "_fresh", freshRows(picture, near: row).values.reduce(0, +))
            }
            await MainActor.run {
                CATransaction.begin()
                CATransaction.setDisableActions(true)
                layers.keyline.isHidden = false
                layers.keyline.backgroundColor = color
                layers.keyline.frame = frame
                layers.pocket?.filters = filters
                layers.pocket?.isHidden = false
                CATransaction.commit()
            }
        }
        await Snapshots.settle()
        return found
    }

    /// The framebuffer's colour at the keyline (its purple line where it has one, else the keyline row's place: the
    /// panel's portrait buffer holds the UI's rows as columns) and the pictures to save
    static func readBack(_ view: UIView, row: Int, _ metrics: inout Metrics) async -> ([Int]?, [(String, Raster)]) {
        let result = await ScanoutReadback.capture(nudge: { PocketStripTest.nudge(view) })
        guard case .frame(let frame) = result else {
            if case .unavailable(let why) = result { metrics.set("readback", why) }
            return (nil, [])
        }
        metrics.set("readback", "ok")
        let image = frame.framebuffer
        var texels = 0
        var (columns, rows) = (Set<Int>(), Set<Int>())
        for y in 0..<image.height {
            for x in 0..<image.width where fresh(image, x, y) {
                texels += 1
                columns.insert(x)
                rows.insert(y)
            }
        }
        metrics.set("framebuffer_fresh", texels)
        metrics.set("framebuffer_fresh_cols", PocketStripTest.ranges(columns.sorted()))
        metrics.set("framebuffer_fresh_rows", PocketStripTest.ranges(rows.sorted()))
        let portrait = image.width < image.height
        let (purpleColumn, purpleRow) = (
            PocketStripTest.purpleColumns(image).first, PocketStripTest.purpleRows(image).first
        )
        let at: (Int, Int, String)
        if let column = purpleColumn {
            at = (column, image.height / 2, "col\(column)")
        } else if let line = purpleRow {
            at = (image.width / 2, line, "row\(line)")
        } else if portrait, row < image.width {
            at = (row, image.height / 2, "col\(row)")
        } else {
            at = (image.width / 2, min(row, image.height - 1), "row\(row)")
        }
        let colour = rgb(image, at.0, at.1)
        metrics.set("framebuffer_line_rgb", text(colour))
        metrics.set("framebuffer_line_at", at.2)
        return (colour, [("framebuffer", image)])
    }

    static func rgb(_ picture: Raster, _ x: Int, _ y: Int) -> [Int] {
        (0..<3).map { PocketStripTest.byte(picture[x, y, $0]) }
    }

    static func text(_ values: [Int]) -> String { values.map(String.init).joined(separator: ",") }

    static func fresh(_ picture: Raster, _ x: Int, _ y: Int) -> Bool { KeylineClaim.fresh(rgb(picture, x, y)) }

    /// The rows holding fresh texels (within three rows of `near`, else anywhere) and how many each holds
    static func freshRows(_ picture: Raster, near: Int? = nil) -> [Int: Int] {
        let rows = near.map { max($0 - 3, 0)..<min($0 + 4, picture.height) } ?? 0..<picture.height
        var found: [Int: Int] = [:]
        for y in rows {
            let count = (0..<picture.width).filter { fresh(picture, $0, y) }.count
            if count > 0 { found[y] = count }
        }
        return found
    }
}
