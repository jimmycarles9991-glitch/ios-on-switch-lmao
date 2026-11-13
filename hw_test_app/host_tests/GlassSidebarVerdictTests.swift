import Foundation
import HWTestCore
import XCTest

final class GlassSidebarVerdictTests: XCTestCase {
    typealias Phase = GlassSidebarVerdict.Phase

    static func mem(
        cbs: UInt64, changed: UInt64 = 0, nonFinite: UInt64 = 0, moved: UInt64 = 0, zero: UInt64 = 0
    )
        -> UInt64
    {
        cbs | changed << 20 | nonFinite << 32 | moved << 44 | zero << 56
    }

    static func pool(
        mb: UInt64, chunks: UInt64, backings: UInt64, backingMB: UInt64, compressed: UInt64, made: UInt64
    )
        -> UInt64
    {
        mb | chunks << 12 | backings << 20 | backingMB << 28 | compressed << 40 | made << 48
    }

    static let busy = pool(mb: 96, chunks: 12, backings: 30, backingMB: 40, compressed: 8, made: 100)
    static let trimmed = pool(mb: 48, chunks: 6, backings: 0, backingMB: 0, compressed: 0, made: 100)
    static let rebuilt = pool(mb: 96, chunks: 12, backings: 30, backingMB: 40, compressed: 8, made: 108)

    /// The pools each moving phase read: the first commit after an idle publishes the trimmed pool.
    func phases(
        sidebar: [UInt64] = [GlassSidebarVerdictTests.trimmed, GlassSidebarVerdictTests.rebuilt],
        detail: [UInt64] = [GlassSidebarVerdictTests.trimmed, GlassSidebarVerdictTests.rebuilt]
    ) -> [Phase] {
        [
            Phase(name: "idle1", moving: false), Phase(name: "sidebar", moving: true, frames: 240, pools: sidebar),
            Phase(name: "idle2", moving: false), Phase(name: "detail", moving: true, frames: 240, pools: detail),
            Phase(name: "both", moving: true, frames: 120, pools: [Self.rebuilt]),
        ]
    }

    /// The idle phases' ends read what the render server published before them.
    static let stale = [busy, busy, rebuilt, rebuilt, rebuilt, rebuilt]

    func words(_ pools: [UInt64], mems: [UInt64]? = nil) -> [GlassMemWords] {
        zip(mems ?? pools.indices.map { Self.mem(cbs: UInt64($0) * 100) }, pools).map {
            GlassMemWords(mem: $0, pool: $1)
        }
    }

    func testTheWordsFieldsAreWhereTheProbePutsThem() {
        let rise = GlassMemRise(
            Self.mem(cbs: 0xf_fff0, changed: 1, nonFinite: 2, moved: 3, zero: 4),
            Self.mem(cbs: 0x10, changed: 2, nonFinite: 4, moved: 6, zero: 8))
        XCTAssertEqual(rise.text, "32/1/2/3/4")
        XCTAssertTrue(rise.faults)
        let pool = GlassPool(
            word: Self.pool(mb: 4095, chunks: 255, backings: 1, backingMB: 2, compressed: 3, made: 65535))
        XCTAssertEqual(pool.text, "4095,255,1,2,3,65535")
    }

    func testASaturatedCounterIsUnknownNotZero() {
        let full = Self.mem(cbs: 1, changed: 4095)
        let rise = GlassMemRise(full, full)
        XCTAssertNil(rise.changed)
        XCTAssertEqual(rise.text, "0/?/0/0/0")
        XCTAssertFalse(rise.faults)
        XCTAssertEqual(GlassMemRise(Self.mem(cbs: 0, zero: 9), Self.mem(cbs: 0, zero: 2)).zero, 2)
    }

    func testAPassingRunWithTheReclaimShowing() {
        let outcome = GlassSidebarVerdict.outcome(
            phases(), reads: words(Self.stale), scene: Metrics(), pngs: [], stopped: nil)
        XCTAssertEqual(outcome.verdict, .pass)
        XCTAssertEqual(outcome.metrics["reclaim"], .text("yes"))
        XCTAssertEqual(outcome.metrics["idle1_pool"], .text("96,12,30,40,8,100>48,6,0,0,0,100"))
        XCTAssertEqual(outcome.metrics["sidebar"], .text("240/0/0"))
        XCTAssertEqual(outcome.metrics["sidebar_mem"], .text("100/0/0/0/0"))
    }

    func testMadeBuffersAfterTheIdleCountAsTheReclaim() {
        let run = phases(sidebar: [], detail: [])
        let reads = words(Self.stale)
        XCTAssertEqual(GlassSidebarVerdict.reclaimed(0, run, reads), true)
        XCTAssertEqual(GlassSidebarVerdict.reclaimed(2, run, reads), false)
    }

    func testTheIdlesOwnEndIsNotCompared() {
        let reads = words([Self.busy, Self.trimmed, Self.busy, Self.busy, Self.busy, Self.busy])
        let run = phases(sidebar: [Self.busy], detail: [Self.busy])
        XCTAssertEqual(GlassSidebarVerdict.reclaimed(0, run, reads), false)
        XCTAssertNil(GlassSidebarVerdict.reclaimed(2, Array(run.prefix(3)), Array(reads.prefix(4))))
    }

