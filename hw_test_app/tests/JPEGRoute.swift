import Darwin
import Foundation
import HWTestCore

/// How many JPEG decodes NVJPG finished for this process, and the output formats they wrote (debug kernels): selector 7
/// of AppleJPEGDriverUserClient, the one CMPhoto's own decodes use, with 0x5050 at +0xcac and no source surface
/// (drivers/TegraNVJPG/CONTRACT.md).
enum JPEGRoute {
    struct Counts {
        let decodes: UInt64
        let formats: UInt64
    }

    /// TegraNVJPGJpeg.h's TEGRA_NVJPG_OUT_* order.
    static let formatNames = ["420f", "y420", "BGRA", "RGBA", "ARGB", "L008", "422f", "444f", "yuvf"]

    static func names(_ formats: UInt64) -> String {
        let named = formatNames.enumerated().filter { formats >> UInt64($0.offset) & 1 != 0 }.map(\.element)
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
            reply.withUnsafeBytes {
                Counts(decodes: $0.load(as: UInt64.self), formats: $0.load(fromByteOffset: 8, as: UInt64.self))
            })
    }
}
