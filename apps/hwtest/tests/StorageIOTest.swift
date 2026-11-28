import Darwin
import Foundation
import HWTestCore
import HWTestKit
import HWTestShared

/// The SD driver's own accounting (kern.switch_sd, debug kernels; drivers/NativeSwitchBlock/SwitchSdIo.h) around this
/// test's reads: random 4 KiB pages of a 16 MiB file with F_NOCACHE into page-aligned memory, one reader at a time and
/// then 32 reader threads at once. Its setup ran no self-tests (setup_ms), the 32 readers really queued at the driver
/// (deep_queue), the card moved every byte straight into the readers' pages (no bounce copy), and every page came back
/// as written. The rest is reported: command and request times per pass, interrupts per command, and what the cryptex
/// devices copied and raised to the high class since boot.
struct StorageIOTest: HWTest {
    let id = "storage.io"
    let name = "SD requests: setup, copies, latency at depth"
    let subsystem = Subsystem.storage

    static let sysctl = "kern.switch_sd"
    static let size = 16 << 20
    static let page = 4096
    static let reads = 1024
    static let threads = 32
    // bus up and tuned (47-48 ms on six boots on disk), identification, GPT entries, no setup self-tests
    static let setupCeilingMs = 75.0
    // requests waiting at the driver on average while 32 readers each keep one read outstanding
    static let deepQueueFloor = 8.0

    func run(_ context: TestContext) async throws -> Outcome {
        let read = KernelSysctl.string(Self.sysctl)
        guard case .value(let boot) = read, boot != "down" else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(read.error))
            return .skip(KernelSysctl.unreadable(Self.sysctl, read.error, flag: "--//bazel:stats_io"), metrics)
        }
        let url = Disk.file("io.bin")
        defer { try? FileManager.default.removeItem(at: url) }
        let pattern = Disk.block(1 << 20, seed: 11)
        let writer = try Disk.open(url, write: true)
        for offset in stride(from: 0, to: Self.size, by: 1 << 20) {
            _ = pattern.withUnsafeBytes { pwrite(writer, $0.baseAddress, 1 << 20, off_t(offset)) }
        }
        fsync(writer)
        close(writer)
        let fd = try Disk.open(url, write: false)
        defer { close(fd) }

        let before = SdTotals(Self.sysctlLine())
        let serial = Self.pass(fd, threads: 1, pattern: pattern)
        let middle = SdTotals(Self.sysctlLine())
        let deep = Self.pass(fd, threads: Self.threads, pattern: pattern)
        let after = SdTotals(Self.sysctlLine())

        var metrics = Metrics()
        let setupMs = Double(SdTotals(boot).value("setup_us")) / 1000
        metrics.set("setup_ms", setupMs, digits: 1)
        metrics.set("serial_iops", Double(Self.reads) / serial.seconds, digits: 0)
        metrics.set("deep_iops", Double(Self.reads) / deep.seconds, digits: 0)
        for (name, from, to) in [("serial", before, middle), ("deep", middle, after)] {
            let cmd = to.histogram("rd0").minus(from.histogram("rd0"))
            let all = to.histogram("all0").minus(from.histogram("all0"))
            let wait = to.histogram("wait0").minus(from.histogram("wait0"))
            metrics.set("\(name)_cmd_p50_us", cmd.percentileUs(500))
            metrics.set("\(name)_all_p50_us", all.percentileUs(500))
            metrics.set("\(name)_all_p99_us", all.percentileUs(990))
            metrics.set("\(name)_wait_p50_us", wait.percentileUs(500))
            let rtPhases = [("submit", "rt0"), ("prep", "rt1"), ("wire", "rt2"), ("wake", "rt3"), ("gap", "rt4")]
            for (phase, key) in rtPhases {
                let hist = to.histogram(key, firstUs: 1).minus(from.histogram(key, firstUs: 1))
                metrics.set("\(name)_rt_\(phase)_p50_us", hist.percentileUs(500))
                if phase == "wake" || phase == "gap" {
                    metrics.set("\(name)_rt_\(phase)_mean_us", hist.meanUs)
                }
            }
            var commands = 0
            for row in ["rd", "wr"] {
                for size in 0 ..< 4 {
                    commands += to.histogram("\(row)\(size)").count - from.histogram("\(row)\(size)").count
                }
            }
            let interrupts = to.value("irq") - from.value("irq")
            metrics.set("\(name)_irq_per_cmd", commands > 0 ? Double(interrupts) / Double(commands) : 0, digits: 2)
        }
        let span = deep.seconds * 1e6
        let deepQueue = span > 0 ? Double(after.value("depth_us") - middle.value("depth_us")) / span : 0
        metrics.set("deep_queue", deepQueue, digits: 1)
        let bounced = after.value("bounce") - before.value("bounce")
        metrics.set("bounce_kib", bounced >> 10)
        metrics.set("cryptex_copied_mib", after.value("cryptex_copied") >> 20)
        metrics.set("cryptex_waited_mib", after.value("cryptex_waited") >> 20)
        metrics.set("cryptex_card_mib", after.value("cryptex_card") >> 20)
        metrics.set("cryptex_raised_mib", after.value("cryptex_raised") >> 20)
        return .judged(
            [
                ("the driver set up within \(Int(Self.setupCeilingMs)) ms", setupMs <= Self.setupCeilingMs),
                ("32 readers kept the queue \(Int(Self.deepQueueFloor)) deep", deepQueue >= Self.deepQueueFloor),
                ("every byte went straight to the readers' pages (no bounce copy)", bounced == 0),
                ("every page read back as written", serial.wrong == 0 && deep.wrong == 0),
            ], metrics
        )
    }

    static func sysctlLine() -> String {
        if case .value(let line) = KernelSysctl.string(sysctl) { return line }
        return ""
    }

    /// `reads` random pages over `threads` threads of their own, all at once (one blocking read outstanding each): the
    /// pages that came back wrong, and the seconds it took.
    static func pass(_ fd: Int32, threads: Int, pattern: [UInt8]) -> (wrong: Int, seconds: Double) {
        // One slot per reader, each written by its own thread before it leaves the group.
        let wrong = UnsafeMutableBufferPointer<Int>.allocate(capacity: threads)
        wrong.initialize(repeating: 0)
        defer { wrong.deallocate() }
        let done = DispatchGroup()
        let start = CFAbsoluteTimeGetCurrent()
        for index in 0 ..< threads {
            done.enter()
            let reader = Thread {
                wrong[index] = readPages(fd, count: reads / threads, pattern: pattern)
                done.leave()
            }
            reader.start()
        }
        done.wait()
        return (wrong.reduce(0, +), CFAbsoluteTimeGetCurrent() - start)
    }

    /// `count` random pages of the file into one page-aligned buffer: how many came back wrong.
    static func readPages(_ fd: Int32, count: Int, pattern: [UInt8]) -> Int {
        var allocation: UnsafeMutableRawPointer?
        guard posix_memalign(&allocation, Int(getpagesize()), page) == 0, let buffer = allocation else { return count }
        defer { free(buffer) }
        var generator = SystemRandomNumberGenerator()
        var bad = 0
        for _ in 0 ..< count {
            let offset = Int.random(in: 0 ..< (size / page), using: &generator) * page
            let got = pread(fd, buffer, page, off_t(offset))
            let at = offset % (1 << 20)
            let same = pattern.withUnsafeBytes { memcmp(buffer, $0.baseAddress! + at, page) == 0 }
            if got != page || !same { bad += 1 }
        }
        return bad
    }
}

