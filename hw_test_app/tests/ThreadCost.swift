import Darwin

/// What a stretch of the calling thread cost: its user and system CPU time (THREAD_BASIC_INFO) and the task's Mach and
/// Unix system calls, context switches and page faults over it (TASK_EVENTS_INFO). User time is the frameworks' own work;
/// system time and the calls are what reached the kernel (IOKit and HID user clients, sysctl, Mach messages); wall time
/// neither covers is time the thread was blocked.
struct ThreadCost {
    var userUs = 0.0, systemUs = 0.0
    var machCalls = 0, unixCalls = 0, switches = 0, faults = 0

    static func sample() -> ThreadCost {
        var cost = ThreadCost()
        var thread = thread_basic_info()
        var count = mach_msg_type_number_t(MemoryLayout<thread_basic_info>.size / MemoryLayout<integer_t>.size)
        let port = mach_thread_self()
        let status = withUnsafeMutablePointer(to: &thread) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                thread_info(port, thread_flavor_t(THREAD_BASIC_INFO), $0, &count)
            }
        }
        mach_port_deallocate(mach_task_self_, port)
        if status == KERN_SUCCESS {
            cost.userUs = Double(thread.user_time.seconds) * 1e6 + Double(thread.user_time.microseconds)
            cost.systemUs = Double(thread.system_time.seconds) * 1e6 + Double(thread.system_time.microseconds)
        }
        var events = task_events_info()
        var eventCount = mach_msg_type_number_t(MemoryLayout<task_events_info>.size / MemoryLayout<integer_t>.size)
        let taskStatus = withUnsafeMutablePointer(to: &events) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(eventCount)) {
                task_info(mach_task_self_, task_flavor_t(TASK_EVENTS_INFO), $0, &eventCount)
            }
        }
        if taskStatus == KERN_SUCCESS {
            cost.machCalls = Int(events.syscalls_mach)
            cost.unixCalls = Int(events.syscalls_unix)
            cost.switches = Int(events.csw)
            cost.faults = Int(events.faults)
        }
        return cost
    }

    static func - (a: ThreadCost, b: ThreadCost) -> ThreadCost {
        ThreadCost(
            userUs: a.userUs - b.userUs, systemUs: a.systemUs - b.systemUs, machCalls: a.machCalls - b.machCalls,
            unixCalls: a.unixCalls - b.unixCalls, switches: a.switches - b.switches, faults: a.faults - b.faults)
    }
}
