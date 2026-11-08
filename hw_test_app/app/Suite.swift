import Foundation
import HWTestCore
import os

/// Runs tests one at a time off the main thread (benchmarks never share the machine with each other) and sends each
/// result to the list, the log and report.json.
@MainActor
final class Suite: ObservableObject {
    struct Row: Identifiable {
        let test: any HWTest
        var record: TestRecord?
        var running = false

        var id: String { test.id }
    }

    static let logSubsystem = "com.brunostjohn.switch.hwtest"

    @Published private(set) var rows: [Row]
    @Published private(set) var running = false
    @Published private(set) var summary = ""

    let prompter = Prompter()
    let stage = Stage()
    let documents: URL
    private let tests: [any HWTest]
    private let started = Date()
    private let logger = Logger(subsystem: Suite.logSubsystem, category: "results")
    private var records: [TestRecord] = []
    private var autoRan = false

    init(tests: [any HWTest]) {
        self.tests = tests
        rows = tests.map { Row(test: $0) }
        documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    var reportURL: URL { documents.appendingPathComponent("report.json") }

    func autoRunOnce() {
        guard !autoRan else { return }
        autoRan = true
        runAll()
    }

    func runAll() {
        run(tests.filter { !$0.interactive } + tests.filter(\.interactive), summarize: true)
    }

    func rerun(_ id: String) {
        guard let test = tests.first(where: { $0.id == id }) else { return }
        run([test], summarize: false)
    }

    private func run(_ batch: [any HWTest], summarize: Bool) {
        guard !running else { return }
        running = true
        let context = TestContext(
            media: Bundle.main.resourceURL!.appendingPathComponent("media"), documents: documents, prompter: prompter,
            stage: stage)
        Task.detached(priority: .userInitiated) { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            for test in batch {
                await self?.mark(test.id, running: true)
                let record = await Suite.execute(test, context)
                await self?.finish(record)
            }
            await self?.done(summarize: summarize, milliseconds: (CFAbsoluteTimeGetCurrent() - start) * 1000)
        }
    }

    nonisolated static func execute(_ test: any HWTest, _ context: TestContext) async -> TestRecord {
        let start = CFAbsoluteTimeGetCurrent()
        let outcome: Outcome
        do {
            outcome = try await withTimeout(test.timeout) { try await test.run(context) }
        } catch {
            outcome = .fail(String(describing: error))
        }
        await context.clearStage()
        return TestRecord(
            id: test.id, subsystem: test.subsystem.rawValue, name: test.name, interactive: test.interactive,
            outcome: outcome, milliseconds: (CFAbsoluteTimeGetCurrent() - start) * 1000, finished: Date())
    }

    /// A hung test cannot be stopped from outside, so the suite stops waiting for it instead: the FAIL names the
    /// timeout and the next test starts.
    nonisolated static func withTimeout(
        _ seconds: TimeInterval, _ operation: @escaping @Sendable () async throws -> Outcome
    ) async throws -> Outcome {
        let waiter = Waiter<Result<Outcome, Error>>()
        let task = Task {
            do {
                waiter.resume(.success(try await operation()))
            } catch {
                waiter.resume(.failure(error))
            }
        }
        guard let result = await waiter.wait(timeout: seconds) else {
            task.cancel()
            throw TestFailure("timed out after \(Int(seconds)) s")
        }
        return try result.get()
    }

    private func mark(_ id: String, running: Bool) {
        if let index = rows.firstIndex(where: { $0.id == id }) { rows[index].running = running }
    }

    private func finish(_ record: TestRecord) {
        if let index = rows.firstIndex(where: { $0.id == record.id }) {
            rows[index].record = record
            rows[index].running = false
        }
        records.append(record)
        logger.notice("\(LogLine.format(record), privacy: .public)")
        writeReport()
    }

    private func done(summarize: Bool, milliseconds: Double) {
        running = false
        let latest = rows.compactMap(\.record)
        let line = LogLine.summary(latest, milliseconds: milliseconds)
        if summarize { logger.notice("\(line, privacy: .public)") }
        summary = line
    }

    private func writeReport() {
        do {
            try Report(started: started, device: DeviceInfo.current(), records: records).encoded()
                .write(to: reportURL, options: .atomic)
        } catch {
            logger.error("report.json not written: \(String(describing: error), privacy: .public)")
        }
    }
}
