import Darwin
import Foundation
import HWTestCore
import os

/// What the bench screen shows.
struct BenchStatus: Sendable {
    var device = "opening the raw device"
    var selfCheck = "-"
    var roots: [String] = []
    var current = "waiting for a batch"
    var last = ""
    var finished = 0
}

/// The bench loop (tools/benchloop): batches the Mac puts in an inbox run one experiment at a time, each journalled
/// before it starts and its result written as it ends, until the screen closes. A batch is noticed when the inbox
/// directory changes (a vnode source), never by looking again on a timer.
///
/// Two roots, the first the host can reach: the app's Documents (house_arrest, UIFileSharingEnabled) and
/// /private/var/mobile/Media/Switch/benchloop (plain AFC, SwitchHWTest.entitlements' file exception).
final class BenchWorker: @unchecked Sendable {
    static let mediaRoot = URL(fileURLWithPath: "/private/var/mobile/Media/Switch/benchloop")

    private let queue = DispatchQueue(label: "com.brunostjohn.switch.hwtest.bench", qos: .userInitiated)
    private let logger = Logger(subsystem: "com.brunostjohn.switch.hwtest", category: "bench")
    private let roots: [URL]
    private let ios: Int
    private let publish: @Sendable (BenchStatus) -> Void
    private var status = BenchStatus()
    private var runner: BenchRunner?
    private var watchers: [DispatchSourceFileSystemObject] = []

    init(documents: URL, ios: Int, publish: @escaping @Sendable (BenchStatus) -> Void) {
        roots = [documents.appendingPathComponent("benchloop"), Self.mediaRoot]
        self.ios = ios
        self.publish = publish
    }

    func start() {
        queue.async { [self] in
            do {
                runner = try BenchRunner.open()
                status.device = runner!.summary
            } catch {
                status.device = "no raw device: \((error as? BenchInvalid)?.why ?? "\(error)")"
            }
            status.selfCheck = BenchSelfCheck.run(runner)
            logger.notice("BENCH selfcheck \(self.status.selfCheck, privacy: .public)")
            status.roots = roots.compactMap(attach)
            send()
            drain()
        }
    }

    func stop() {
        queue.async { [self] in
            watchers.forEach { $0.cancel() }
            watchers = []
            runner = nil
            for root in roots { try? FileManager.default.removeItem(at: root.appendingPathComponent("status.json")) }
        }
    }

    private func send() {
        let snapshot = status
        publish(snapshot)
    }

