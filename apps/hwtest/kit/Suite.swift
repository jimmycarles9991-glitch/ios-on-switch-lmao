import Foundation
import HWTestCore
import os
import UIKit

/// Runs tests one at a time off the main thread (benchmarks never share the machine with each other) and sends each
/// result to the log, the run's journal on the card (RunRecorder) and its row in the list. Between two tests the
/// runner logs the result, updates the list, flushes the journal and waits for the main thread to answer, all
/// before the next test's clock starts: when the app's process ends mid-run, its next launch offers to continue.
@MainActor
public final class Suite: ObservableObject {
    nonisolated static let logSubsystem = "com.brunostjohn.switch.hwtest"
    nonisolated static let results = Logger(subsystem: Suite.logSubsystem, category: "results")

    let list: RunList
    @Published private(set) var running = false
    @Published private(set) var session = RunSession()
    @Published private(set) var resumeError: String?
    /// A run choice being saved, or the launch's or Run All's read of earlier runs, is on the recorder's queue: the
    /// controls that start a run wait for it.
    @Published private(set) var busy = false

    /// The run shown: as it stood when its batch started while one runs, as it ended after.
    var report: Report? { session.report }
    var reportSaved: Bool { session.reportSaved }
    var pendingRun: Report? { session.pendingRun }
    var resumePromptShown: Bool { session.resumePromptShown }
    /// Nothing runs, waits for the resume sheet or waits for the card: a run may start.
    var idle: Bool { !running && !busy && !resumePromptShown }
    /// What this launch found of the last one, shown above the results.
    @Published private(set) var notice = ""

    let prompter = Prompter()
    let stage = Stage()
    let documents: URL
    private let tests: [any HWTest]
    let ios: Int
    private let recorder: RunRecorder
    private var launched = false
    private var recording = false
    private var wedgedBy: String?

    /// `ios`: the iOS major version the app is built for, which its summary line names so a log tells which
    /// version's tests ran.
    public init(tests: [any HWTest], ios: Int) {
        self.tests = tests
        self.ios = ios
        list = RunList(tests: tests)
        documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        recorder = RunRecorder(store: RunStore(directory: AppFiles.working), log: Suite.results)
    }

    /// An unfinished run opens the resume sheet; a finished run from this boot is shown; otherwise stays idle until
    /// started.
    func launch() {
        guard !launched else { return }
        launched = true
        busy = true
        let boot = DeviceInfo.bootUUID
        Task {
            let recent = await recorder.recent(boot: boot)
            busy = false
            switch session.applyLaunch(recent: recent, boot: boot) {
            case .empty:
                break
            case .showLast(let last):
                populateRows(from: last)
            case .promptUnfinished(let interrupted):
                populateRows(from: interrupted)
            }
        }
    }

    func dismissPendingRun() {
        guard !busy else { return }
        session.dismissResumePrompt()
        resumeError = nil
    }

