import Foundation

/// The synthetic picture every test clip and image is made of, and which the app checks decoded output against.
///
/// The top two rows of 16 cells are a barcode: 16 bits of frame number, then a CRC-16 of the clip ID and that frame
/// number. Cells are flat black or white, big enough that any codec at any tested bitrate keeps them readable, so a
/// decoded frame's number and CRC are recovered exactly even though the codec is lossy. Below them is a diagonal
/// triangle-wave gradient that moves 8 steps a frame (motion for the inter predictor) over static chroma ramps; it is
/// checked against a tolerance to catch wrong planes, wrong ranges and garbage that still happens to keep the barcode.
///
/// Values are 8-bit video range (Y 16-235, Cb/Cr 16-240); 10-bit buffers carry the same values shifted left by 2.
public struct Pattern: Equatable, Sendable {
    public static let columns = 16
    public static let black = 16
    public static let white = 235

    public let width: Int
    public let height: Int
    public let clip: UInt16

    public init(width: Int, height: Int, clip: UInt16) {
        precondition(width >= 64 && height >= 64 && width % 2 == 0 && height % 2 == 0)
        self.width = width
        self.height = height
        self.clip = clip
    }

    public var cellWidth: Int { (width / Self.columns) & ~1 }
    public var cellHeight: Int { (height / 12) & ~1 }
    public var barcodeHeight: Int { cellHeight * 2 }

    public func code(frame: Int) -> UInt32 {
        let number = UInt16(truncatingIfNeeded: frame)
        let bytes = [UInt8(clip >> 8), UInt8(clip & 0xff), UInt8(number >> 8), UInt8(number & 0xff)]
        return UInt32(number) << 16 | UInt32(CRC16.ccitt(bytes))
    }

    func bit(_ code: UInt32, row: Int, column: Int) -> Bool {
        code >> UInt32(31 - (row * Self.columns + column)) & 1 == 1
    }

    func inBarcode(x: Int, y: Int) -> Bool { y < barcodeHeight && x < cellWidth * Self.columns }

    public func luma(x: Int, y: Int, frame: Int) -> Int {
        if inBarcode(x: x, y: y) {
            return bit(code(frame: frame), row: y / cellHeight, column: x / cellWidth) ? Self.white : Self.black
        }
        let phase = (x * 512 / width + y * 128 / height + frame * 8) % 512
        let triangle = phase < 256 ? phase : 511 - phase
        return 16 + triangle * 219 / 255
    }

    /// Chroma at a luma position; 4:2:0 writers take it at even coordinates.
    public func chroma(x: Int, y: Int) -> (cb: Int, cr: Int) {
        if inBarcode(x: x, y: y) { return (128, 128) }
        return (80 + y * 96 / height, 176 - x * 96 / width)
    }

    /// Full-range RGB of the same picture (BT.601), for the still images.
    public func rgb(x: Int, y: Int, frame: Int = 0) -> (r: UInt8, g: UInt8, b: UInt8) {
        let (cb, cr) = chroma(x: x, y: y)
        let luma = Double(luma(x: x, y: y, frame: frame) - 16) * 255 / 219
        let u = Double(cb - 128) * 255 / 224
        let v = Double(cr - 128) * 255 / 224
        func clamp(_ value: Double) -> UInt8 { UInt8(max(0, min(255, value.rounded()))) }
        return (
            clamp(luma + 1.402 * v),
            clamp(luma - 0.344136 * u - 0.714136 * v),
            clamp(luma + 1.772 * u)
        )
    }
}

public enum CRC16 {
    /// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF).
    public static func ccitt(_ bytes: [UInt8]) -> UInt16 {
        var crc: UInt16 = 0xFFFF
        for byte in bytes {
            crc ^= UInt16(byte) << 8
            for _ in 0..<8 { crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1 }
        }
        return crc
    }
}
