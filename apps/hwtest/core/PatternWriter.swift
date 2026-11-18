import Foundation

/// Writes the pattern into raw planes: 8-bit samples, or 16-bit samples holding a 10-bit value in their high bits
/// (P010's layout).
public enum PatternWriter {
    public struct Plane {
        public let base: UnsafeMutableRawPointer
        public let rowBytes: Int
        public let sampleBytes: Int

        public init(base: UnsafeMutableRawPointer, rowBytes: Int, sampleBytes: Int = 1) {
            precondition(sampleBytes == 1 || sampleBytes == 2)
            self.base = base
            self.rowBytes = rowBytes
            self.sampleBytes = sampleBytes
        }

        func store(_ value: Int, x: Int, y: Int) {
            let row = base + y * rowBytes
            if sampleBytes == 1 {
                row.storeBytes(of: UInt8(value), toByteOffset: x, as: UInt8.self)
            } else {
                row.storeBytes(of: UInt16(value << 8), toByteOffset: x * 2, as: UInt16.self)
            }
        }
    }

    public static func writeLuma(_ pattern: Pattern, frame: Int, into plane: Plane) {
        for y in 0..<pattern.height {
            if y < pattern.barcodeHeight {
                for x in 0..<pattern.width { plane.store(pattern.luma(x: x, y: y, frame: frame), x: x, y: y) }
                continue
            }
            let rowPhase = y * 128 / pattern.height + frame * 8
            for x in 0..<pattern.width {
                let phase = (x * 512 / pattern.width + rowPhase) % 512
                plane.store(16 + (phase < 256 ? phase : 511 - phase) * 219 / 255, x: x, y: y)
            }
        }
    }

    /// 4:2:0 CbCr pairs in one plane (NV12, P010).
    public static func writeChromaInterleaved(_ pattern: Pattern, into plane: Plane) {
        for cy in 0..<(pattern.height / 2) {
            for cx in 0..<(pattern.width / 2) {
                let (cb, cr) = pattern.chroma(x: cx * 2, y: cy * 2)
                plane.store(cb, x: cx * 2, y: cy)
                plane.store(cr, x: cx * 2 + 1, y: cy)
            }
        }
    }

    /// 4:2:0 Cb and Cr in planes of their own (I420).
    public static func writeChromaPlanar(_ pattern: Pattern, cb: Plane, cr: Plane) {
        for cy in 0..<(pattern.height / 2) {
            for cx in 0..<(pattern.width / 2) {
                let value = pattern.chroma(x: cx * 2, y: cy * 2)
                cb.store(value.cb, x: cx, y: cy)
                cr.store(value.cr, x: cx, y: cy)
            }
        }
    }

    /// Full-range RGB, `channels` bytes a pixel (3: RGB, 4: RGBX with X = 255).
    public static func writeRGB(_ pattern: Pattern, frame: Int = 0, into plane: Plane, channels: Int) {
        for y in 0..<pattern.height {
            let row = plane.base + y * plane.rowBytes
            for x in 0..<pattern.width {
                let (r, g, b) = pattern.rgb(x: x, y: y, frame: frame)
                let pixel = row + x * channels
                pixel.storeBytes(of: r, toByteOffset: 0, as: UInt8.self)
                pixel.storeBytes(of: g, toByteOffset: 1, as: UInt8.self)
                pixel.storeBytes(of: b, toByteOffset: 2, as: UInt8.self)
                if channels == 4 { pixel.storeBytes(of: 255, toByteOffset: 3, as: UInt8.self) }
            }
        }
    }
}
