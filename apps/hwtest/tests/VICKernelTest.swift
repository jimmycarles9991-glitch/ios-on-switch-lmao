import Darwin
import Foundation
import HWTestCore

/// TegraVIC's kernel-test selector 6 on AppleM2ScalerCSCDriver (debug kernels; a release kernel answers
/// kIOReturnNotPermitted): the IOKit client the app's sandbox admits, so the kernel experiments of other drivers come
/// through it too, told apart by the request's first word (drivers/TegraVIC/TegraVICUserClient.cpp kernelTest).
public enum VICKernelTest {
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

    /// The reply's `replyBytes` bytes, or why there are none.
    public static func call(_ request: [UInt8], replyBytes: Int) -> Result<[UInt8], TestFailure> {
        Session.open().flatMap { session in
            defer { session.close() }
            return session.call(request, replyBytes: replyBytes)
        }
    }

    /// One connection kept open across calls: what the kernel holds for a connection (forced display traits) lasts
    /// until it closes, or until the process dies.
    public final class Session: @unchecked Sendable {
        let connection: mach_port_t
        let callFunction: Call
        let closeFunction: Release

        init(connection: mach_port_t, call: Call, close: Release) {
            self.connection = connection
            callFunction = call
            closeFunction = close
        }

        public static func open() -> Result<Session, TestFailure> {
            guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
                let matching = dlsym(iokit, "IOServiceMatching"),
                let service = dlsym(iokit, "IOServiceGetMatchingService"), let open = dlsym(iokit, "IOServiceOpen"),
                let release = dlsym(iokit, "IOObjectRelease"), let close = dlsym(iokit, "IOServiceClose"),
                let call = dlsym(iokit, "IOConnectCallStructMethod")
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
            return .success(
                Session(
                    connection: connection, call: unsafeBitCast(call, to: Call.self),
                    close: unsafeBitCast(close, to: Release.self)))
        }

        public func close() {
            _ = closeFunction(connection)
        }

        public func call(_ request: [UInt8], replyBytes: Int) -> Result<[UInt8], TestFailure> {
            var reply = [UInt8](repeating: 0, count: max(replyBytes, 1))
            var size = replyBytes
            let kr = request.withUnsafeBytes { input in
                reply.withUnsafeMutableBytes { output in
                    callFunction(
                        connection, selector, input.baseAddress, input.count, replyBytes > 0 ? output.baseAddress : nil,
                        &size)
                }
            }
            guard kr == KERN_SUCCESS, size == replyBytes else {
                return .failure(TestFailure("call \(String(UInt32(bitPattern: kr), radix: 16)) size \(size)"))
            }
            return .success(Array(reply.prefix(replyBytes)))
        }
    }
}
