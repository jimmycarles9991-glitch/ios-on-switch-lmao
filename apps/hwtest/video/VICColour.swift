import CoreVideo
import Foundation
import IOSurface

/// The scaler's colour stages through a direct client (ScalerClient), surfaces tagged with IOSurfaceSetValue as a
/// direct client tags them (gpu.x1.vic_gamut, gpu.x1.vic_tags)
public enum VICColour {
    public static let unsupported = Int32(bitPattern: 0xe000_02c7)
    static let transfer = "IOSurfaceTransferFunction", primaries = "IOSurfaceColorPrimaries"
    static let matrix = "IOSurfaceYCbCrMatrix"

    static func tag(_ buffer: CVPixelBuffer, _ values: [String: String]) {
        guard let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue() else { return }
        for (key, value) in values { IOSurfaceSetValue(surface, key as CFString, value as CFString) }
    }

    static func bgra(_ width: Int, _ height: Int, _ values: [String: String],
                     pixels: [UInt32] = []) throws -> CVPixelBuffer {
        let buffer = try ScalerClient.buffer(width, height, kCVPixelFormatType_32BGRA)
        ScalerClient.edit(buffer) { base, pitch in
            for y in 0 ..< height {
                for x in 0 ..< width {
                    let i = y * width + x, p = i < pixels.count ? pixels[i] : 0x5a5a5a
                    let at = y * pitch + 4 * x
                    (base[at], base[at + 1], base[at + 2], base[at + 3]) =
                        (UInt8(p & 0xff), UInt8(p >> 8 & 0xff), UInt8(p >> 16 & 0xff), pixels.isEmpty ? 0x5a : 255)
                }
            }
        }
        tag(buffer, values)
        return buffer
    }

    /// Pixel i of a BGRA buffer as 0xAARRGGBB
    static func pixels(_ buffer: CVPixelBuffer) -> [UInt32] {
        let width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer)
        var out: [UInt32] = []
        ScalerClient.edit(buffer) { base, pitch in
            for y in 0 ..< height {
                for x in 0 ..< width {
                    let at = y * pitch + 4 * x
                    out
                        .append(UInt32(base[at + 3]) << 24 | UInt32(base[at + 2]) << 16 | UInt32(base[at + 1]) << 8 |
                            UInt32(base[at]))
                }
            }
        }
        return out
    }

    static func worst(_ a: UInt32, _ b: UInt32) -> Int {
        (0 ..< 3).map { abs(Int(a >> (8 * $0) & 0xff) - Int(b >> (8 * $0) & 0xff)) }.max()!
    }

    public struct Gamut: Sendable {
        public var status: Int32 = 0
        public var worst = 0, alpha = 255
        /// The three worst samples: input, written, reference
        public var worstSamples = "-"
    }

    /// `pair`'s 125 inputs (the first pixels) from `from` primaries into `to`, both sRGB-encoded 64x64 BGRA (an M4
    /// Max's scaler refuses 64x2 ones, kIOReturnBadArgument)
    public static func gamut(_ scaler: ScalerClient, _ pair: VICColourReference.Pair,
                             from: String? = nil) throws -> Gamut {
        let source = try bgra(64, 64, [transfer: srgb, primaries: from ?? pair.from], pixels: pair.inputs)
        let destination = try bgra(64, 64, [transfer: srgb, primaries: pair.to])
        var g = Gamut(status: scaler.transform(source, destination))
        guard g.status == 0 else { return g }
        let got = pixels(destination)
        let errors = pair.results.indices.map { (worst(got[$0], pair.results[$0]), $0) }.sorted { $0.0 > $1.0 }
        g.worst = errors.first?.0 ?? 0
        g.alpha = pair.results.indices.map { Int(got[$0] >> 24) }.min() ?? 0
        let hex = { (v: UInt32) in String(v & 0xffffff, radix: 16) }
        g.worstSamples = errors.prefix(3)
            .map { "\(hex(pair.inputs[$0.1]))>\(hex(got[$0.1]))/\(hex(pair.results[$0.1]))" }
            .joined(separator: ",")
        return g
    }

    /// A transfer the scaler refuses: its IOReturn, and whether the destination changed
    public static func refused(
        _ scaler: ScalerClient, from: [String: String], to: [String: String], size: Int = 64
    ) throws -> (Int32, Bool) {
        let source = try bgra(64, 64, from, pixels: (0 ..< 4096).map { UInt32($0 & 127) * 0x020301 })
        let destination = try bgra(size, size, to), before = pixels(destination)
        let status = scaler.transform(source, destination)
        return (status, pixels(destination) != before)
    }

    /// A byte copy between BGRA surfaces of `from` and `to` tags: its IOReturn and the worst channel off the source's
    public static func copy(_ scaler: ScalerClient, from: [String: String],
                            to: [String: String]) throws -> (Int32, Int) {
        let inputs = (0 ..< 4096).map { UInt32(truncatingIfNeeded: ($0 &* 2_654_435_761) >> 8) & 0xffffff }
        let source = try bgra(64, 64, from, pixels: inputs), destination = try bgra(64, 64, to)
        let status = scaler.transform(source, destination)
        let got = pixels(destination)
        return (status, inputs.indices.map { worst(got[$0], inputs[$0]) }.max()!)
    }

    /// A 64x64 '420v' of luma 63 and every chroma pair (102, 240), with `values`, into a BGRA: IOReturn, R and G at (8, 8)
    public static func ycbcr(_ scaler: ScalerClient, _ values: [String: String]) throws -> (Int32, Int, Int) {
        let source = try ScalerClient.buffer(64, 64, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        ScalerClient
            .edit(source, plane: 0) { base, pitch in
                for y in 0 ..< 64 { for x in 0 ..< 64 { base[y * pitch + x] = 63 } } }
        ScalerClient.edit(source, plane: 1) { base, pitch in
            for y in 0 ..<
                32 { for x in 0 ..< 32 { (base[y * pitch + 2 * x], base[y * pitch + 2 * x + 1]) = (102, 240) } }
        }
        tag(source, values)
        let destination = try ScalerClient.buffer(64, 64, kCVPixelFormatType_32BGRA)
        let status = scaler.transform(source, destination)
        let p = pixels(destination)[8 * 64 + 8]
        return (status, Int(p >> 16 & 0xff), Int(p >> 8 & 0xff))
    }

    public static let srgb = "IEC_sRGB"
    public static let tf709 = "ITU_R_709_2"
    public static func tags(_ primaries: String, _ function: String = srgb) -> [String: String] {
        [Self.primaries: primaries, transfer: function]
    }
    public static func matrixTag(_ value: String) -> [String: String] { [matrix: value] }
}
