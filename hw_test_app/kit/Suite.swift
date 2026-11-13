import Foundation
import HWTestCore
import UIKit
import os

/// Runs tests one at a time off the main thread (benchmarks never share the machine with each other) and sends each
/// result to the list, the log and the run's report (RunStore). The report is written before each test and after it:
/// when the app's process ends mid-run, its next launch offers to continue the saved run.
@MainActor
public final class Suite: ObservableObject {
    struct Row: Identifiable {
        let test: any HWTest
        var record: TestRecord?
        var running = false
        var started: Date?

        var id: String { test.id }
    }

    static let logSubsystem = "com.brunostjohn.switch.hwtest"

    @Published private(set) var rows: [Row]
    @Published private(set) var running = false
    @Published private(set) var report: Report?
    @Published private(set) var pendingRun: Report?
    @Published private(set) var resumeError: String?
    /// What this launch found of the last one, shown above the results.
    @Published private(set) var notice = ""

    let prompter = Prompter()
    let stage = Stage()
    let documents: URL
    private let tests: [any HWTest]
    let ios: Int
    private let store: RunStore
    private let logger = Logger(subsystem: Suite.logSubsystem, category: "results")
    private var launched = false
    private var wedgedBy: String?

    /// `ios`: the iOS major version the app is built for, which its summary line names so a log tells which
    /// version's tests ran.
    public init(tests: [any HWTest], ios: Int) {
        self.tests = tests
        self.ios = ios
        rows = tests.map { Row(test: $0) }
        documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        store = RunStore(documents: documents)
    }

    /// A boot's first launch runs everything; an unfinished run waits for the person's choice;
    /// any other launch shows this boot's last run until Run All.
    func launch() {
        guard !launched else { return }
        launched = true
        let boot = DeviceInfo.bootUUID
        let reports = store.reports()
        if let interrupted = Report.interrupted(reports) {
            show(interrupted)
            pendingRun = interrupted
        } else if let last = reports.last(where: { $0.latestBootUUID == boot }) {
            show(last)
        } else {
            runAll()
        }
    }

