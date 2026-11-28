import Darwin
import Foundation

/// Another process's footprint against its jetsam inactive limit, read the way a memory-managing daemon reads it:
/// memorystatus_control (xnu bsd/sys/kern_memorystatus.h, the same numbers on xnu-10063 and xnu-12377), which the
/// app's com.apple.private.memorystatus entitlement admits where the sandbox refuses proc_pid_rusage. Excess is the
/// footprint past the inactive limit, 0 at or under it (MEMORYSTATUS_CMD_GET_MEMLIMIT_EXCESS).
public enum JetsamExcess {
    static let getMemlimitProperties: UInt32 = 8
    static let getMemlimitExcess: UInt32 = 13

    public struct Reading: Equatable, Sendable {
        public let inactiveLimitMB: Int
        public let excessBytes: UInt64

        public init(inactiveLimitMB: Int, excessBytes: UInt64) {
            self.inactiveLimitMB = inactiveLimitMB
            self.excessBytes = excessBytes
        }

        public var excessMB: Double { Double(excessBytes) / 1_048_576 }
    }

    typealias Control = @convention(c) (UInt32, Int32, UInt32, UnsafeMutableRawPointer?, Int) -> Int32

    /// The reading, or the errno of the call that failed.
    public static func read(pid: Int32) -> Result<Reading, POSIXError> {
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "memorystatus_control") else {
            return .failure(POSIXError(.ENOSYS))
        }
        let control = unsafeBitCast(symbol, to: Control.self)
        var limits = (Int32(0), UInt32(0), Int32(0), UInt32(0))
        guard control(getMemlimitProperties, pid, 0, &limits, MemoryLayout.size(ofValue: limits)) == 0 else {
            return .failure(POSIXError(POSIXErrorCode(rawValue: errno) ?? .EPERM))
        }
        var excess: UInt64 = 0
        guard control(getMemlimitExcess, pid, 0, &excess, MemoryLayout<UInt64>.size) == 0 else {
            return .failure(POSIXError(POSIXErrorCode(rawValue: errno) ?? .EPERM))
        }
        return .success(Reading(inactiveLimitMB: Int(limits.2), excessBytes: excess))
    }
}
