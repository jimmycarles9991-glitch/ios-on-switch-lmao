import Darwin
import Foundation
import HWTestCore
import os

/// The VM's page counts, jetsam's free-memory level and how much more this app may take before jetsam kills it.
struct MemoryTest: HWTest {
    let id = "system.memory"
    let name = "Free memory, compressor and jetsam headroom"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        var stats = vm_statistics64()
        var count = mach_msg_type_number_t(MemoryLayout<vm_statistics64>.size / MemoryLayout<integer_t>.size)
        let status = withUnsafeMutablePointer(to: &stats) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &count)
            }
        }
        guard status == KERN_SUCCESS else { return .fail("host_statistics64: \(status)") }
        let megabytes = { (pages: UInt32) in Int(UInt64(pages) * UInt64(vm_kernel_page_size) >> 20) }
        let headroom = Int(os_proc_available_memory() >> 20)
        let level = DeviceInfo.sysctlInt("kern.memorystatus_level")
        var metrics = Metrics()
        metrics.set("total_mb", Int(ProcessInfo.processInfo.physicalMemory >> 20))
        metrics.set("free_mb", megabytes(stats.free_count))
        metrics.set("active_mb", megabytes(stats.active_count))
        metrics.set("inactive_mb", megabytes(stats.inactive_count))
        metrics.set("wired_mb", megabytes(stats.wire_count))
        metrics.set("compressed_mb", megabytes(stats.compressor_page_count))
        metrics.set("purgeable_mb", megabytes(stats.purgeable_count))
        metrics.set("pageouts", Int(stats.pageouts))
        metrics.set("swapouts", Int(stats.swapouts))
        metrics.set("memorystatus_level", level)
        metrics.set("app_headroom_mb", headroom)
        metrics.set("page_kb", Int(vm_kernel_page_size >> 10))
        return .judged(
            [
                ("jetsam's free-memory level above 10%", level > 10),
                ("the app may take 256 MB more", headroom >= 256),
            ], metrics)
    }
}
