import Darwin
import Foundation
import HWTestCore
import HWTestKit
import HWTestShared

/// The flight recorder (kernel/switch/recorder, kernels built with --//kernel:recorder): the app's main thread held off
/// its run loop for 400 ms must be taken for a hang of this thread and dumped. The test owns that dump: writing its
/// main thread's id to debug.switch_recorder_expect waits until the recorder is quiet, then turns every other thread's
/// automatic trigger away, and debug.switch_recorder_wait returns once this thread's capture is written. The dump's
/// ring must then parse as the host tool reads it, with this hang's trigger among its events.
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
        "span_ms", "events_s", "samples", "captures", "dumps", "pending", "skipped", "held", "queued", "taken", "hang",
        "late_frame", "mark", "last_reason", "last_value", "last_slot", "last_trace_bytes", "last_shot_bytes",
        "last_deflate_bytes", "last_trace_us", "last_shot_us", "last_latency_us", "last_deflate_us", "last_write_us",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let first = KernelSysctl.string(Self.status)
        guard case .value(let before) = first else {
            return .skip(KernelSysctl.unreadable(Self.status, first.error, flag: "--//kernel:recorder"), Metrics())
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
            return .skip(KernelSysctl.unreadable(Self.wait, second.error, flag: "--//kernel:recorder"), Metrics())
        }
        let fields = KernelLine.fields(after)
        let ring = RecorderRing(dump: "\(Self.directory)/Switch-recorder-\(fields["last_slot"] ?? "0").rec")
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
                ("the ring was in memory within 250 ms of the trigger (last_latency_us < 250000)",
                 number("last_latency_us") < 250_000),
                ("the ring parses: RAW kdebug at offset 0, then events (ring_error = -)", ring.error == nil),
                ("the ring holds this hang's trigger for this thread", ring.triggers.contains(tid)),
            ], metrics
        )
    }
}

/// A dump's ring as tools/perf/dumps.py reads it. The file (kernel/switch/recorder/store.c) is a 1024-byte text header
/// ("SwitchRecorder 2 ... shot=<bytes> ring=<bytes> deflate=<bytes> ..."), then the stackshot and the ring as one raw
/// deflate stream. The ring is RAW_VERSION1 kdebug: its header at offset 0, padding to 4 KiB, then 64-byte events
/// (mach time, four arguments, thread, debug id, CPU).
struct RecorderRing {
    static let version: UInt32 = 0x55AA_0101
    static let trigger: UInt32 = (6 << 24) | (18 << 16) | (0x3E0 << 2)
    static let header = 1024
    var events = 0
    var triggers = Set<UInt64>()
    var error: String?

    init(dump path: String) {
        switch Self.ring(dump: path) {
        case .success(let data): self.init(ring: data)
        case .failure(let why): self.init(error: why.text)
        }
    }

    private init(error: String) {
        self.error = error
    }

    struct Unreadable: Error {
        let text: String
    }

    /// The ring inside a dump file: the body inflated and split at the stackshot's size.
    static func ring(dump path: String) -> Result<Data, Unreadable> {
        guard let file = FileManager.default.contents(atPath: path), file.count > header else {
            return .failure(Unreadable(text: "unreadable"))
        }
        let text = String(decoding: file.prefix(header), as: UTF8.self)
        let fields = KernelLine.fields(text)
        guard text.hasPrefix("SwitchRecorder 2 "), let shot = Int(fields["shot"] ?? ""),
              let ring = Int(fields["ring"] ?? ""), let packed = Int(fields["deflate"] ?? ""),
              file.count >= header + packed
        else {
            return .failure(Unreadable(text: "no complete SwitchRecorder 2 header"))
        }
        let body = file.subdata(in: header ..< (header + packed)) as NSData
        guard let whole = try? body.decompressed(using: .zlib) as Data, whole.count == shot + ring else {
            return .failure(Unreadable(text: "the body does not inflate to shot=\(shot) ring=\(ring)"))
        }
        return .success(whole.subdata(in: shot ..< (shot + ring)))
    }

    init(ring data: Data) {
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
