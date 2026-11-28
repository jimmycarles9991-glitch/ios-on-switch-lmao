import Foundation
import HWTestCore
import HWTestGPU
import UIKit

/// ui.glass_live's read-back loop, on a thread of its own so the display link keeps its frames: each presented frame
/// read in place and judged, compared with the frame read before it, then the read-back armed for the next present.
final class LiveReader {
    private struct Kept {
        let sequence: UInt64
        let whole: UInt64
        let glass: UInt64
        let strip: UInt64
        let image: Data
    }

    let session: ScanoutSession
    let directory: URL
    let variant: LiveVariant
    let uiPixels: (width: Int, height: Int)
    private(set) var frames: [LiveFrame] = []
    private(set) var saved: [String] = []
    private(set) var failure: String?
    private var kept: Kept?
    private var saveNext = false

    @MainActor
    init(session: ScanoutSession, directory: URL, variant: LiveVariant) {
        self.session = session
        self.directory = directory
        self.variant = variant
        let size = UIScreen.main.nativeBounds.size
        let (w, h) = (Int(size.width), Int(size.height))
        uiPixels = (max(w, h), min(w, h))
    }

    static func run(_ reader: LiveReader?, until deadline: Date) async {
        guard let reader else {
            let remaining = deadline.timeIntervalSinceNow
            if remaining > 0 { try? await Task.sleep(nanoseconds: UInt64(remaining * 1e9)) }
            return
        }
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                reader.loop(until: deadline)
                continuation.resume()
            }
        }
    }

    func loop(until deadline: Date) {
        let start = Date()
        _ = session.rearm()
        while Date() < deadline {
            switch session.next(timeout: 0.5) {
            case .none: break
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
        let glassRegion = image.rect(uiX: 0.35 ... 0.65, uiY: 0.42 ... 0.58)
        let stripRegion = image.rect(uiX: 0.05 ... 0.95, uiY: 0.05 ... 0.2)
        let glass = image.glass(glassRegion)
        let now = Kept(
            sequence: frame.sequence, whole: image.hash(), glass: image.hash(glassRegion),
            strip: image.hash(stripRegion), image: image.copy()
        )
        let consecutive = frame.sequence > 0 && kept?.sequence == frame.sequence - 1 ? kept : nil
        let second = LiveImage(base: frame.second, width: frame.width, height: frame.height, turned: turned)
        var end: LiveImage.Glass?
        var changed: Bool?
        var glassChanged: Bool?
        if let previous = frame.previous {
            end = second.glass(glassRegion)
            if let kept, UInt32(truncatingIfNeeded: kept.sequence) == previous {
                changed = second.hash() != kept.whole
                glassChanged = second.hash(glassRegion) != kept.glass
            }
        } else if frame.direct {
            changed = second.hash() != now.whole
            glassChanged = second.hash(glassRegion) != now.glass
        }
        frames.append(
            LiveFrame(
                sequence: frame.sequence, atPresent: glass.verdict, black: glass.black, mean: glass.mean,
                backdropChanged: consecutive.map { $0.strip != now.strip }, endVerdict: end?.verdict,
                endBlack: end?.black, changedOnScreen: changed, glassChangedOnScreen: glassChanged,
                ms: ms
            )
        )
        save(frame, now: now, consecutive: consecutive, glass: glass, end: end, changed: changed == true)
        kept = now
    }

    private func save(
        _ frame: ScanoutSession.Frame, now: Kept, consecutive: Kept?, glass: LiveImage.Glass, end: LiveImage.Glass?,
        changed: Bool
    ) {
        guard saved.count < 6 else { return }
        let wrong = glass.verdict == "black" || glass.verdict == "partial"
        if saveNext {
            write(now.image, frame, "\(frame.sequence)-after")
            saveNext = false
        }
        if wrong {
            if let consecutive { write(consecutive.image, frame, "\(consecutive.sequence)-before") }
            write(now.image, frame, "\(frame.sequence)-present")
            saveNext = true
        }
        if let previous = frame.previous, end?.verdict == "black" || end?.verdict == "partial" || changed {
            write(Data(bytes: frame.second, count: frame.width * frame.height * 4), frame, "\(previous)-endscan")
            if changed, let kept, UInt32(truncatingIfNeeded: kept.sequence) == previous {
                write(kept.image, frame, "\(kept.sequence)-present")
            }
        }
    }

    private func write(_ bgra: Data, _ frame: ScanoutSession.Frame, _ stem: String) {
        let name = "\(variant.rawValue)-\(stem).png"
        guard !saved.contains(name), let png = LiveImage.png(bgra, width: frame.width, height: frame.height) else {
            return
        }
        try? png.place(at: directory.appendingPathComponent(name))
        saved.append(name)
    }
}