    func testNoReclaimIsReportedNotFailed() {
        let held = words(Array(repeating: Self.busy, count: 6))
        let run = phases(sidebar: [Self.busy], detail: [Self.busy])
        let outcome = GlassSidebarVerdict.outcome(run, reads: held, scene: Metrics(), pngs: [], stopped: nil)
        XCTAssertEqual(outcome.verdict, .pass)
        XCTAssertEqual(outcome.metrics["reclaim"], .text("no"))
        let release = words(Array(repeating: 0, count: 6), mems: Array(repeating: 0, count: 6))
        let quiet = GlassSidebarVerdict.outcome(
            phases(sidebar: [0], detail: [0]), reads: release, scene: Metrics(), pngs: [], stopped: nil)
        XCTAssertEqual(quiet.verdict, .pass)
        XCTAssertEqual(quiet.metrics["probe"], .text("-"))
        XCTAssertEqual(quiet.metrics["reclaim"], .text("-"))
        XCTAssertNil(quiet.metrics["sidebar_mem"])
    }

    func testBlackFramesAndProbeFaultsFail() {
        var run = phases()
        run[3] = Phase(
            name: "detail", moving: true, frames: 240, black: [5012, 5040], partial: 1, pools: [Self.trimmed])
        let mems = [0, 100, 200, 300, 400, 500].map { Self.mem(cbs: $0) }
        var moved = mems
        moved[4] = Self.mem(cbs: 400, moved: 2)
        moved[5] = Self.mem(cbs: 500, moved: 2)
        let reads = words(Self.stale, mems: moved)
        let outcome = GlassSidebarVerdict.outcome(run, reads: reads, scene: Metrics(), pngs: [], stopped: nil)
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertEqual(outcome.metrics["detail"], .text("240/2/1"))
        XCTAssertEqual(outcome.metrics["detail_first"], .integer(5012))
        XCTAssertEqual(outcome.metrics["detail_mem"], .text("100/0/0/2/0"))
        XCTAssertEqual(
            outcome.detail,
            "detail: 2 of 240 frames show the sidebar black; detail: glass-mem 100/0/0/2/0; "
                + "1 frames show the sidebar partly black")
    }

    /// ui.glass_settings judges its glass pane, phases with no idle between them
    func testTheReasonNamesWhatTheBandShows() {
        let run = [
            Phase(name: "scroll", moving: true, frames: 180),
            Phase(name: "appear", moving: true, frames: 180, black: [812], partial: 3),
        ]
        let reads = [0, 100, 200].map { GlassMemWords(mem: Self.mem(cbs: $0), pool: Self.busy) }
        let outcome = GlassSidebarVerdict.outcome(
            run, reads: reads, scene: Metrics(), pngs: [], stopped: nil, subject: "glass pane")
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertEqual(outcome.metrics["reclaim"], .text("-"))
        XCTAssertEqual(
            outcome.detail,
            "appear: 1 of 180 frames show the glass pane black; 3 frames show the glass pane partly black")
    }

    func testNoFrameIsASkipAndAStoppedReadBackNeverPasses() {
        let empty = phases().map { Phase(name: $0.name, moving: $0.moving) }
        let release = Array(repeating: GlassMemWords(mem: 0, pool: 0), count: 6)
        let none = GlassSidebarVerdict.outcome(empty, reads: release, scene: Metrics(), pngs: [], stopped: nil)
        XCTAssertEqual(none.verdict, .skip)
        let stopped = GlassSidebarVerdict.outcome(
            Array(phases().prefix(2)), reads: Array(release.prefix(3)), scene: Metrics(), pngs: [],
            stopped: "sidebar:rearm")
        XCTAssertEqual(stopped.verdict, .skip)
    }

    func testTheWorstLineFitsWhatOsLogKeeps() {
        let worst = phases().map {
            Phase(
                name: $0.name, moving: $0.moving, frames: 999, black: Array(repeating: UInt64(UInt32.max), count: 999),
                partial: 999, pools: [UInt64.max])
        }
        let full = Self.mem(cbs: 0xf_ffff, changed: 4094, nonFinite: 4094, moved: 4094, zero: 254)
        let reads = (0..<6).map {
            GlassMemWords(
                mem: $0 == 0 ? 0 : full,
                pool: $0 % 2 == 0
                    ? UInt64.max
                    : Self.pool(
                        mb: 4095, chunks: 255, backings: 255, backingMB: 4095, compressed: 255, made: 65535))
        }
        var scene = Metrics()
        scene.set("under", true)
        scene.set("region", "1234,1234,1234,1234")
        scene.set("means", "0.00/0.00")
        let pngs = Array(repeating: "detail-\(UInt32.max)-before.png", count: 9)
        let outcome = GlassSidebarVerdict.outcome(
            worst, reads: reads, scene: scene, pngs: pngs, stopped: "both:query_0x1")
        let line = LogLine.format(id: "ui.glass_sidebar", outcome: outcome, milliseconds: 123_456.7)
        XCTAssertLessThanOrEqual(line.utf8.count, 1000, line)
    }
}
