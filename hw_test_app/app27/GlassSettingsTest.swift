import Foundation
import HWTestCore
import HWTestKit
import UIKit

/// Settings' Liquid Glass over its right column going partly black as it appears or the column scrolls, judged on what
/// the display was handed: ui.glass_sidebar's dark split view with a glass pane over the detail (SidebarScene's pane,
/// as Settings' glass bars and buttons stand over its detail), every presented frame of each phase read back and the
/// pane's middle judged as ui.glass_live judges its glass. Phases: scroll (the detail scrolls under the shown pane),
/// appear (the pane shows and hides every SidebarDriver.appearEvery display frames over the still detail), both (it
/// shows and hides while the detail scrolls), sidebar (it shows and hides while the sidebar's list scrolls).
///
/// The result line is ui.glass_sidebar's, judged on the pane: frames=, black=, partial=, probe=, per phase
/// frames/black/partial, its first black frame's sequence and its glass-mem counts' rise, then under= (the detail is
/// behind the pane), region= (the pane in points, x,y,w,h) and means= (the pane's mean level both ways the buffer may
/// turn). Each black frame logs one line with its sequence and mach ticks; every frame is in
/// Documents/ui/ui.glass_settings/frames.json, with PNGs of each phase's first wrong frame and its neighbours.
struct UIGlassSettingsTest: HWTest {
    let id = "ui.glass_settings"
    let name =
        "Liquid Glass appearing over Settings' scrolling detail beside its sidebar, read back per presented frame"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 60 }

    struct Step {
        let name: String
        let sidebar: Bool
        let detail: Bool
        let appears: Bool
    }

    static let settle: TimeInterval = 1
    static let seconds: TimeInterval = 3
    static let steps = [
        Step(name: "scroll", sidebar: false, detail: true, appears: false),
        Step(name: "appear", sidebar: false, detail: false, appears: true),
        Step(name: "both", sidebar: false, detail: true, appears: true),
        Step(name: "sidebar", sidebar: true, detail: false, appears: true),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let (opened, why) = ScanoutSession.open()
        guard let session = opened else { return .skip("no scanout read-back: \(why)") }
        defer { session.close() }
        let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        GlassProbe.arm(lines: false)
        let presented = await MainActor.run { () -> (SidebarScene, UIView)? in
            guard let scene = SidebarScene.present(pane: true), let pane = scene.pane else { return nil }
            return (scene, pane)
        }
        guard case (let scene, let pane)? = presented else {
            return .skip("no key window to present the split view over")
        }
        try? await Task.sleep(nanoseconds: UInt64(Self.settle * 1e9))
        let layout = await MainActor.run { scene.layout() }
        guard layout.shown && !layout.collapsed && layout.sidebar.width > 0 && layout.paneClear else {
            await MainActor.run { scene.dismiss() }
            return .skip(
                "the split view showed no sidebar beside a detail clear of the pane (collapsed=\(layout.collapsed))")
        }
        let paneRect = SidebarLayout.paneRect(in: layout.screen)
        var run = SidebarRun(
            session: session, test: id, layout: layout, band: SidebarLayout.paneBand, directory: directory)
        for step in Self.steps where run.stopped == nil {
            let scrolls = await MainActor.run { () -> [UIScrollView] in
                pane.isHidden = step.appears
                return scene.scrolls(sidebar: step.sidebar, detail: step.detail)
            }
            await run.phase(step.name, seconds: Self.seconds, scrolls: scrolls, appearing: step.appears ? pane : nil)
        }
        await MainActor.run { scene.dismiss() }
        await Snapshots.settle()
        run.write()
        var metrics = Metrics()
        metrics.set("under", layout.detail.contains(paneRect))
        metrics.set(
            "region",
            [paneRect.minX, paneRect.minY, paneRect.width, paneRect.height].map { String(Int($0.rounded())) }
                .joined(separator: ","))
        metrics.set("means", run.means)
        return GlassSidebarVerdict.outcome(
            run.phases, reads: run.reads, scene: metrics, pngs: run.pngs, stopped: run.stopped, subject: "glass pane")
    }
}
