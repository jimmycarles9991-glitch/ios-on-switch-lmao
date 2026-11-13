import Darwin
import Foundation
import HWTestCore

/// TegraVIC's DEVELOPMENT probes (drivers/TegraVIC/SwitchVICKernelTest.h): IOSurfaceAcceleratorClient selector 6 with
/// {magic, 0}, the probe's reply back. A release kernel refuses the selector. A VIC job that hangs panics the kernel, so
/// these run last.
enum X1VICKernelTest {
    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> CFMutableDictionary?
    typealias Service = @convention(c) (mach_port_t, CFDictionary?) -> mach_port_t
    typealias Open =
        @convention(c) (mach_port_t, mach_port_t, UInt32, UnsafeMutablePointer<mach_port_t>) -> kern_return_t
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t
    typealias Call =
        @convention(c) (
            mach_port_t, UInt32, UnsafeRawPointer?, Int, UnsafeMutableRawPointer?, UnsafeMutablePointer<Int>
        ) -> kern_return_t

    static let selector: UInt32 = 6

    /// The reply's words, or why there are none.
    static func call(_ magic: UInt32, replyBytes: Int) -> Result<[UInt32], TestFailure> {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let open = dlsym(iokit, "IOServiceOpen"), let release = dlsym(iokit, "IOObjectRelease"),
            let close = dlsym(iokit, "IOServiceClose"), let call = dlsym(iokit, "IOConnectCallStructMethod")
        else { return .failure(TestFailure("no IOKit")) }
        let vic = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("AppleM2ScalerCSCDriver"))
        guard vic != 0 else { return .failure(TestFailure("no AppleM2ScalerCSCDriver service")) }
        var connection: mach_port_t = 0
        let opened = unsafeBitCast(open, to: Open.self)(vic, mach_task_self_, 0, &connection)
        _ = unsafeBitCast(release, to: Release.self)(vic)
        guard opened == KERN_SUCCESS else {
            return .failure(TestFailure("open \(String(UInt32(bitPattern: opened), radix: 16))"))
        }
        defer { _ = unsafeBitCast(close, to: Release.self)(connection) }
        let request: [UInt32] = [magic, 0]
        var reply = [UInt32](repeating: 0, count: replyBytes / 4)
        var size = replyBytes
        let kr = request.withUnsafeBytes { input in
            reply.withUnsafeMutableBytes { output in
                unsafeBitCast(call, to: Call.self)(
                    connection, selector, input.baseAddress, input.count, output.baseAddress, &size)
            }
        }
        guard kr == KERN_SUCCESS, size == replyBytes else {
            return .failure(TestFailure("call \(String(UInt32(bitPattern: kr), radix: 16)) size \(size)"))
        }
        return .success(reply)
    }

    /// A fourcc's four characters, lower-cased for a metric key.
    static func name(_ fourcc: UInt32) -> String {
        String((0..<4).map { Character(UnicodeScalar(UInt8(truncatingIfNeeded: fourcc >> (24 - 8 * $0)))) })
            .lowercased()
    }
}
