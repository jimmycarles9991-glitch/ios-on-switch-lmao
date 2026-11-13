import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import UIKit

/// Video with alpha as AVPlayer hands it to the render server: VideoToolbox's MuxedAlphaDecoder merges HEVC-with-alpha
/// into a tri-planar 'v0a8' surface (BT.709 video-range Y and CbCr, full-range alpha in plane 2). Two such surfaces are
/// layer contents over a solid background, one marked straight alpha and one premultiplied
/// (kCVImageBufferAlphaChannelModeKey), each a grid of five colours by five alphas (255, 192, 128, 64, 0). The render
/// server composites them (drawHierarchy(afterScreenUpdates:)); each patch's mean must be within 6 levels of the
/// composite its alpha mode names (VideoAlpha), blended either on sRGB-encoded values or in linear light (<layer>_space).
/// Per layer: y_err (opaque greys: plane 0 and its range), cbcr_err (opaque colours: plane 1, matrix and range),
/// alpha_err (translucent patches: plane 2 and the blend) and clear_err (alpha 0), in levels, and fit, the
/// VideoAlpha.Model (mode/alpha source/range/space) that explains every patch best, which names the mistake on a
/// failure. bg_err checks the snapshot's background itself.
struct VideoAlphaTest: HWTest {
    let id = "ui.video_alpha"
    let name = "Video with alpha ('v0a8') composited over a background"
    let subsystem = Subsystem.compositor

    static let patch = 32
    static let inset = 8
    static let tolerance: Float = 6.0 / 255
    static let background = VideoAlpha.background
    static let layers: [(label: String, mode: VideoAlpha.Mode)] = [("straight", .straight), ("premul", .premultiplied)]
    static var side: Int { patch * VideoAlpha.colours.count }

    /// A patch's codes and its mean in the composited picture.
    struct Sample {
        let row: Int
        let codes: VideoAlpha.Codes
        let measured: SIMD3<Float>
    }

    struct Picture {
        let buffer: CVPixelBuffer
        let surface: IOSurfaceRef
        /// By colour, then by alpha.
        let codes: [[VideoAlpha.Codes]]
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let pictures = try Self.layers.map { try Self.picture($0.mode) }
        nonisolated(unsafe) let surfaces = pictures.map(\.surface)
        let (composited, origins, backdrop) = await UISceneTests.staged(context) {
            stage, screen -> (UIView, [CGRect]) in
            let root = UIView(frame: Snapshots.aligned(screen))
            let b = Self.background
            root.backgroundColor = UIColor(red: CGFloat(b.x), green: CGFloat(b.y), blue: CGFloat(b.z), alpha: 1)
            let size = CGFloat(Self.side) / UIScreen.main.scale
            let frames = surfaces.indices.map { index in
                Snapshots.aligned(CGRect(x: 40 + CGFloat(index) * (size + 40), y: 80, width: size, height: size))
            }
            for (surface, frame) in zip(surfaces, frames) {
                let layer = CALayer()
                layer.frame = frame
                layer.contents = surface
                root.layer.addSublayer(layer)
            }
            stage.addSubview(root)
            return (root, frames)
        } _: { staged -> (Raster, [(Int, Int)], CGRect) in
            let (root, frames) = staged
            let scale = root.window?.screen.scale ?? UIScreen.main.scale
            let origins = frames.map { (Int(($0.minX * scale).rounded()), Int(($0.minY * scale).rounded())) }
            let below = frames[0].maxY + 40
            let backdrop = CGRect(x: frames[0].minX * scale, y: below * scale, width: 64, height: 64)
            return (Snapshots.composited(root), origins, backdrop)
        }
        withExtendedLifetime(pictures) {}

        var metrics = Metrics()
        var reasons: [String] = []
        let backdropMean = Self.mean(composited, Int(backdrop.minX), Int(backdrop.minY), 64, 64)
        let bgError = Self.error(backdropMean, Self.background)
        metrics.set("bg_err", Self.levels(bgError))
        if bgError > Self.tolerance { reasons.append("the background itself is off by \(Self.levels(bgError)) levels") }
        for (index, layer) in Self.layers.enumerated() {
            let samples = Self.samples(composited, origin: origins[index], codes: pictures[index].codes)
            reasons += Self.judge(samples, label: layer.label, mode: layer.mode, &metrics)
        }
        if !reasons.isEmpty {
            let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try? composited.png()?.write(to: directory.appendingPathComponent("composited.png"))
            return .fail(LogLine.fitted(reasons), metrics)
        }
        return .pass(metrics)
    }

