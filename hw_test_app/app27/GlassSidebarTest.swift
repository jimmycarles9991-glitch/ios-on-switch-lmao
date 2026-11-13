import Foundation
import HWTestCore
import HWTestKit
import UIKit

/// Settings' Liquid Glass sidebar blinking black for single frames while its list or the detail beside it scrolls,
/// judged on what the display was handed: the sidebar split view of SidebarScene, dark, with every presented frame of
/// each moving phase read back (ScanoutSession) and the sidebar's band judged as ui.glass_live judges its glass.
/// Phases: idle1 (12 s with no commit, past the driver's 10 s idle reclaim), sidebar (the list scrolls every display
/// frame), idle2, detail (the detail scrolls, so the blur behind the sidebar is rebuilt every frame), both.
///
/// The render server's glass memory probe (GlassMemWords, DEVELOPMENT builds) is read at each phase boundary and its
/// record armed before each moving phase. The result line: frames=, black=, partial=, probe= (- without the probe),
/// reclaim= (yes: the pool showed the idle reclaim after both idle phases; reported, not judged: the black does not
/// need it), under= (the detail is behind the judged
/// band), region= (the band in points, x,y,w,h), means= (both candidate bands' mean level, a/b: the sidebar's should be
/// the lower, the other near the stripes' 0.7), then per moving phase frames/black/partial and its first black frame's
/// sequence, per phase its glass-mem counts' rise (cbs/changed/nonfinite/moved/zero), per idle phase the pool before
/// it and the first one published after it (MB,chunks,backings,backingMB,compressed,made). Each black frame logs one
/// line with its sequence and mach ticks; every frame is in Documents/ui/ui.glass_sidebar/frames.json, with PNGs of each
/// phase's first black frame and its neighbours.
struct UIGlassSidebarTest: HWTest {
    let id = "ui.glass_sidebar"
    let name = "Settings' Liquid Glass sidebar over scrolling content after idle, read back per presented frame"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 120 }

    struct Step {
        let name: String
        let seconds: TimeInterval
        let sidebar: Bool
        let detail: Bool
    }

    static let settle: TimeInterval = 1
    static let idle: TimeInterval = 12
    static let steps = [
        Step(name: "idle1", seconds: idle, sidebar: false, detail: false),
        Step(name: "sidebar", seconds: 4, sidebar: true, detail: false),
        Step(name: "idle2", seconds: idle, sidebar: false, detail: false),
        Step(name: "detail", seconds: 4, sidebar: false, detail: true),
        Step(name: "both", seconds: 2, sidebar: true, detail: true),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let (opened, why) = ScanoutSession.open()
        guard let session = opened else { return .skip("no scanout read-back: \(why)") }
        defer { session.close() }
        let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        GlassProbe.arm(lines: false)
        guard let scene = await MainActor.run(body: { SidebarScene.present() }) else {
            return .skip("no key window to present the split view over")
        }
        try? await Task.sleep(nanoseconds: UInt64(Self.settle * 1e9))
        let layout = await MainActor.run { scene.layout() }
        guard layout.shown && !layout.collapsed && layout.sidebar.width > 0 else {
            await MainActor.run { scene.dismiss() }
            return .skip("the split view showed no sidebar beside the detail (collapsed=\(layout.collapsed))")
        }
        var run = SidebarRun(session: session, test: id, layout: layout, band: layout.band, directory: directory)
        for step in Self.steps where run.stopped == nil {
            let scrolls = await MainActor.run { scene.scrolls(sidebar: step.sidebar, detail: step.detail) }
            await run.phase(step.name, seconds: step.seconds, scrolls: scrolls)
        }
        await MainActor.run { scene.dismiss() }
        await Snapshots.settle()
        run.write()
        var metrics = Metrics()
        metrics.set("under", layout.under)
        metrics.set("region", layout.regionText)
        metrics.set("means", run.means)
        return GlassSidebarVerdict.outcome(
            run.phases, reads: run.reads, scene: metrics, pngs: run.pngs, stopped: run.stopped)
    }
}

