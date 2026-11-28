import CoreVideo
import Foundation
import HWTestCore
import HWTestKit
import HWTestVideo

/// The luma VIC writes into an L008 for a direct IOSurfaceAccelerator client (video/ScalerClient.swift), 64x64, each
/// one synchronous TransformSurface: a, a 'BGRA' red left (255, 0, 0) and green right (0, 255, 0) into an untagged
/// L008: BT.601's, luma_red_601 76, luma_green_601 150, exact (an untagged destination is BT.601 to Apple's scaler: an
/// M4 Max's IOSurfaceAccelerator encodes BGRA into an untagged '420v' as BT.601 at every size from 640x480 to
/// 3840x2160; VideoToolbox's 54 and 182 come from its tagging the destination with the untagged BGRA's BT.709 before it
/// converts; the untagged-scaler probe, macOS 27.2.1); b, the same into an L008 tagged ITU_R_709_2: BT.709's,
/// luma_red_709 54, luma_green_709 182, exact (the VIC luma model, equal to VideoToolbox's per tag on the M4 Max); c, a
/// '420v' of luma 235 above and 126 below, every Cb and Cr 16, into an L008: y_white 255, y_mid 128 (no chroma in it);
/// d, an L008 ramp into a 'BGRA': gray_err 0 (max |R - L|, |G - L|, |B - L|), gray_alpha 255. c within one code. This
/// Mac's scaler refuses an L008 either side (0xe00002c2), so c and d are the standards' arithmetic, which
/// VideoToolbox's own conversions on an M4 Max give. status_<case>: each IOReturn (0). Reported, not judged: d's BGRA
/// at L 0, 64, 128 and 255 (gray_at_<L>, B/G/R/A), and every red and every green code alone into an L008 by each
/// standard against round(K code), K its luma coefficient (ramp_<standard>_<primary>: the codes one over, one under,
/// the first over), which tells a rounding rule from a coefficient.
struct VICLumaTest: HWTest {
    let id = "gpu.x1.vic_luma"
    let name = "VIC writes the luma of the destination's standard into an L008, and gray back to BGRA"
    let subsystem = Subsystem.images
    var timeout: TimeInterval { 30 }

    static let size = 64
    static let bgra = kCVPixelFormatType_32BGRA, l008 = kCVPixelFormatType_OneComponent8