    func continueRun() {
        guard let pending = pendingRun, !running, !busy else { return }
        let byID = Dictionary(tests.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        let continued = pending.continued(
            boot: DeviceInfo.bootUUID, at: Date(), identity: DeviceInfo.identity(ios: ios), available: Set(byID.keys)
        ) { id, outcome in
            byID[id].map { Suite.record($0, outcome, 0) }
                ?? TestRecord(
                    id: id, subsystem: "", name: id, interactive: false, outcome: outcome, milliseconds: 0,
                    finished: Date()
                )
        }
        busy = true
        Task {
            let saved = await acceptRunChoice(continued.report, ended: continued.order.isEmpty)
            busy = false
            guard saved else { return }
            notice = Self.continuationNotice(continued.cause) { id in byID[id]?.name ?? id }
            if let crash = continued.crash { Self.log(LogLine.format(crash)) }
            run(continued.order.compactMap { byID[$0] }, summarize: true)
        }
    }

    func startNewRun() {
        guard var cancelled = pendingRun, !running, !busy else { return }
        cancelled.cancel(at: Date())
        busy = true
        Task {
            let saved = await acceptRunChoice(cancelled, ended: true)
            busy = false
            if saved { runAll() }
        }
    }

    /// Saved before the run goes on: a run that continued from a choice the card never got would be offered again. The
    /// caller holds `busy` from the tap until this returns.
    private func acceptRunChoice(_ chosen: Report, ended: Bool) async -> Bool {
        do {
            if ended { try await recorder.close(chosen) } else { try await recorder.checkpoint(chosen) }
        } catch {
            resumeError = "Could not save the run. Try again."
            Self.results.error("Run choice not saved: \(String(describing: error), privacy: .public)")
            return false
        }
        session.acceptPendingChoice(chosen)
        resumeError = nil
        populateRows(from: chosen)
        return true
    }

    /// The app going to the background or away while a run goes, for the next launch to tell a person's or the
    /// system's ending of the process from a crash.
    func interrupted(_ reason: String?) {
        guard running, recording else { return }
        recorder.interrupt(reason)
    }

    func requestRunAll() {
        guard idle else { return }
        switch session.requestRunAll() {
        case .promptUnfinished:
            break
        case .startFresh:
            runAll()
        }
    }

    func runAll() {
        guard !running, !busy, pendingRun == nil else { return }
        busy = true
        let boot = DeviceInfo.bootUUID
        Task {
            let recent = await recorder.recent(boot: boot)
            busy = false
            runAll(stoppedIn: Report.lastBootStoppedIn(recent, boot: boot))
        }
    }

    /// `stoppedIn`: the test the last boot went down in, which runs last.
    private func runAll(stoppedIn stopped: String?) {
        guard !running, !busy, pendingRun == nil else { return }
        wedgedBy = nil
        session.beginFreshRun(newReport())
        for row in list.rows { row.record = nil }
        var batch = tests.filter { !$0.interactive }
        notice = ""
        if let stopped, let index = batch.firstIndex(where: { $0.id == stopped }) {
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
        guard idle, !batch.isEmpty else { return }
        running = true
        UIApplication.shared.isIdleTimerDisabled = true
        let target = session.planBatch(batch.map(\.id)) { newReport() }
        let planned = target.report
        let persists = target.persists
        recording = persists
        if persists { recorder.start(planned) }
        list.seed(planned)
        let context = TestContext(
            media: Bundle.main.resourceURL!.appendingPathComponent("media"), documents: documents, ios: ios,
            prompter: prompter, stage: stage
        )
        let recorder = recorder
        let main = ThreadTimes.current()
        let mainPort = pthread_mach_thread_np(pthread_self())
        Task.detached(priority: .userInitiated) { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            var harness = HarnessTally()
            harness.longRuns = LongRuns.on
            var ephemeral = persists ? nil : planned
            var finished: TestRecord?
            var ended: Double?
            var cpu = ThreadTimes.sample(main: main)
            for test in batch {
                guard let self else { return }
                let mainBefore = ThreadTimes.nanoseconds(of: mainPort)
                let wedged = await self.advance(finished, next: test.id)
                let flush: Double = persists ? await recorder.advance(finished, begin: test.id) : 0
                ephemeral = Suite.advanceEphemeral(ephemeral, finished: finished, next: test.id)
                let mainWait = await Suite.mainThreadAnswer()
                let begun = CFAbsoluteTimeGetCurrent()
                if let ended {
                    harness.add(
                        boundaryMs: (begun - ended) * 1000, flushMs: flush, mainWaitMs: mainWait,
                        mainCpuMs: Suite.cpuMs(of: mainPort, since: mainBefore)
                    )
                }
                let ran = await Suite.outcome(of: test, context, wedgedBy: wedged, cpuSince: cpu)
                harness.add(cpuSampleMs: ran.sampleMs)
                if TestBudget.over(ran.record.milliseconds, interactive: test.interactive) {
                    harness.add(overBudget: test.id)
                }
                cpu = ran.cpu
                ended = begun + ran.record.milliseconds / 1000
                finished = ran.record
            }
            guard let self else { return }
            _ = await self.advance(finished, next: nil)
            if persists { _ = await recorder.advance(finished, begin: nil) }
            let final: Report? = persists
                ? await recorder.finish(at: Date())
                : Suite.advanceEphemeral(ephemeral, finished: finished, next: nil)
            await self.done(
                final, summarize: summarize, milliseconds: (CFAbsoluteTimeGetCurrent() - start) * 1000, harness: harness
            )
        }
    }

    private nonisolated static func advanceEphemeral(
        _ report: Report?, finished: TestRecord?, next: String?
    ) -> Report? {
        guard var report else { return nil }
        if let finished { report.record(finished) }
        if let next { report.begin(next) } else { report.finish(at: Date()) }
        return report
    }

    /// Returns the test that wedged the GPU, if one did.
    private func advance(_ finished: TestRecord?, next: String?) -> String? {
        stage.dismiss()
        if let finished, finished.outcome.metrics[Suite.gpuAnswersKey] == .flag(false) { wedgedBy = finished.id }
        list.advance(finished, next: next)
        return wedgedBy
    }

    nonisolated static func cpuMs(of port: thread_act_t, since before: UInt64?) -> Double? {
        guard let before, let now = ThreadTimes.nanoseconds(of: port) else { return nil }
        return Double(now - min(before, now)) / 1e6
    }

    nonisolated static func mainThreadAnswer() async -> Double {
        let asked = CFAbsoluteTimeGetCurrent()
        _ = await MainActor.run { 0 }
        return (CFAbsoluteTimeGetCurrent() - asked) * 1000
    }

    private func done(_ final: Report?, summarize: Bool, milliseconds: Double, harness: HarnessTally) {
        running = false
        recording = false
        UIApplication.shared.isIdleTimerDisabled = false
        session.finishBatch(final)
        guard summarize else { return }
        let ran = Dictionary(
            (report?.results ?? []).enumerated().map { ($1.id, $0) }, uniquingKeysWith: { first, _ in first }
        )
        let records = list.rows.compactMap(\.record).sorted { (ran[$0.id] ?? .max) < (ran[$1.id] ?? .max) }
        let span = report?.milliseconds(at: Date()) ?? milliseconds
        Self.log(LogLine.rollUp(records, milliseconds: span))
        Self.log(harness.line)
        Self.log(LogLine.summary(records, ios: ios, milliseconds: span))
    }

    nonisolated static func log(_ line: String) {
        for piece in LogLine.pieces(line) { results.notice("\(piece, privacy: .public)") }
    }

    private func populateRows(from shown: Report) {
        let entries = Dictionary(shown.results.map { ($0.id, $0) }, uniquingKeysWith: { $1 })
        for row in list.rows {
            row.record = entries[row.id].map { entry in
                TestRecord(
                    id: entry.id, subsystem: entry.subsystem, name: entry.name, interactive: entry.interactive,
                    outcome: Outcome(entry), milliseconds: entry.milliseconds, finished: Date()
                )
            }
        }
    }

    private func newReport() -> Report {
        Report(bootUUID: DeviceInfo.bootUUID, ios: ios, started: Date(), identity: DeviceInfo.identity(ios: ios))
    }

    private static func continuationNotice(_ cause: Continuation.Cause, name: (String) -> String) -> String {
        switch cause {
        case .none: return ""
        case .restarted(let id): return "The Switch restarted while \(name(id)) ran. It will run last."
        case .interrupted(let id, let reason): return "HW Test was \(reason) while \(name(id)) ran. It will run again."
        case .crashed(let id):
            return "The app's process ended while \(name(id)) ran. Recorded as a crash; continuing the remaining tests."
        }
    }
}
