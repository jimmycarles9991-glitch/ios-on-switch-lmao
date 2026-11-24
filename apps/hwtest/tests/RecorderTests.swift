import Darwin
import Foundation
import HWTestCore

/// The flight recorder (kernel/switch/recorder, kernels built with --//kernel:recorder): the app's main thread held off
/// its run loop for 400 ms must be taken for a hang of this thread and dumped. The test owns that dump: writing its
/// main thread's id to debug.switch_recorder_expect waits until no dump is in flight and the gap has passed, then
/// turns every other thread's trigger away, and debug.switch_recorder_wait returns once this thread's dump is written.
/// The dump's ring must then parse as the host tool reads it, with this hang's trigger among its events.
struct RecorderHangDumpTest: HWTest {
    let id = "recorder.hang_dump"
    let name = "Flight recorder: a 400 ms main-thread hang dumps a ring that parses and names the thread"
    let subsystem = Subsystem.cpu
    var timeout: TimeInterval { 30 }

    static let status = "debug.switch_recorder"
    static let expect = "debug.switch_recorder_expect"
    static let wait = "debug.switch_recorder_wait"
    static let directory = "/private/var/mobile/Library/Logs/CrashReporter"
    static let holdMicroseconds: UInt32 = 400_000
    static let keys = [
        "span_ms", "events_s", "samples", "dumps", "skipped", "held", "taken", "hang", "late_frame", "mark",
        "last_reason", "last_value", "last_slot", "last_trace_bytes", "last_shot_bytes", "last_trace_us",
        "last_shot_us",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let first = KernelSysctl.string(Self.status)
        guard case .value(let before) = first else {
            return .skip(KernelSysctl.unreadable(Self.status, first.error), Metrics())
        }
        let dumps = Int(KernelLine.fields(before)["dumps"] ?? "") ?? 0
        let tid: UInt64 = await MainActor.run {
            var tid: UInt64 = 0
            pthread_threadid_np(nil, &tid)
            return tid
        }
        var owner = Int64(tid)
        guard sysctlbyname(Self.expect, nil, nil, &owner, MemoryLayout<Int64>.size) == 0 else {
            return .judged([("\(Self.expect) took this thread (\(KernelSysctl.code(errno)))", false)], Metrics())
        }
        await MainActor.run { _ = usleep(Self.holdMicroseconds) }
        let second = KernelSysctl.string(Self.wait)
        guard case .value(let after) = second else {
            return .skip(KernelSysctl.unreadable(Self.wait, second.error), Metrics())
        }
        let fields = KernelLine.fields(after)
        let ring = RecorderRing("\(Self.directory)/Switch-recorder-\(fields["last_slot"] ?? "0").ktrace")
        var metrics = KernelLine.metrics(fields, keys: Self.keys)
        metrics.set("main_tid", String(tid))
        metrics.set("last_tid", fields["last_tid"] ?? "-")
        metrics.set("ring_events", String(ring.events))
        metrics.set("ring_error", ring.error ?? "-")
        let number = { (key: String) in UInt64(fields[key] ?? "") ?? 0 }
        return .judged(
            [
                ("a dump followed the hang (dumps rose)", Int(number("dumps")) > dumps),
                ("a hang triggered it (last_reason = hang)", fields["last_reason"] == "hang"),
                ("it named this app's main thread (last_tid = main_tid)", fields["last_tid"] == String(tid)),
                ("the hang had lasted 250 ms or more when it fired (last_value >= 250)", number("last_value") >= 250),
                ("the stackshot was written (last_shot_bytes > 0)", number("last_shot_bytes") > 0),
                ("the ring parses: RAW kdebug at offset 0, then events (ring_error = -)", ring.error == nil),
                ("the ring holds this hang's trigger for this thread", ring.triggers.contains(tid)),
            ], metrics
        )
    }
}

/// A dump's ring as tools/perf/ktrace.py reads it: kdbg_dump_trace_to_file's RAW_VERSION1 header at offset 0, its
/// thread map, padding to 4 KiB, then 64-byte events (mach time, four arguments, thread, debug id, CPU).
struct RecorderRing {
    static let version: UInt32 = 0x55AA_0101
    static let trigger: UInt32 = (6 << 24) | (18 << 16) | (0x3E0 << 2)
    var events = 0
    var triggers = Set<UInt64>()
    var error: String?

    init(_ path: String) {
        guard let data = FileManager.default.contents(atPath: path) else {
            error = "unreadable"
            return
        }
        func word(_ at: Int) -> UInt32 {
            data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: at, as: UInt32.self) }
        }
        func quad(_ at: Int) -> UInt64 {
            data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: at, as: UInt64.self) }
        }
        guard data.count >= 24, word(0) == Self.version else {
            error = "no RAW_VERSION1 header at offset 0"
            return
        }
        let start = (24 + Int(word(4)) * 32 + 4095) / 4096 * 4096
        var at = start
        while at + 64 <= data.count {
            if quad(at) != 0 {
                events += 1
                if word(at + 48) & ~3 == Self.trigger, word(at + 8) == 2 {
                    triggers.insert(quad(at + 16))
                }
            }
            at += 64
        }
        if events == 0 {
            error = "no events after \(start)"
        }
    }
}
