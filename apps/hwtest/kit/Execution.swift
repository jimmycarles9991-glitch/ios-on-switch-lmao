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
        await context.clearStage()
        return record(test, outcome.withHarnessFaults, (CFAbsoluteTimeGetCurrent() - start) * 1000)
    }

    /// A GPU test that timed out gets GPUProbe's answer; gpu_answers=0 skips every later test that draws.
    nonisolated static func checked(_ test: any HWTest, _ record: TestRecord) async -> TestRecord {
        guard test.drawsWithGPU, record.outcome.metrics[timedOutKey] == .flag(true) else { return record }
        var outcome = record.outcome
        outcome.metrics.set(gpuAnswersKey, await GPUProbe.answers())
        return Suite.record(test, outcome, record.milliseconds)
    }

    nonisolated static func record(_ test: any HWTest, _ outcome: Outcome, _ milliseconds: Double) -> TestRecord {
        TestRecord(
            id: test.id, subsystem: test.subsystem.rawValue, name: test.name, interactive: test.interactive,
            outcome: outcome, milliseconds: milliseconds, finished: Date())
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
