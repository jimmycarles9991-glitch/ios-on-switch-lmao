import Foundation
import HWTestCore

struct TimedOut: Error, CustomStringConvertible {
    let seconds: TimeInterval

    var description: String { "timed out after \(Int(seconds)) s" }
}

extension Suite {
    /// On a test that timed out.
    nonisolated static let timedOutKey = "timed_out"
    /// On a GPU test that timed out: whether GPUProbe's fill on a fresh queue then completed.
    nonisolated static let gpuAnswersKey = "gpu_answers"

    /// The line goes out as the measured span ends, so the line's time less its ms= is when the test started; only a
    /// GPU test that timed out waits for GPUProbe first, and the thread sample its CPU split is taken from (since
    /// `earlier`, the last test's) comes before it, outside the span.
    nonisolated static func outcome(
        of test: any HWTest, _ context: TestContext, wedgedBy wedged: String?, cpuSince earlier: ThreadTimes
    ) async -> (record: TestRecord, cpu: ThreadTimes, sampleMs: Double) {
        var record: TestRecord
        if let wedged, test.drawsWithGPU, !test.runsWhenGPUWedged {
            record = Suite.record(test, .skip("the app's GPU stopped answering after \(wedged) timed out"), 0)
        } else {
            record = await execute(test, context)
            if probes(test, record) {
                await context.clearStage()
                record = await checked(test, record)
            }
        }
        let sampled = CFAbsoluteTimeGetCurrent()
        let cpu = ThreadTimes.sample(main: earlier.main)
        let sampleMs = (CFAbsoluteTimeGetCurrent() - sampled) * 1000
        var outcome = record.outcome
        outcome.metrics.set(CPUSplit.key, cpu.split(since: earlier).field)
        if TestBudget.over(record.milliseconds, interactive: test.interactive) { outcome.metrics.set(
            TestBudget.key,
            true
        ) }
        record = Suite.record(test, outcome, record.milliseconds)
        log(LogLine.format(record))
        return (record, cpu, sampleMs)
    }

    /// The test's own span only: the stage it leaves is cleared between tests.
    nonisolated static func execute(_ test: any HWTest, _ context: TestContext) async -> TestRecord {
        let start = CFAbsoluteTimeGetCurrent()
        let outcome: Outcome
        do {
            outcome = try await withTimeout(test.timeout) { try await test.run(context) }
        } catch let timeout as TimedOut {
            var metrics = Metrics()
            metrics.set(timedOutKey, true)
            outcome = .fail(timeout.description, metrics)
        } catch {
            outcome = .fail(String(describing: error))
        }
        return record(test, outcome.withHarnessFaults, (CFAbsoluteTimeGetCurrent() - start) * 1000)
    }

    nonisolated static func probes(_ test: any HWTest, _ record: TestRecord) -> Bool {
        test.drawsWithGPU && record.outcome.metrics[timedOutKey] == .flag(true)
    }

    /// A GPU test that timed out gets GPUProbe's answer; gpu_answers=0 skips every later test that draws.
    nonisolated static func checked(_ test: any HWTest, _ record: TestRecord) async -> TestRecord {
        var outcome = record.outcome
        outcome.metrics.set(gpuAnswersKey, await GPUProbe.answers())
        return Suite.record(test, outcome, record.milliseconds)
    }

    nonisolated static func record(_ test: any HWTest, _ outcome: Outcome, _ milliseconds: Double) -> TestRecord {
        TestRecord(
            id: test.id, subsystem: test.subsystem.rawValue, name: test.name, interactive: test.interactive,
            outcome: outcome, milliseconds: milliseconds, finished: Date()
        )
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
            throw TimedOut(seconds: seconds)
        }
        return try result.get()
    }
}