    private func attach(_ root: URL) -> String? {
        let inbox = root.appendingPathComponent("inbox")
        try? FileManager.default.createDirectory(at: inbox, withIntermediateDirectories: true)
        try? FileManager.default.createDirectory(
            at: root.appendingPathComponent("outbox"), withIntermediateDirectories: true)
        let fd = open(inbox.path, O_EVTONLY)
        guard fd >= 0 else {
            logger.error(
                "BENCH root \(root.path, privacy: .public) unreachable: \(String(cString: strerror(errno)), privacy: .public)"
            )
            return nil
        }
        let source = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write], queue: queue)
        source.setEventHandler { [weak self] in self?.drain() }
        source.setCancelHandler { close(fd) }
        source.resume()
        watchers.append(source)
        writeStatus(root)
        return root.path
    }

    private func writeStatus(_ root: URL) {
        let fields: [String: Any] = [
            "protocol": BenchProtocol.version, "ios": ios, "device": status.device, "selfcheck": status.selfCheck,
            "formats": BenchProtocol.formats.keys.sorted(), "root": root.path,
        ]
        if let data = try? JSONSerialization.data(withJSONObject: fields, options: [.sortedKeys]) {
            try? DurableFile.write(data + Data("\n".utf8), to: root.appendingPathComponent("status.json"))
        }
    }

    private func drain() {
        for root in roots {
            let inbox = root.appendingPathComponent("inbox")
            let files = (try? FileManager.default.contentsOfDirectory(atPath: inbox.path)) ?? []
            for name in files.sorted() where name.hasSuffix(".json") && !name.hasPrefix(".") {
                process(root, inbox.appendingPathComponent(name))
            }
        }
        status.current = "waiting for a batch"
        send()
    }

    private func process(_ root: URL, _ file: URL) {
        let fallback = file.deletingPathExtension().lastPathComponent
        let batch: BenchBatch
        do {
            batch = try BenchBatch.decode(try Data(contentsOf: file))
        } catch {
            let why = (error as? BenchInvalid)?.why ?? "\(error)"
            finish(root, file, name: BenchProtocol.validName(fallback) ? fallback : "invalid", summary: ["why": why])
            logger.error("BENCH \(fallback, privacy: .public) invalid why=\(LogLine.sanitized(why), privacy: .public)")
            return
        }
        let outbox = root.appendingPathComponent("outbox").appendingPathComponent(batch.name)
        // done.json means an earlier batch of this name finished; only an unfinished one is resumed
        if FileManager.default.fileExists(atPath: outbox.appendingPathComponent("done.json").path) {
            try? FileManager.default.removeItem(at: outbox)
        }
        try? FileManager.default.createDirectory(at: outbox, withIntermediateDirectories: true)
        let journal = outbox.appendingPathComponent("journal.txt")
        let earlier = (try? String(contentsOf: journal, encoding: .utf8)) ?? ""
        let crashed = Set(BenchJournal.crashed(earlier)), done = BenchJournal.finished(earlier)
        var counts: [String: Int] = [:]
        for (index, entry) in batch.entries.enumerated() where !done.contains(entry.id) {
            status.current = "\(batch.name) \(index + 1)/\(batch.entries.count): \(entry.id)"
            send()
            let outcome: BenchOutcome
            if crashed.contains(entry.id) {
                outcome = BenchOutcome(status: "crashed", why: "the app's process ended while it ran")
            } else {
                try? DurableFile.append(BenchJournal.starting(entry.id), to: journal)
                outcome = run(entry)
            }
            try? DurableFile.write(
                BenchResult.encode(batch: batch.name, entry: entry, outcome: outcome),
                to: outbox.appendingPathComponent(entry.id + ".json"))
            try? DurableFile.append(BenchJournal.done(entry.id, outcome.status), to: journal)
            counts[outcome.status, default: 0] += 1
            let line = BenchResult.logLine(batch: batch.name, id: entry.id, outcome: outcome)
            logger.notice("\(line, privacy: .public)")
            status.last = line
            status.finished += 1
        }
        finish(root, file, name: batch.name, summary: ["experiments": batch.entries.count, "statuses": counts])
    }

    private func run(_ entry: BenchBatch.Entry) -> BenchOutcome {
        switch entry {
        case .invalid(_, _, let why):
            return BenchOutcome(status: "invalid", why: why)
        case .experiment(let e):
            guard let runner else { return BenchOutcome(status: "error", why: status.device) }
            return runner.run(e)
        }
    }

    /// done.json, then the batch moved out of the inbox beside its results, so a later launch never runs it again
    private func finish(_ root: URL, _ file: URL, name: String, summary: [String: Any]) {
        let outbox = root.appendingPathComponent("outbox").appendingPathComponent(name)
        try? FileManager.default.createDirectory(at: outbox, withIntermediateDirectories: true)
        let fields = summary.merging(["protocol": BenchProtocol.version, "batch": name, "device": status.device]) {
            a, _ in a
        }
        if let data = try? JSONSerialization.data(withJSONObject: fields, options: [.sortedKeys]) {
            try? DurableFile.write(data + Data("\n".utf8), to: outbox.appendingPathComponent("done.json"))
        }
        let kept = outbox.appendingPathComponent("batch.json")
        try? FileManager.default.removeItem(at: kept)
        if (try? FileManager.default.moveItem(at: file, to: kept)) == nil {
            try? FileManager.default.removeItem(at: file)
        }
        logger.notice("BENCH \(name, privacy: .public) done")
    }
}

@MainActor
final class BenchLoop: ObservableObject {
    @Published private(set) var status = BenchStatus()
    private var worker: BenchWorker?

    func start(documents: URL, ios: Int) {
        guard worker == nil else { return }
        let worker = BenchWorker(documents: documents, ios: ios) { [weak self] status in
            Task { @MainActor in self?.status = status }
        }
        self.worker = worker
        worker.start()
    }

    func stop() {
        worker?.stop()
        worker = nil
    }
}
