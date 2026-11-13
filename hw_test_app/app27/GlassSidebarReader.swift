import Foundation
import HWTestCore
import UIKit
import os

/// ui.glass_sidebar's (and ui.glass_settings') read-back of one moving phase, on a thread of its own as ui.glass_live's
/// LiveReader: each presented frame read in place, the band judged in both places the turned buffer may put it (and, with a live
/// kernel, the frame before as it was scanned at the end of its time on screen), the read-back armed again. The caller
/// arms the read-back before the phase's first commit, so the first frame after an idle phase is the first one read.
final class SidebarReader {
    private static let logger = Logger(subsystem: "com.brunostjohn.switch.hwtest", category: "glass_sidebar")

    let session: ScanoutSession
    let test: String
    let phase: String
    let band: SidebarBand
    let directory: URL
    let uiPixels: (width: Int, height: Int)
    private(set) var frames: [SidebarFrame] = []
    private(set) var saved: [String] = []
    private(set) var failure: String?
    private var kept: (sequence: UInt64, image: Data)?
    private var savedWrong = false
    private var saveNext = false

    @MainActor
    init(session: ScanoutSession, test: String, phase: String, band: SidebarBand, directory: URL) {
        self.session = session
        self.test = test
        self.phase = phase
        self.band = band
        self.directory = directory
        let size = UIScreen.main.nativeBounds.size
        let (w, h) = (Int(size.width), Int(size.height))
        uiPixels = (max(w, h), min(w, h))
    }

    /// The frames seen black, at their present or at the end of their time on screen (a live kernel can see one
    /// frame both ways), each once in the order seen.
    var black: [UInt64] {
        var seen = Set<UInt64>()
        return frames.flatMap { $0.sightings("black") }.filter { seen.insert($0).inserted }
    }

    var partial: Int { Set(frames.flatMap { $0.sightings("partial") }).subtracting(black).count }
    var pools: [UInt64] { frames.map(\.pool) }

    func run(until deadline: Date) async {
        await withCheckedContinuation { done in
            DispatchQueue.global(qos: .userInitiated).async {
                self.loop(until: deadline)
                done.resume()
            }
        }
    }

    private func loop(until deadline: Date) {
        let start = Date()
        while Date() < deadline {
            switch session.next(timeout: 0.5) {
            case .none: continue
            case .failed(let why):
                failure = why
                return
            case .frame(let frame):
                judge(frame, ms: Date().timeIntervalSince(start) * 1000)
                guard session.rearm() else {
                    failure = "rearm"
                    return
                }
            }
        }
    }

    private func judge(_ frame: ScanoutSession.Frame, ms: Double) {
        let turned = frame.width == uiPixels.height && frame.height == uiPixels.width
        let image = LiveImage(base: frame.first, width: frame.width, height: frame.height, turned: turned)
        let a = image.rect(uiX: band.x, uiY: band.y)
        let b = image.rect(uiX: band.mirroredX, uiY: band.y)
        var end: (LiveImage.Glass, LiveImage.Glass)?
        if frame.previous != nil {
            let second = LiveImage(base: frame.second, width: frame.width, height: frame.height, turned: turned)
            end = (second.glass(a), second.glass(b))
        }
        let record = SidebarFrame(
            sequence: frame.sequence, ticks: frame.ticks, ms: ms, turned: turned, a: .init(image.glass(a)),
            b: .init(image.glass(b)), pool: GlassMemWords.pool(), previous: frame.previous,
            endA: end.map { .init($0.0) }, endB: end.map { .init($0.1) })
        frames.append(record)
        if record.verdict == "black" { log(frame.sequence, ticks: frame.ticks, at: "present", record.a, record.b) }
        if let endA = record.endA, let endB = record.endB, record.endVerdict == "black" {
            log(record.endSequence ?? 0, ticks: frame.ticks, at: "end", endA, endB)
        }
        save(frame, record, image)
    }

