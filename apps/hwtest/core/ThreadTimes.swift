import Darwin
import Foundation

/// The process's CPU time per live thread (THREAD_EXTENDED_INFO, keyed by THREAD_IDENTIFIER_INFO's id, which the
/// kernel never reuses) and what its exited threads used (MACH_TASK_BASIC_INFO counts only those).
public struct ThreadTimes: Equatable {
    public struct Thread: Equatable {
        public let name: String
        public let nanoseconds: UInt64

        public init(name: String, nanoseconds: UInt64) {
            self.name = name
            self.nanoseconds = nanoseconds
        }
    }

    public static let driverPrefix = "com.brunostjohn.switch.gm20b."

    public var threads: [UInt64: Thread]
    public var exitedNanoseconds: UInt64
    public var main: UInt64

    public init(threads: [UInt64: Thread], exitedNanoseconds: UInt64, main: UInt64) {
        self.threads = threads
        self.exitedNanoseconds = exitedNanoseconds
        self.main = main
    }

    /// CPU milliseconds between `earlier` and this sample. Pool: unnamed threads, the test's tasks and the dispatch
    /// queues it and the driver run on, with the driver's work done on them. Driver: its own named threads. Other:
    /// every other named thread, and threads that exited in between (their time since `earlier`).
    public func split(since earlier: ThreadTimes) -> CPUSplit {
        var split = CPUSplit()
        for (id, thread) in threads {
            let ms = Double(thread.nanoseconds - min(thread.nanoseconds, earlier.threads[id]?.nanoseconds ?? 0)) / 1e6
            if id == main {
                split.main += ms
            } else if thread.name.hasPrefix(Self.driverPrefix) {
                split.driver += ms
            } else if thread.name.isEmpty {
                split.pool += ms
            } else {
                split.other += ms
            }
        }
        let goneEarlier = earlier.threads.filter { threads[$0.key] == nil }.values.reduce(0) { $0 + $1.nanoseconds }
        let exited = exitedNanoseconds - min(exitedNanoseconds, earlier.exitedNanoseconds + goneEarlier)
        split.other += Double(exited) / 1e6
        return split
    }

    /// `main`: the main thread's id (`current()` on it).
    public static func sample(main: UInt64) -> ThreadTimes {
        var times = ThreadTimes(threads: [:], exitedNanoseconds: exited(), main: main)
        var list: thread_act_array_t?
        var count: mach_msg_type_number_t = 0
        guard task_threads(mach_task_self_, &list, &count) == KERN_SUCCESS, let list else { return times }
        defer {
            vm_deallocate(
                mach_task_self_, vm_address_t(UInt(bitPattern: list)),
                vm_size_t(Int(count) * MemoryLayout<thread_act_t>.stride)
            )
        }
        for index in 0 ..< Int(count) {
            let port = list[index]
            if let id = identifier(port), let thread = extended(port) { times.threads[id] = thread }
            mach_port_deallocate(mach_task_self_, port)
        }
        return times
    }

    /// One thread's CPU nanoseconds by its port (pthread_mach_thread_np, which holds no reference), from any thread:
    /// one thread_info call, cheap enough for every test boundary.
    public static func nanoseconds(of port: thread_act_t) -> UInt64? {
        extended(port)?.nanoseconds
    }

    public static func current() -> UInt64 {
        var id: UInt64 = 0
        pthread_threadid_np(nil, &id)
        return id
    }

    private static func identifier(_ port: thread_act_t) -> UInt64? {
        var info = thread_identifier_info()
        var count = mach_msg_type_number_t(MemoryLayout<thread_identifier_info>.size / MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                thread_info(port, thread_flavor_t(THREAD_IDENTIFIER_INFO), $0, &count)
            }
        }
        return status == KERN_SUCCESS ? info.thread_id : nil
    }

    private static func extended(_ port: thread_act_t) -> Thread? {
        var info = thread_extended_info()
        var count = mach_msg_type_number_t(MemoryLayout<thread_extended_info>.size / MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                thread_info(port, thread_flavor_t(THREAD_EXTENDED_INFO), $0, &count)
            }
        }
        guard status == KERN_SUCCESS else { return nil }
        let name = withUnsafeBytes(of: info.pth_name) { bytes in
            String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
        }
        return Thread(name: name, nanoseconds: info.pth_user_time + info.pth_system_time)
    }

    private static func exited() -> UInt64 {
        var info = mach_task_basic_info()
        var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size / MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), $0, &count)
            }
        }
        guard status == KERN_SUCCESS else { return 0 }
        return [info.user_time, info.system_time].reduce(0) {
            $0 + UInt64($1.seconds) * 1_000_000_000 + UInt64($1.microseconds) * 1000
        }
    }
}

public struct CPUSplit: Equatable {
    public static let key = "cpu_split_ms"

    public var pool = 0.0, driver = 0.0, main = 0.0, other = 0.0

    public init() {}

    /// One text field, so tools/hwtest/compare.py does not hold every test's CPU to its 10 % band run against run.
    public var field: String {
        [("pool", pool), ("driver", driver), ("main", main), ("other", other)]
            .map { "\($0):\(String(format: "%.1f", $1))" }.joined(separator: ",")
    }
}
