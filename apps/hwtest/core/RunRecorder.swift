import Foundation
import os

/// The runs on the card (RunStore, RunJournal), read and written only on one serial queue, in the order the calls
/// arrive: the runner's lines, the app's interruptions, a person's run choice and the launch's read of earlier runs.
/// JSON encoding and decoding and every F_FULLFSYNC happen on that queue. The main thread waits on it only for an
/// interruption's line, which must be written before a terminating app returns (no flush). Every other call is
/// awaited, and the caller keeps its own work ordered by awaiting it. The queue runs at the runner's priority because
/// the runner waits for it between tests.
public final class RunRecorder: @unchecked Sendable {
    private let store: RunStore
    private let log: Logger
    private let queue = DispatchQueue(label: "com.brunostjohn.switch.hwtest.journal", qos: .userInitiated)
    /// Only touched on `queue`.
    private var journal: RunJournal?

    /// `log`: where a line that could not be written is reported.
    public init(store: RunStore, log: Logger) {
        self.store = store
        self.log = log
    }

    /// This boot's runs and the last one of an earlier boot (RunStore.recent).
    public func recent(boot: String) async -> [Report] {
        await withCheckedContinuation { continuation in
            queue.async { [store] in continuation.resume(returning: store.recent(boot: boot)) }
        }
    }

    /// A run just continued has its journal already.
    public func start(_ report: Report) {
        queue.async { [self] in
            guard journal?.report != report else { return }
            attempt("\(report.fileName): journal not started") { journal = try store.open(report) }
        }
    }

    /// Returns once the lines are on the card, with the milliseconds that took.
    public func advance(_ finished: TestRecord?, begin next: String?) async -> Double {
        await withCheckedContinuation { continuation in
            queue.async { [self] in
                let start = CFAbsoluteTimeGetCurrent()
                attempt("journal line not written") { try journal?.advance(finished, begin: next) }
                continuation.resume(returning: (CFAbsoluteTimeGetCurrent() - start) * 1000)
            }
        }
    }

    /// Waits for the line: the app may be terminating, and the next launch tells a closed app from a crash by it.
    public func interrupt(_ reason: String?) {
        queue.sync {
            attempt("interruption not written") { try journal?.interrupt(reason) }
        }
    }

    /// Nil when the journal never started.
    public func finish(at date: Date) async -> Report? {
        await withCheckedContinuation { continuation in
            queue.async { [self] in
                guard let open = journal else { return continuation.resume(returning: nil) }
                if open.report.remaining.isEmpty && open.report.run.current == nil {
                    attempt("\(open.report.fileName): run not closed") {
                        try open.finish(at: date)
                        try store.close(open.report)
                    }
                    journal = nil
                    store.prune()
                }
                continuation.resume(returning: open.report)
            }
        }
    }

    /// On the card before this returns, so the run goes on only from a journal that holds it.
    public func checkpoint(_ continued: Report) async throws {
        try await onQueue { [self] in journal = try store.open(continued) }
    }

    public func close(_ ended: Report) async throws {
        try await onQueue { [self] in
            journal = nil
            try store.close(ended)
            store.prune()
        }
    }

    private func onQueue(_ work: @escaping () throws -> Void) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async {
                do {
                    try work()
                    continuation.resume()
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    private func attempt(_ what: String, _ work: () throws -> Void) {
        do {
            try work()
        } catch {
            log.error("\(what, privacy: .public): \(String(describing: error), privacy: .public)")
        }
    }
}