    /// One layer's verdict: its errors by plane, the space its blend matched in, and its best-fitting model.
    static func judge(_ samples: [Sample], label: String, mode: VideoAlpha.Mode, _ metrics: inout Metrics) -> [String] {
        let space = VideoAlpha.Space.allCases.min { a, b in
            worst(samples, VideoAlpha.Model(mode, .plane2, .video, a))
                < worst(samples, VideoAlpha.Model(mode, .plane2, .video, b))
        }!
        let expected = VideoAlpha.Model(mode, .plane2, .video, space)
        let groups: [(String, (Sample) -> Bool)] = [
            ("y_err", { $0.codes.a == 255 && $0.row < VideoAlpha.greys }),
            ("cbcr_err", { $0.codes.a == 255 && $0.row >= VideoAlpha.greys }),
            ("alpha_err", { $0.codes.a != 255 && $0.codes.a != 0 }),
            ("clear_err", { $0.codes.a == 0 }),
        ]
        metrics.set(label + "_space", space.rawValue)
        var wrong: [String] = []
        for (key, member) in groups {
            let error = worst(samples.filter(member), expected)
            metrics.set("\(label)_\(key)", levels(error))
            if error > tolerance { wrong.append("\(key)=\(levels(error))") }
        }
        let fit = VideoAlpha.Model.all.map { ($0, squared(samples, $0)) }.min { $0.1 < $1.1 }!.0
        metrics.set(label + "_fit", fit.name)
        metrics.set(label + "_fit_err", levels(worst(samples, fit)))
        guard !wrong.isEmpty else { return [] }
        return ["\(label): \(wrong.joined(separator: " ")) against \(expected.name), fits \(fit.name)"]
    }

    /// A 'v0a8' surface holding the grid, its colours premultiplied for that alpha mode, marked as the decoder marks
    /// its output (BT.709 matrix and primaries; sRGB transfer, so no colour matching moves the reference).
    static func picture(_ mode: VideoAlpha.Mode) throws -> Picture {
        let attributes: [String: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            kCVPixelBufferMetalCompatibilityKey as String: true,
        ]
        var made: CVPixelBuffer?
        guard
            CVPixelBufferCreate(
                nil, side, side, kCVPixelFormatType_420YpCbCr8VideoRange_8A_TriPlanar, attributes as CFDictionary,
                &made) == kCVReturnSuccess, let buffer = made, CVPixelBufferGetPlaneCount(buffer) == 3,
            let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { throw TestFailure("CoreVideo made no IOSurface-backed \(side)x\(side) 'v0a8' buffer") }
        let codes = VideoAlpha.grid(mode)
        CVPixelBufferLockBaseAddress(buffer, [])
        let planes = (0..<3).map { index in
            (
                CVPixelBufferGetBaseAddressOfPlane(buffer, index)!.assumingMemoryBound(to: UInt8.self),
                CVPixelBufferGetBytesPerRowOfPlane(buffer, index)
            )
        }
        for y in 0..<side {
            for x in 0..<side {
                let code = codes[y / patch][x / patch]
                planes[0].0[y * planes[0].1 + x] = code.y
                planes[2].0[y * planes[2].1 + x] = code.a
            }
        }
        for y in 0..<(side / 2) {
            for x in 0..<(side / 2) {
                let code = codes[2 * y / patch][2 * x / patch]
                planes[1].0[y * planes[1].1 + 2 * x] = code.cb
                planes[1].0[y * planes[1].1 + 2 * x + 1] = code.cr
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        let marks: [CFString: CFString] = [
            kCVImageBufferYCbCrMatrixKey: kCVImageBufferYCbCrMatrix_ITU_R_709_2,
            kCVImageBufferColorPrimariesKey: kCVImageBufferColorPrimaries_ITU_R_709_2,
            kCVImageBufferTransferFunctionKey: kCVImageBufferTransferFunction_sRGB,
            kCVImageBufferAlphaChannelModeKey: mode == .premultiplied
                ? kCVImageBufferAlphaChannelMode_PremultipliedAlpha : kCVImageBufferAlphaChannelMode_StraightAlpha,
        ]
        CVBufferSetAttachments(buffer, marks as CFDictionary, .shouldPropagate)
        for (key, value) in marks { IOSurfaceSetValue(surface, key, value) }
        return Picture(buffer: buffer, surface: surface, codes: codes)
    }

    static func samples(_ picture: Raster, origin: (Int, Int), codes: [[VideoAlpha.Codes]]) -> [Sample] {
        let size = patch - 2 * inset
        return codes.enumerated().flatMap { row, line in
            line.enumerated().map { column, code in
                let x = origin.0 + column * patch + inset
                let y = origin.1 + row * patch + inset
                return Sample(row: row, codes: code, measured: mean(picture, x, y, size, size))
            }
        }
    }

    static func worst(_ samples: [Sample], _ model: VideoAlpha.Model) -> Float {
        samples.map { error($0.measured, VideoAlpha.composite($0.codes, over: background, model)) }.max() ?? 0
    }

    static func squared(_ samples: [Sample], _ model: VideoAlpha.Model) -> Float {
        samples.reduce(0) { sum, sample in
            let d = sample.measured - VideoAlpha.composite(sample.codes, over: background, model)
            return sum + d.x * d.x + d.y * d.y + d.z * d.z
        }
    }

    static func error(_ a: SIMD3<Float>, _ b: SIMD3<Float>) -> Float {
        let d = a - b
        return max(abs(d.x), abs(d.y), abs(d.z))
    }

    static func levels(_ value: Float) -> Int { Int((value * 255).rounded()) }

    static func mean(_ picture: Raster, _ x: Int, _ y: Int, _ width: Int, _ height: Int) -> SIMD3<Float> {
        guard x >= 0, y >= 0, x + width <= picture.width, y + height <= picture.height else { return SIMD3(-1, -1, -1) }
        var sum = SIMD3<Float>(repeating: 0)
        for py in y..<(y + height) {
            for px in x..<(x + width) {
                let p = picture.pixel(px, py)
                sum += SIMD3(p[0], p[1], p[2])
            }
        }
        return sum / Float(width * height)
    }
}
