import CoreVideo
import Foundation
import HWTestCore

/// IOSurface-backed CVPixelBuffers holding the pattern, and the checker's view of decoded ones.
public enum PatternPixels {
    public static func format(bitDepth: Int) -> OSType {
        bitDepth == 10
            ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    }

    public static func attributes(width: Int, height: Int, format: OSType) -> [String: Any] {
        [
            kCVPixelBufferPixelFormatTypeKey as String: format,
            kCVPixelBufferWidthKey as String: width,
            kCVPixelBufferHeightKey as String: height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            kCVPixelBufferMetalCompatibilityKey as String: true,
        ]
    }

    public static func pool(width: Int, height: Int, format: OSType) throws -> CVPixelBufferPool {
        var pool: CVPixelBufferPool?
        let status = CVPixelBufferPoolCreate(
            nil, nil, attributes(width: width, height: height, format: format) as CFDictionary, &pool)
        guard status == kCVReturnSuccess, let pool else { throw VideoError.status("CVPixelBufferPoolCreate", status) }
        return pool
    }

    public static func make(
        _ pattern: Pattern, frame: Int, bitDepth: Int, pool: CVPixelBufferPool?
    ) throws -> CVPixelBuffer {
        var buffer: CVPixelBuffer?
        let status: CVReturn
        if let pool {
            status = CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer)
        } else {
            let format = format(bitDepth: bitDepth)
            status = CVPixelBufferCreate(
                nil, pattern.width, pattern.height, format,
                attributes(width: pattern.width, height: pattern.height, format: format) as CFDictionary, &buffer)
        }
        guard status == kCVReturnSuccess, let buffer else { throw VideoError.status("CVPixelBufferCreate", status) }
        fill(buffer, pattern: pattern, frame: frame)
        return buffer
    }

    public static func fill(_ buffer: CVPixelBuffer, pattern: Pattern, frame: Int) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let sampleBytes = sampleBytes(of: buffer)
        PatternWriter.writeLuma(pattern, frame: frame, into: plane(buffer, 0, sampleBytes))
        PatternWriter.writeChromaInterleaved(pattern, into: plane(buffer, 1, sampleBytes))
    }

    static func sampleBytes(of buffer: CVPixelBuffer) -> Int {
        switch CVPixelBufferGetPixelFormatType(buffer) {
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: return 2
        default: return 1
        }
    }

    static func plane(_ buffer: CVPixelBuffer, _ index: Int, _ sampleBytes: Int) -> PatternWriter.Plane {
        PatternWriter.Plane(
            base: CVPixelBufferGetBaseAddressOfPlane(buffer, index)!,
            rowBytes: CVPixelBufferGetBytesPerRowOfPlane(buffer, index), sampleBytes: sampleBytes)
    }

    /// Checks a decoded 4:2:0 or 4:2:2 biplanar buffer (8-bit or P010) against the pattern.
    public static func check(_ buffer: CVPixelBuffer, pattern: Pattern, expected: Int) -> FrameCheck {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        let sampleBytes = sampleBytes(of: buffer)
        let luma = Reader(buffer: buffer, plane: 0, sampleBytes: sampleBytes)
        let chroma = Reader(buffer: buffer, plane: 1, sampleBytes: sampleBytes)
        let rowShift = CVPixelBufferGetHeightOfPlane(buffer, 1) == CVPixelBufferGetHeight(buffer) ? 0 : 1
        return PatternChecker(pattern).check(
            expected: expected,
            luma: { luma.mean($0.x, $0.y, $0.width, $0.height, step: 1, offset: 0) },
            chroma: { region in
                let (x, w) = (region.x / 2, max(1, region.width / 2))
                let (y, h) = (region.y >> rowShift, max(1, region.height >> rowShift))
                return (
                    chroma.mean(x * 2, y, w, h, step: 2, offset: 0), chroma.mean(x * 2, y, w, h, step: 2, offset: 1)
                )
            })
    }

    /// Checks a decoded 32-bit BGRA buffer (full-range RGB) against the pattern.
    public static func checkBGRA(_ buffer: CVPixelBuffer, pattern: Pattern, expected: Int) -> FrameCheck {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        let base = CVPixelBufferGetBaseAddress(buffer)!
        let rowBytes = CVPixelBufferGetBytesPerRow(buffer)
        return PatternChecker(pattern).checkRGB(expected: expected) { region in
            var (r, g, b) = (0.0, 0.0, 0.0)
            for y in region.y..<(region.y + region.height) {
                for x in region.x..<(region.x + region.width) {
                    let pixel = base + y * rowBytes + x * 4
                    b += Double(pixel.load(as: UInt8.self))
                    g += Double(pixel.load(fromByteOffset: 1, as: UInt8.self))
                    r += Double(pixel.load(fromByteOffset: 2, as: UInt8.self))
                }
            }
            let count = Double(region.width * region.height)
            return (r / count, g / count, b / count)
        }
    }

    struct Reader {
        let base: UnsafeRawPointer
        let rowBytes: Int
        let sampleBytes: Int

        init(buffer: CVPixelBuffer, plane: Int, sampleBytes: Int) {
            base = UnsafeRawPointer(CVPixelBufferGetBaseAddressOfPlane(buffer, plane)!)
            rowBytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            self.sampleBytes = sampleBytes
        }

        /// Mean of a `w`x`h` block in 8-bit units, every `step`th sample of a row starting at `x + offset`. Big blocks
        /// (barcode cells) are read on a 16x16 grid: their mean is the same and a 4K frame checks in microseconds,
        /// which keeps the check out of the decoder's measured frame time.
        func mean(_ x: Int, _ y: Int, _ w: Int, _ h: Int, step: Int, offset: Int) -> Double {
            var sum = 0.0
            var count = 0
            for row in stride(from: y, to: y + h, by: max(1, h / 16)) {
                let line = base + row * rowBytes
                for column in stride(from: 0, to: w, by: max(1, w / 16)) {
                    let index = x + column * step + offset
                    sum +=
                        sampleBytes == 1
                        ? Double(line.load(fromByteOffset: index, as: UInt8.self))
                        : Double(line.load(fromByteOffset: index * 2, as: UInt16.self)) / 256
                    count += 1
                }
            }
            return sum / Double(count)
        }
    }
}

public enum VideoError: Error, CustomStringConvertible {
    case status(String, OSStatus)
    case message(String)

    public var description: String {
        switch self {
        case .status(let call, let status): return "\(call) failed: \(status)"
        case .message(let text): return text
        }
    }
}
