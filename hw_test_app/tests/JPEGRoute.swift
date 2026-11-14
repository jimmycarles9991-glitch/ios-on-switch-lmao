import Darwin
import Foundation
import HWTestCore

/// How many JPEG decodes NVJPG finished, failed and refused for this process, the output formats it wrote and the ones
/// asked for (debug kernels): selector 7 of AppleJPEGDriverUserClient, the one CMPhoto's own decodes use, with 0x5050
/// at +0xcac and no source surface (drivers/TegraNVJPG/TegraNVJPGStats.h).
enum JPEGRoute {
    struct Counts {
        let decodes: UInt64
        let formats: UInt64
        let failed: UInt64
        let refused: UInt64
        /// Requests per TEGRA_NVJPG_OUT_* (formatNames' order), refused and failed ones too.
        let asked: [UInt64]
    }

    /// TegraNVJPGJpeg.h's TEGRA_NVJPG_OUT_* order.
    static let formatNames = ["420f", "y420", "BGRA", "RGBA", "ARGB", "L008", "422f", "444f", "yuvf"]

    static func names(_ formats: UInt64) -> String {
        let named = formatNames.enumerated().filter { formats >> UInt64($0.offset) & 1 != 0 }.map(\.element)
        return named.isEmpty ? "-" : named.joined(separator: ",")
    }

    /// The formats asked for between two reads, with their counts: "444f:1,BGRA:1".
    static func asked(from first: Counts, to last: Counts) -> String {
        let named = formatNames.indices.compactMap { index -> String? in
            let count = Int(truncatingIfNeeded: last.asked[index] &- first.asked[index])
            return count > 0 ? "\(formatNames[index]):\(count)" : nil
        }
        return named.isEmpty ? "-" : named.joined(separator: ",")
    }

    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> CFMutableDictionary?
    typealias Service = @convention(c) (mach_port_t, CFDictionary?) -> mach_port_t
    typealias Open =
        @convention(c) (mach_port_t, mach_port_t, UInt32, UnsafeMutablePointer<mach_port_t>) -> kern_return_t
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t
    typealias Call =
        @convention(c) (
            mach_port_t, UInt32, UnsafeRawPointer?, Int, UnsafeMutableRawPointer?, UnsafeMutablePointer<Int>
        ) -> kern_return_t

    static let requestBytes = 0xda0

    /// The count, or why there is none.
    static func decodes() -> Result<Counts, TestFailure> {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let open = dlsym(iokit, "IOServiceOpen"), let release = dlsym(iokit, "IOObjectRelease"),
            let close = dlsym(iokit, "IOServiceClose"), let call = dlsym(iokit, "IOConnectCallStructMethod")
        else { return .failure(TestFailure("no IOKit")) }
        let jpeg = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("AppleJPEGDriver"))
        guard jpeg != 0 else { return .failure(TestFailure("no AppleJPEGDriver service")) }
        var connection: mach_port_t = 0
        let opened = unsafeBitCast(open, to: Open.self)(jpeg, mach_task_self_, 0, &connection)
        _ = unsafeBitCast(release, to: Release.self)(jpeg)
        guard opened == KERN_SUCCESS else {
            return .failure(TestFailure("open \(String(UInt32(bitPattern: opened), radix: 16))"))
        }
        defer { _ = unsafeBitCast(close, to: Release.self)(connection) }
        var request = [UInt8](repeating: 0, count: requestBytes)
        request.withUnsafeMutableBytes {
            $0.storeBytes(of: UInt32(0x5050).littleEndian, toByteOffset: 0xcac, as: UInt32.self)
        }
        var reply = [UInt8](repeating: 0, count: requestBytes)
        var size = requestBytes
        let kr = request.withUnsafeBytes { input in
            reply.withUnsafeMutableBytes { output in
                unsafeBitCast(call, to: Call.self)(
                    connection, 7, input.baseAddress, input.count, output.baseAddress, &size)
            }
        }
        guard kr == KERN_SUCCESS, size == requestBytes else {
            return .failure(TestFailure("selector 7 \(String(UInt32(bitPattern: kr), radix: 16)) size \(size)"))
        }
        return .success(
            reply.withUnsafeBytes { words in
                Counts(
                    decodes: words.load(as: UInt64.self), formats: words.load(fromByteOffset: 8, as: UInt64.self),
                    failed: words.load(fromByteOffset: 16, as: UInt64.self),
                    refused: words.load(fromByteOffset: 24, as: UInt64.self),
                    asked: formatNames.indices.map { words.load(fromByteOffset: 32 + 8 * $0, as: UInt64.self) })
            })
    }
}