    func continueRun() {
        guard var continued = pendingRun, !running else { return }
        let boot = DeviceInfo.bootUUID
        let restarted = continued.latestBootUUID != boot
        let current = continued.run.current
        var crash: TestRecord?
        var nextNotice = ""
        if let current {
            let name = tests.first { $0.id == current }?.name ?? current
            if restarted {
                continued.requeueInterrupted()
                nextNotice = "The Switch restarted while \(name) ran. It will run last."
            } else if let reason = continued.run.interruption {
                continued.requeueInterrupted()
                nextNotice = "HW Test was \(reason) while \(name) ran. It will run again."
            } else {
                let record = crashRecord(current)
                continued.record(record)
                crash = record
                nextNotice =
                    "The app's process ended while \(name) ran. Recorded as a crash; continuing the remaining tests."
            }
        }
        continued.interrupt(nil)
        continued.resume(bootUUID: boot, at: Date(), identity: DeviceInfo.identity(ios: ios))
        let byID = Dictionary(tests.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        for id in continued.remaining where byID[id] == nil {
            continued.record(
                TestRecord(
                    id: id, subsystem: "", name: id, interactive: false,
                    outcome: .skip("test is not available in this build"), milliseconds: 0, finished: Date()))
        }
        var rest = continued.remaining.compactMap { byID[$0] }
        if restarted, let index = rest.firstIndex(where: { $0.id == current }) {
            rest.append(rest.remove(at: index))
        }
        if rest.isEmpty { continued.finish(at: Date()) }
        guard acceptRunChoice(continued) else { return }
        notice = nextNotice
        if let crash { logger.notice("\(LogLine.format(crash), privacy: .public)") }
        if rest.isEmpty { settle() } else { run(rest, summarize: true) }
    }

    func startNewRun() {
        guard var cancelled = pendingRun, !running else { return }
        cancelled.cancel(at: Date())
        guard acceptRunChoice(cancelled) else { return }
        runAll()
    }

    private func acceptRunChoice(_ chosen: Report) -> Bool {
        do {
            try store.save(chosen)
        } catch {
            resumeError = "Could not save the run. Try again."
            logger.error("Run choice not saved: \(String(describing: error), privacy: .public)")
            return false
        }
        pendingRun = nil
        resumeError = nil
        show(chosen)
        return true
    }

    /// The app going to the background or away while a run goes: written to the card at once, so the next launch
    /// tells a person's or the system's ending of the process from a crash.
    func interrupted(_ reason: String?) {
        guard running, report?.run.interruption != reason else { return }
        report?.interrupt(reason)
        save()
    }

    func runAll() {
        guard !running, pendingRun == nil else { return }
        wedgedBy = nil
        report = newReport()
        for index in rows.indices { rows[index].record = nil }
        var batch = tests.filter { !$0.interactive }
        notice = ""
        if let stopped = Report.lastBootStoppedIn(store.reports(), boot: DeviceInfo.bootUUID),
            let index = batch.firstIndex(where: { $0.id == stopped })
        {
            notice = "The last boot went down while \(batch[index].name) ran: it runs last this time."
            batch.append(batch.remove(at: index))
        }
        run(batch, summarize: true)
    }

    /// The tests that need a person, each behind a prompt that can be skipped, into the current run.
    func runInteractive() {
        run(tests.filter(\.interactive), summarize: false)
    }

    func rerun(_ id: String) {
        guard let test = tests.first(where: { $0.id == id }) else { return }
        run([test], summarize: false)
    }

    private func run(_ batch: [any HWTest], summarize: Bool) {
        guard !running, pendingRun == nil, !batch.isEmpty else { return }
        running = true
        UIApplication.shared.isIdleTimerDisabled = true
        if report == nil { report = newReport() }
        report?.plan(batch.map(\.id))
        save()
        let context = TestContext(
            media: Bundle.main.resourceURL!.appendingPathComponent("media"), documents: documents, ios: ios,
            prompter: prompter, stage: stage)
        Task.detached(priority: .userInitiated) { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            for test in batch {
                guard let self else { return }
                let record: TestRecord
                if let wedged = await self.begin(test.id), test.drawsWithGPU, !test.runsWhenGPUWedged {
                    record = Suite.record(test, .skip("the app's GPU stopped answering after \(wedged) timed out"), 0)
                } else if GlassMemWatch.measuresFrames(test.id), let left = GlassMemWatch.armedSeconds() {
                    record = Suite.record(test, .fail(GlassMemWatch.refusal(left)), 0)
                } else {
                    let executed = await Suite.execute(test, context)
                    record = await Suite.checked(test, executed)
                }
                if GlassMemWatch.armsTheProbe(test.id) { GlassMemWatch.endGlassTest() }
                await self.finish(record)
            }
            await self?.done(summarize: summarize, milliseconds: (CFAbsoluteTimeGetCurrent() - start) * 1000)
        }
    }

    /// Marks `id` running in the list and in the report on the card; returns the test that wedged the GPU, if one
    /// did.
    private func begin(_ id: String) -> String? {
        if let index = rows.firstIndex(where: { $0.id == id }) {
            rows[index].running = true
            rows[index].started = Date()
        }
        report?.begin(id)
        save()
        return wedgedBy
    }

    private func finish(_ record: TestRecord) {
        if let index = rows.firstIndex(where: { $0.id == record.id }) {
            rows[index].record = record
            rows[index].running = false
        }
        if record.outcome.metrics[Suite.gpuAnswersKey] == .flag(false) { wedgedBy = record.id }
        report?.record(record)
        logger.notice("\(LogLine.format(record), privacy: .public)")
        save()
    }

    private func done(summarize: Bool, milliseconds: Double) {
        running = false
        UIApplication.shared.isIdleTimerDisabled = false
        settle()
        if summarize {
            let records = rows.compactMap(\.record)
            logger.notice("\(LogLine.summary(records, ios: self.ios, milliseconds: milliseconds), privacy: .public)")
        }
    }

    /// Ends the run on the card once nothing it planned is left.
    private func settle() {
        if let report, report.remaining.isEmpty && report.run.current == nil { self.report?.finish(at: Date()) }
        save()
        store.prune()
    }

    private func show(_ shown: Report) {
        report = shown
        let entries = Dictionary(shown.results.map { ($0.id, $0) }, uniquingKeysWith: { $1 })
        for index in rows.indices {
            rows[index].record = entries[rows[index].id].map { entry in
                TestRecord(
                    id: entry.id, subsystem: entry.subsystem, name: entry.name, interactive: entry.interactive,
                    outcome: Outcome(entry), milliseconds: entry.milliseconds, finished: Date())
            }
        }
    }

    private func newReport() -> Report {
        Report(bootUUID: DeviceInfo.bootUUID, ios: ios, started: Date(), identity: DeviceInfo.identity(ios: ios))
    }

    private func crashRecord(_ id: String) -> TestRecord {
        guard let test = tests.first(where: { $0.id == id }) else {
            return TestRecord(
                id: id, subsystem: "", name: id, interactive: false, outcome: Report.crashOutcome(id), milliseconds: 0,
                finished: Date())
        }
        return Suite.record(test, Report.crashOutcome(id), 0)
    }

    private func save() {
        guard let report else { return }
        do {
            try store.save(report)
        } catch {
            logger.error(
                "\(report.fileName, privacy: .public) not written: \(String(describing: error), privacy: .public)")
        }
    }
}