    func run(_ context: TestContext) async throws -> Outcome {
        let scaler: ScalerClient
        switch ScalerClient.open() {
        case .failure(let failure): return .fail(failure.description)
        case .success(let client): scaler = client
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let near = { (key: String, got: Int, want: Int) in
            metrics.set(key, got)
            checks.append(("\(key)=\(got) not \(want)", abs(got - want) <= 1))
        }
        do {
            for (standard, tags) in [
                ("601", [String: String]()),
                ("709", [kCVImageBufferYCbCrMatrixKey as String: kCVImageBufferYCbCrMatrix_ITU_R_709_2 as String]),
            ] {
                let (status, red, green) = try Self.halves(scaler, tags)
                metrics.set("status_" + standard, Int(status))
                checks.append(("status_\(standard)", status == 0))
                let (wantRed, wantGreen) = standard == "601" ? (76, 150) : (54, 182)
                metrics.set("luma_red_" + standard, red)
                metrics.set("luma_green_" + standard, green)
                checks.append(("luma_red_\(standard)=\(red) not \(wantRed)", red == wantRed))
                checks.append(("luma_green_\(standard)=\(green) not \(wantGreen)", green == wantGreen))
            }
            let (status, white, mid) = try Self.lumaOnly(scaler)
            metrics.set("status_420v", Int(status))
            checks.append(("status_420v", status == 0))
            near("y_white", white, 255)
            near("y_mid", mid, 128)
            let (grayStatus, error, alpha, at) = try Self.grayRamp(scaler)
            metrics.set("status_gray", Int(grayStatus))
            checks.append(("status_gray", grayStatus == 0))
            metrics.set("gray_err", error)
            metrics.set("gray_alpha", alpha)
            checks.append(("gray_err=\(error)", error == 0))
            checks.append(("gray_alpha=\(alpha)", alpha == 255))
            for (luma, bytes) in at { metrics.set("gray_at_\(luma)", bytes) }
            for (key, ramp) in (try? Self.primaryRamps(scaler)) ?? [("ramps", "unmade")] { metrics.set(key, ramp) }
        } catch {
            checks.append(("\(error)", false))
        }
        return .judged(checks, metrics)
    }

    static func byte(_ buffer: CVPixelBuffer, _ x: Int, _ y: Int) -> Int {
        var value = 0
        ScalerClient.edit(buffer) { base, pitch in value = Int(base[y * pitch + x]) }
        return value
    }

    static func halves(_ scaler: ScalerClient, _ tags: [String: String]) throws -> (Int32, Int, Int) {
        let source = try ScalerClient.buffer(size, size, bgra), destination = try ScalerClient.buffer(
            size,
            size,
            l008,
            tags: tags
        )
        ScalerClient.edit(source) { base, pitch in
            for y in 0 ..< size {
                for x in 0 ..< size {
                    let red = x < size / 2
                    (base[y * pitch + 4 * x], base[y * pitch + 4 * x + 1]) = (0, red ? 0 : 255)
                    (base[y * pitch + 4 * x + 2], base[y * pitch + 4 * x + 3]) = (red ? 255 : 0, 255)
                }
            }
        }
        let status = scaler.transform(source, destination)
        return (status, byte(destination, 8, 8), byte(destination, 40, 8))
    }

    static func lumaOnly(_ scaler: ScalerClient) throws -> (Int32, Int, Int) {
        let source = try ScalerClient.buffer(size, size, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        let destination = try ScalerClient.buffer(size, size, l008)
        ScalerClient.edit(source, plane: 0) { base, pitch in
            for y in 0 ..< size { for x in 0 ..< size { base[y * pitch + x] = y < size / 2 ? 235 : 126 } }
        }
        ScalerClient.edit(source, plane: 1) { base, pitch in
            for y in 0 ..< (size / 2) { for x in 0 ..< size { base[y * pitch + x] = 16 } }
        }
        let status = scaler.transform(source, destination)
        return (status, byte(destination, 8, 8), byte(destination, 8, 40))
    }

    static func grayRamp(_ scaler: ScalerClient) throws -> (Int32, Int, Int, [(Int, String)]) {
        let source = try ScalerClient.buffer(size, size, l008), destination = try ScalerClient.buffer(size, size, bgra)
        let ramp = { (x: Int, y: Int) in UInt8(x * 4 + y / 16) }
        ScalerClient.edit(source) { base, pitch in
            for y in 0 ..< size { for x in 0 ..< size { base[y * pitch + x] = ramp(x, y) } }
        }
        let status = scaler.transform(source, destination)
        var (worst, alpha) = (0, 255)
        var samples: [(Int, String)] = []
        ScalerClient.edit(destination) { base, pitch in
            for y in 0 ..< size {
                for x in 0 ..< size {
                    let at = y * pitch + 4 * x, luma = Int(ramp(x, y))
                    for c in 0 ..< 3 { worst = max(worst, abs(Int(base[at + c]) - luma)) }
                    alpha = min(alpha, Int(base[at + 3]))
                }
            }
            for (luma, x, y) in [(0, 0, 0), (64, 16, 0), (128, 32, 0), (255, 63, 63)] {
                samples.append((luma, (0 ..< 4).map { String(base[y * pitch + 4 * x + $0]) }.joined(separator: "/")))
            }
        }
        return (status, worst, alpha, samples)
    }

    /// Every code of red, then of green, alone into an L008 by BT.601 (untagged) and by BT.709: per standard and
    /// primary, the codes VIC writes one over round(K code), one under, and the first over (-1: none). Code c sits at
    /// (c / 4, 16 (c % 4)), as d's ramp has it.
    static func primaryRamps(_ scaler: ScalerClient) throws -> [(String, String)] {
        let bt709 = [kCVImageBufferYCbCrMatrixKey as String: kCVImageBufferYCbCrMatrix_ITU_R_709_2 as String]
        let standards: [(String, [String: String], Double, Double)] = [
            ("601", [:], 0.299, 0.587), ("709", bt709, 0.2126, 0.7152),
        ]
        var out: [(String, String)] = []
        for (standard, tags, kr, kg) in standards {
            for (primary, channel, k) in [("red", 2, kr), ("green", 1, kg)] {
                let source = try ScalerClient.buffer(size, size, bgra)
                let destination = try ScalerClient.buffer(size, size, l008, tags: tags)
                ScalerClient.edit(source) { base, pitch in
                    for y in 0 ..< size {
                        for x in 0 ..< size {
                            for c in 0 ..< 3 { base[y * pitch + 4 * x + c] = 0 }
                            base[y * pitch + 4 * x + 3] = 255
                            base[y * pitch + 4 * x + channel] = UInt8(x * 4 + y / 16)
                        }
                    }
                }
                let status = scaler.transform(source, destination)
                var (over, under, first) = (0, 0, -1)
                ScalerClient.edit(destination) { base, pitch in
                    for code in 0 ..< 256 {
                        let got = Int(base[16 * (code % 4) * pitch + code / 4])
                        let want = Int((k * Double(code)).rounded())
                        over += got == want + 1 ? 1 : 0
                        under += got == want - 1 ? 1 : 0
                        if got > want && first < 0 { first = code }
                    }
                }
                out.append(("ramp_\(standard)_\(primary)", status == 0 ? "\(over)/\(under)/\(first)" : "\(status)"))
            }
        }
        return out
    }
}
