import CoreGraphics
import Darwin
import Foundation
import HWTestCore
import IOSurface
import ImageIO
import UniformTypeIdentifiers

/// A JPEG decoded by NVJPG straight into an IOSurface of the caller's: selector 7 of AppleJPEGDriverUserClient, the
/// synchronous request CMPhoto sends (drivers/TegraNVJPG/TegraNVJPGRequest.h), from a surface holding the file into a
/// BGRA destination at full size.
enum JPEGSurfaceDecode {
    /// Within this of the picture a byte is right: JPEG at quality 1 and NVJPG's YCbCr to RGB stay inside it, a byte a
    /// layout mistake moves by a GOB's 16-byte run or more does not.
    static let tolerance = 10

    /// Triangle waves 64 pixels across (blue) and 128 rows down (green), red their sum: smooth enough for JPEG, steep
    /// enough that a texel moved by a GOB's run reads another value.
    static func byte(_ x: Int, _ y: Int, _ c: Int) -> UInt8 {
        func wave(_ v: Int, _ period: Int) -> Int {
            let at = v % period
            return (at < period / 2 ? at : period - at) * 510 / period
        }
        switch c {
        case 0: return UInt8(min(255, wave(x, 64)))
        case 1: return UInt8(min(255, wave(y, 128)))
        case 2: return UInt8(min(255, (wave(x, 64) + wave(y, 128)) / 2))
        default: return 255
        }
    }