/// The phases ui.glass_sidebar (or ui.glass_settings) ran, the probe words read at their boundaries and each moving
/// phase's frames.
struct SidebarRun {
    let session: ScanoutSession
    let test: String
    let layout: SidebarLayout
    let band: SidebarBand
    let directory: URL
    private(set) var phases: [GlassSidebarVerdict.Phase] = []
    private(set) var reads: [GlassMemWords] = [.read()]
    private(set) var frames: [(String, [SidebarFrame])] = []
    private(set) var pngs: [String] = []
    private(set) var stopped: String?

    /// The two bands' mean levels over every frame read, a/b: the sidebar's is the lower one, the other the stripes'.
    var means: String {
        let all = frames.flatMap(\.1)
        guard !all.isEmpty else { return "-" }
        let mean = { (band: (SidebarFrame) -> Double) in all.map(band).reduce(0, +) / Double(all.count) }
        return String(format: "%.2f/%.2f", mean { $0.a.mean }, mean { $0.b.mean })
    }

    /// An idle phase (no scroll views and nothing appearing) only waits: no display link, timer or read-back runs, so
    /// nothing commits.
    mutating func phase(_ name: String, seconds: TimeInterval, scrolls: [UIScrollView], appearing: UIView? = nil) async
    {
        guard !scrolls.isEmpty || appearing != nil else {
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
            phases.append(.init(name: name, moving: false))
            reads.append(.read())
            return
        }
        GlassProbe.arm(lines: false)
        let (session, test, band, directory) = (self.session, self.test, self.band, self.directory)
        let reader = await MainActor.run {
            SidebarReader(session: session, test: test, phase: name, band: band, directory: directory)
        }
        guard session.rearm() else {
            stopped = "\(name):arm"
            return
        }
        let driver = await MainActor.run { SidebarDriver(scrolls, appearing: appearing) }
        await reader.run(until: Date().addingTimeInterval(seconds))
        await MainActor.run { driver.stop() }
        try? await Task.sleep(nanoseconds: 100_000_000)
        phases.append(
            .init(
                name: name, moving: true, frames: reader.frames.count, black: reader.black, partial: reader.partial,
                pools: reader.pools))
        reads.append(.read())
        frames.append((name, reader.frames))
        pngs += reader.saved
        stopped = reader.failure.map { "\(name):\($0)" }
    }

    func write() {
        struct Phase: Encodable {
            let name: String
            let frames: [SidebarFrame]
        }
        struct Record: Encodable {
            let sidebar: [Double]
            let detail: [Double]
            let region: [Double]
            let bandX: [Double]
            let mirroredX: [Double]
            let bandY: [Double]
            let reads: [[UInt64]]
            let phases: [Phase]
        }
        let rect = { (r: CGRect) in [r.minX, r.minY, r.width, r.height].map(Double.init) }
        let record = Record(
            sidebar: rect(layout.sidebar), detail: rect(layout.detail), region: rect(layout.region),
            bandX: [band.x.lowerBound, band.x.upperBound],
            mirroredX: [band.mirroredX.lowerBound, band.mirroredX.upperBound],
            bandY: [band.y.lowerBound, band.y.upperBound], reads: reads.map { [$0.mem, $0.pool] },
            phases: frames.map { Phase(name: $0.0, frames: $0.1) })
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try? encoder.encode(record).write(to: directory.appendingPathComponent("frames.json"))
    }
}

extension GlassMemWords {
    static func read() -> GlassMemWords {
        GlassMemWords(mem: GlassProbe.state(GlassProbe.prefix + "glass-mem") ?? 0, pool: pool())
    }

    static func pool() -> UInt64 { GlassProbe.state(GlassProbe.prefix + "glass-mem.pool") ?? 0 }
}