    /// The join key with the render server's per-command-buffer lines: the frame's offer sequence and the mach time it
    /// was read at, whole and as GlassProbe's 40 low bits.
    private func log(_ sequence: UInt64, ticks: UInt64, at: String, _ a: SidebarFrame.Band, _ b: SidebarFrame.Band) {
        let low = ticks & ((1 << 40) - 1)
        Self.logger.notice(
            """
            \(self.test, privacy: .public) black phase=\(self.phase, privacy: .public) seq=\(sequence, privacy: .public) \
            ticks=\(ticks, privacy: .public) ticks40=\(low, privacy: .public) at=\(at, privacy: .public) \
            a=\(a.black, privacy: .public) b=\(b.black, privacy: .public)
            """)
    }

    /// The phase's first black or partly black frame, the frames read before and after it (their sequences show any
    /// frames the read-back missed), as PNGs.
    private func save(_ frame: ScanoutSession.Frame, _ record: SidebarFrame, _ image: LiveImage) {
        if saveNext {
            write(image.copy(), frame, "\(frame.sequence)-after")
            saveNext = false
        }
        guard !savedWrong else { return }
        if record.wrong {
            if let kept { write(kept.image, frame, "\(kept.sequence)-before") }
            write(image.copy(), frame, "\(frame.sequence)-present")
            if let previous = frame.previous, record.endVerdict == "black" || record.endVerdict == "partial" {
                write(Data(bytes: frame.second, count: frame.width * frame.height * 4), frame, "\(previous)-endscan")
            }
            savedWrong = true
            saveNext = true
            kept = nil
            return
        }
        kept = (frame.sequence, image.copy())
    }

    private func write(_ bgra: Data, _ frame: ScanoutSession.Frame, _ stem: String) {
        let name = "\(phase)-\(stem).png"
        guard let png = LiveImage.png(bgra, width: frame.width, height: frame.height) else { return }
        try? png.write(to: directory.appendingPathComponent(name))
        saved.append(name)
    }
}

/// What ui.glass_sidebar saw of one presented frame. `a` is the band where the sidebar sits when the buffer runs
/// with the UI's x, `b` where it sits when it runs against it; the other one holds the detail's bright stripes. For
/// ui.glass_settings both are the pane's middle.
struct SidebarFrame: Codable {
    struct Band: Codable {
        let verdict: String
        let black: Double
        let mean: Double

        init(_ glass: LiveImage.Glass) {
            verdict = glass.verdict
            black = (glass.black * 1000).rounded() / 1000
            mean = (glass.mean * 1000).rounded() / 1000
        }
    }

    let sequence: UInt64
    /// mach_absolute_time when the read-back found the frame done.
    let ticks: UInt64
    let ms: Double
    let turned: Bool
    let a: Band
    let b: Band
    /// The glass-mem.pool word the render server last published (at its commits) when this frame was read.
    let pool: UInt64
    /// Live: the offer sequence (low 32 bits) of the frame before, and its bands at the end of its time on screen.
    let previous: UInt32?
    let endA: Band?
    let endB: Band?

    var verdict: String { Self.worst(a, b) }
    var endVerdict: String? { endA.flatMap { endA in endB.map { Self.worst(endA, $0) } } }
    var wrong: Bool { Self.wrong.contains(verdict) || endVerdict.map(Self.wrong.contains) == true }

    static let wrong: Set = ["black", "partial"]

    /// The frame before's whole offer sequence, from its low 32 bits and this frame's high ones.
    var endSequence: UInt64? {
        guard let previous else { return nil }
        let (low, high) = (UInt64(previous), sequence & 0xffff_ffff_0000_0000)
        return low > sequence & 0xffff_ffff ? (high &- 0x1_0000_0000) | low : high | low
    }

    /// The frames this read saw with `verdict`: this one at its present, the one before at the end of its time on screen.
    func sightings(_ verdict: String) -> [UInt64] {
        (self.verdict == verdict ? [sequence] : []) + (endVerdict == verdict ? endSequence.map { [$0] } ?? [] : [])
    }

    static func worst(_ a: Band, _ b: Band) -> String {
        let order = ["good", "dark", "partial", "black"]
        return order[max(order.firstIndex(of: a.verdict) ?? 0, order.firstIndex(of: b.verdict) ?? 0)]
    }
}