    static func file(_ size: Int) -> Data? {
        var pixels = [UInt8](repeating: 0, count: size * size * 4)
        for y in 0..<size {
            for x in 0..<size {
                for c in 0..<4 { pixels[(y * size + x) * 4 + c] = byte(x, y, c) }
            }
        }
        let info = CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
        guard let provider = CGDataProvider(data: Data(pixels) as CFData),
            let image = CGImage(
                width: size, height: size, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: size * 4,
                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: info), provider: provider,
                decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, UTType.jpeg.identifier as CFString, 1, nil)
        else { return nil }
        CGImageDestinationAddImage(
            destination, image, [kCGImageDestinationLossyCompressionQuality: 1.0] as CFDictionary)
        return CGImageDestinationFinalize(destination) ? data as Data : nil
    }

    /// The file's TEGRA_NVJPG_SUB_* code from its frame header's first component: 4:2:0 0, 4:2:2 1, 4:4:4 2
    static func subsampling(_ file: Data) -> UInt32? {
        let bytes = [UInt8](file)
        var at = 2
        while at + 12 < bytes.count, bytes[at] == 0xff {
            let marker = bytes[at + 1]
            let length = Int(bytes[at + 2]) << 8 | Int(bytes[at + 3])
            if marker == 0xc0 || marker == 0xc1 {
                switch bytes[at + 11] {
                case 0x22: return 0
                case 0x21: return 1
                case 0x11: return 2
                default: return nil
                }
            }
            at += 2 + length
        }
        return nil
    }

    /// NVJPG's decode of `file` into `surface` (BGRA, the file's size), or why there is none
    static func decode(_ file: Data, into surface: IOSurface) -> TestFailure? {
        guard let sub = subsampling(file) else { return TestFailure("no baseline frame header in the JPEG") }
        let rowBytes = 4096
        let rows = (file.count + rowBytes - 1) / rowBytes
        guard
            let source = IOSurface(properties: [
                .width: rowBytes, .height: rows, .bytesPerElement: 1, .bytesPerRow: rowBytes,
                .pixelFormat: UInt32(0x4C30_3038),
            ]), source.allocationSize >= file.count
        else { return TestFailure("no source surface for \(file.count) bytes") }
        source.lock(options: [], seed: nil)
        file.withUnsafeBytes { source.baseAddress.copyMemory(from: $0.baseAddress!, byteCount: file.count) }
        source.unlock(options: [], seed: nil)
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let open = dlsym(iokit, "IOServiceOpen"), let release = dlsym(iokit, "IOObjectRelease"),
            let close = dlsym(iokit, "IOServiceClose"), let call = dlsym(iokit, "IOConnectCallStructMethod")
        else { return TestFailure("no IOKit") }
        let jpeg = unsafeBitCast(service, to: JPEGRoute.Service.self)(
            0, unsafeBitCast(matching, to: JPEGRoute.Matching.self)("AppleJPEGDriver"))
        guard jpeg != 0 else { return TestFailure("no AppleJPEGDriver service") }
        var connection: mach_port_t = 0
        let opened = unsafeBitCast(open, to: JPEGRoute.Open.self)(jpeg, mach_task_self_, 0, &connection)
        _ = unsafeBitCast(release, to: JPEGRoute.Release.self)(jpeg)
        guard opened == KERN_SUCCESS else {
            return TestFailure("open \(String(UInt32(bitPattern: opened), radix: 16))")
        }
        defer { _ = unsafeBitCast(close, to: JPEGRoute.Release.self)(connection) }
        var request = [UInt8](repeating: 0, count: JPEGRoute.requestBytes)
        let fields: [(Int, UInt32)] = [
            (0x00, IOSurfaceGetID(unsafeBitCast(source, to: IOSurfaceRef.self))),
            (0x04, UInt32(min(source.allocationSize, file.count))),
            (0x08, IOSurfaceGetID(unsafeBitCast(surface, to: IOSurfaceRef.self))),
            (0x0c, UInt32(surface.allocationSize)), (0x14, UInt32(surface.width)), (0x18, UInt32(surface.height)),
            (0x2c, sub), (0x4c, UInt32(surface.width)), (0x50, UInt32(surface.height)),
        ]
        request.withUnsafeMutableBytes { raw in
            for (offset, value) in fields {
                raw.storeBytes(of: value.littleEndian, toByteOffset: offset, as: UInt32.self)
            }
        }
        var reply = [UInt8](repeating: 0, count: JPEGRoute.requestBytes)
        var size = JPEGRoute.requestBytes
        let kr = request.withUnsafeBytes { input in
            reply.withUnsafeMutableBytes { output in
                unsafeBitCast(call, to: JPEGRoute.Call.self)(
                    connection, 7, input.baseAddress, input.count, output.baseAddress, &size)
            }
        }
        withExtendedLifetime(source) {}
        return kr == KERN_SUCCESS ? nil : TestFailure("selector 7 \(String(UInt32(bitPattern: kr), radix: 16))")
    }

    /// The layout NVJPG's writes match when a read is wrong: the picture written as rows (`rows`) or into blocks of 2^n
    /// GOBs (`y<n>`), read back through the blocks GM20B decided for the surface (TegraGM20BLayoutFit.h); `-` for none
    static func explained(_ got: [UInt8], _ surface: IOSurface) -> String {
        let rowBytes = (surface.width * 4 + 63) & ~63
        var decided = SurfaceCrossProcessTest.blockHeightLog2(surface.height)
        while decided > 0, rowBytes * blockRows(surface.height, decided) > surface.allocationSize { decided -= 1 }
        var best = ("-", Int.max)
        for candidate in [-1, 0, 1, 2, 3, 4, 5] {
            var memory = [UInt8](repeating: 0, count: rowBytes * blockRows(surface.height, 5))
            for y in 0..<surface.height {
                for xB in 0..<(surface.width * 4) {
                    let at =
                        candidate < 0
                        ? y * rowBytes + xB : SurfaceCrossProcessTest.gobOffset(rowBytes, candidate, xB, y)
                    if at < memory.count { memory[at] = byte(xB / 4, y, xB % 4) }
                }
            }
            var wrong = 0
            for y in 0..<surface.height {
                for xB in 0..<(surface.width * 4) {
                    let at = SurfaceCrossProcessTest.gobOffset(rowBytes, decided, xB, y)
                    let read = at < memory.count ? Int(memory[at]) : 0
                    if abs(read - Int(got[y * surface.width * 4 + xB])) > tolerance { wrong += 1 }
                }
            }
            if wrong < best.1 { best = (candidate < 0 ? "rows" : "y\(candidate)", wrong) }
        }
        return best.1 == 0 ? best.0 : "-"
    }

    static func blockRows(_ height: Int, _ yLog2: Int) -> Int {
        let block = 8 << yLog2
        return (height + block - 1) / block * block
    }

    /// Bytes further than `tolerance` from the picture, and the first one's offset in the rows (-1: none)
    static func wrong(_ got: [UInt8], _ surface: IOSurface) -> (Int, Int) {
        var wrong = 0, first = -1
        for y in 0..<surface.height {
            for x in 0..<surface.width {
                for c in 0..<4 where abs(Int(got[(y * surface.width + x) * 4 + c]) - Int(byte(x, y, c))) > tolerance {
                    wrong += 1
                    if first < 0 { first = (y * surface.width + x) * 4 + c }
                }
            }
        }
        return (wrong, first)
    }
}