/// kern.switch_sd's "key=value" tokens; a histogram's value is "count,sum_us,b0,...,b31" over half-octave buckets from
/// 64 us (SwitchSdIo.h switchSdBucketLow).
struct SdTotals {
    var fields: [String: [UInt64]] = [:]

    init(_ line: String) {
        for token in line.split(separator: " ") {
            let pair = token.split(separator: "=", maxSplits: 1)
            guard pair.count == 2 else { continue }
            fields[String(pair[0])] = pair[1].split(separator: ",").map { UInt64($0) ?? 0 }
        }
    }

    func value(_ key: String) -> Int { Int(fields[key]?.first ?? 0) }

    func histogram(_ key: String, firstUs: Int = 64) -> SdHistogram {
        let values = fields[key] ?? []
        let sumUs = values.count > 1 ? Int(values[1]) : 0
        let buckets = values.count > 2 ? values.dropFirst(2).map { Int($0) } : []
        return SdHistogram(count: Int(values.first ?? 0), sumUs: sumUs, firstUs: firstUs, buckets: buckets)
    }
}

struct SdHistogram {
    var count: Int
    var sumUs: Int
    var firstUs: Int
    var buckets: [Int]

    var meanUs: Int { count > 0 ? sumUs / count : 0 }

    func minus(_ other: SdHistogram) -> SdHistogram {
        let n = max(buckets.count, other.buckets.count)
        let mine = buckets + Array(repeating: 0, count: n - buckets.count)
        let theirs = other.buckets + Array(repeating: 0, count: n - other.buckets.count)
        return SdHistogram(
            count: count - other.count,
            sumUs: sumUs - other.sumUs,
            firstUs: firstUs,
            buckets: zip(mine, theirs).map { $0 - $1 }
        )
    }

    /// The top of the bucket holding the `permille`-th value, in us; 0 when empty.
    func percentileUs(_ permille: Int) -> Int {
        guard count > 0 else { return 0 }
        let rank = (count * permille + 999) / 1000
        var seen = 0
        for (bucket, n) in buckets.enumerated() {
            seen += n
            if seen >= rank {
                return Self.low(bucket + 1 < buckets.count ? bucket + 1 : bucket, firstUs: firstUs)
            }
        }
        return Self.low(max(buckets.count - 1, 0), firstUs: firstUs)
    }

    /// A bucket's lower bound in us: 0 below `firstUs`, then two per doubling.
    static func low(_ bucket: Int, firstUs: Int = 64) -> Int {
        guard bucket > 0 else { return 0 }
        let octave = (firstUs * 1000) << ((bucket - 1) / 2)
        let ns = (bucket - 1) % 2 == 1 ? octave * 362 / 256 : octave
        return ns / 1000
    }
}
