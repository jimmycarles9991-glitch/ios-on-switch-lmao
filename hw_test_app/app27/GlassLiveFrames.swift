import Foundation
import ImageIO
import UniformTypeIdentifiers

/// One read-back frame as window A reads it (B8G8R8A8, `width` x 4 bytes a row), judged where ui.glass_live's scene
/// puts its regions. The scene is laid out symmetric about the screen's centre, so a region lands at the same place
/// whichever way the panel's portrait buffer turns the UI: only whether rows and columns swap matters.
struct LiveImage {
    let base: UnsafeRawPointer
    let width: Int
    let height: Int
    /// The UI's x runs down the buffer's rows (the panel's portrait buffer).
    let turned: Bool

    /// The buffer's rectangle for a region of the UI given as fractions of its width and height, centred.
    func rect(uiX: ClosedRange<Double>, uiY: ClosedRange<Double>) -> (x: Range<Int>, y: Range<Int>) {
        let (across, down) = turned ? (uiY, uiX) : (uiX, uiY)
        let x = Int(across.lowerBound * Double(width))..<Int(across.upperBound * Double(width))
        let y = Int(down.lowerBound * Double(height))..<Int(down.upperBound * Double(height))
        return (x, y)
    }

    struct Glass: Equatable {
        /// Pixels whose R, G and B are each 2 or less, of every pixel looked at.
        let black: Double
        /// Mean of R, G and B over 255.
        let mean: Double

        var verdict: String {
            if black >= 0.9 { return "black" }
            if black >= 0.02 { return "partial" }
            return mean < 0.15 ? "dark" : "good"
        }
    }

    /// The glass band's inside, away from its rim: every other pixel of every other row.
    func glass(_ region: (x: Range<Int>, y: Range<Int>)) -> Glass {
        var (black, total, sum) = (0, 0, 0)
        let bytes = base.assumingMemoryBound(to: UInt8.self)
        for y in stride(from: region.y.lowerBound, to: region.y.upperBound, by: 2) {
            let row = bytes + y * width * 4
            for x in stride(from: region.x.lowerBound, to: region.x.upperBound, by: 2) {
                let (b, g, r) = (Int(row[x * 4]), Int(row[x * 4 + 1]), Int(row[x * 4 + 2]))
                black += r <= 2 && g <= 2 && b <= 2 ? 1 : 0
                sum += r + g + b
                total += 1
            }
        }
        guard total > 0 else { return Glass(black: 0, mean: 0) }
        return Glass(black: Double(black) / Double(total), mean: Double(sum) / Double(total * 3 * 255))
    }

    /// FNV-1a over the region's rows (all of the buffer without one).
    func hash(_ region: (x: Range<Int>, y: Range<Int>)? = nil) -> UInt64 {
        let (xs, ys) = region ?? (0..<width, 0..<height)
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for y in ys {
            let row = base + (y * width + xs.lowerBound) * 4
            for i in 0..<(xs.count * 4 / 8) {
                hash = (hash ^ row.loadUnaligned(fromByteOffset: i * 8, as: UInt64.self)) &* 0x100_0000_01b3
            }
        }
        return hash
    }

    func copy() -> Data { Data(bytes: base, count: width * height * 4) }

    static func png(_ bgra: Data, width: Int, height: Int) -> Data? {
        let info = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue
        guard let provider = CGDataProvider(data: bgra as CFData),
            let image = CGImage(
                width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: info),
                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        let out = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(out, UTType.png.identifier as CFString, 1, nil)
        else { return nil }
        CGImageDestinationAddImage(destination, image, nil)
        return CGImageDestinationFinalize(destination) ? out as Data : nil
    }
}

/// What ui.glass_live saw of one presented frame.
struct LiveFrame: Codable {
    let sequence: UInt64
    /// The glass at its present: black, partial, dark or good, and its black fraction and mean.
    let atPresent: String
    let black: Double
    let mean: Double
    /// The backdrop strip changed since the frame before (only when that frame was read too).
    let backdropChanged: Bool?
    /// Live: the frame before as window A scanned it at the end of its time on screen.
    let endVerdict: String?
    let endBlack: Double?
    /// Live, and the frame before was read at its own present too: its pixels changed while it was shown (whole frame,
    /// glass band).
    let changedOnScreen: Bool?
    let glassChangedOnScreen: Bool?
    /// The render server's command buffers committed since the frame before was read: draws/encoders[Llevels].
    var buffers: [String]
    let ms: Double
}

/// The frames one variant read, summed.
struct LiveSummary {
    let name: String
    let frames: [LiveFrame]
    let recorded: Int
    let regenerated: Int
    let seconds: Double
    /// "<last>/<after>": the glass of the frame left on screen when the motion stopped, and of the first frame after a
    /// one-point change under the glass half a second later, which makes the render server capture the backdrop again
    var rest = "-"

    var black: [LiveFrame] { frames.filter { $0.atPresent == "black" || $0.endVerdict == "black" } }
    var partial: Int { frames.filter { $0.atPresent == "partial" || $0.endVerdict == "partial" }.count }
    var changedOnScreen: Int { frames.filter { $0.changedOnScreen == true }.count }

    /// "frames/black/partial/changed": the result line's field
    var field: String { "\(frames.count)/\(black.count)/\(partial)/\(changedOnScreen)" }

    /// Gaps between consecutive black frames' sequences, "1x3,2x5": every other frame black shows as gap 2.
    var gaps: String {
        let sequences = black.map(\.sequence)
        var counts: [UInt64: Int] = [:]
        for (a, b) in zip(sequences, sequences.dropFirst()) { counts[b - a, default: 0] += 1 }
        return counts.isEmpty ? "-" : counts.keys.sorted().map { "\($0)x\(counts[$0]!)" }.joined(separator: ",")
    }

    /// Black frames whose backdrop changed / did not / unknown.
    var coincidence: String {
        let changed = black.filter { $0.backdropChanged == true }.count
        let still = black.filter { $0.backdropChanged == false }.count
        return "\(changed)/\(still)/\(black.count - changed - still)"
    }

    var fps: Double { seconds > 0 ? Double(recorded) / seconds : 0 }

    struct Detail: Encodable {
        let name: String
        let recorded: Int
        let regenerated: Int
        let seconds: Double
        let frames: [LiveFrame]

        init(_ s: LiveSummary) {
            name = s.name
            recorded = s.recorded
            regenerated = s.regenerated
            seconds = s.seconds
            frames = s.frames
        }
    }
}
