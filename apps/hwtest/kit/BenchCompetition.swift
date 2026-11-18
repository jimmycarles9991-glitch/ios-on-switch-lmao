import Foundation
import HWTestCore

/// The other channel of a `compete` experiment: the app's known program drawn 64 times over 1280x720, a command buffer
/// at a time, on a second bench device (its own kernel connection, GPU VM and channel: raw_gpu.cpp Device::open and
/// openChannel), from a thread of its own until the experiment ends.
final class BenchCompetition {
    private let runner: BenchRunner
    private let experiment: BenchExperiment
    private let lock = NSLock()
    private var stopping = false
    private var buffers = 0
    private var why = ""
    private let first = DispatchSemaphore(value: 0)
    private let finished = DispatchSemaphore(value: 0)

    init?(runner: BenchRunner) {
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "competitor",
            "experiments": [
                [
                    "id": "competitor", "kind": "known", "draws": [64], "readback": "none",
                    "target": ["format": "bgra8unorm", "width": 1280, "height": 720],
                ]
            ],
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: batch),
            let decoded = try? BenchBatch.decode(data), case .experiment(let e) = decoded.entries[0]
        else { return nil }
        (self.runner, experiment) = (runner, e)
    }

    /// Returns once the other channel has finished its first command buffer, so the experiment never starts alone
    func start() {
        let thread = Thread { [self] in
            var announced = false
            while !isStopping() {
                let outcome = runner.run(experiment)
                lock.lock()
                if outcome.status == "ok" {
                    buffers += 1
                } else {
                    (why, stopping) = ("\(outcome.status): \(outcome.why)", true)
                }
                lock.unlock()
                if !announced {
                    first.signal()
                    announced = true
                }
            }
            if !announced { first.signal() }
            finished.signal()
        }
        thread.qualityOfService = .userInitiated
        thread.start()
        _ = first.wait(timeout: .now() + 2)
    }

    /// Stops after the command buffer in flight: how many the other channel finished, and why it stopped early
    func stop() -> (buffers: Int, why: String) {
        lock.lock()
        stopping = true
        lock.unlock()
        finished.wait()
        lock.lock()
        defer { lock.unlock() }
        return (buffers, why)
    }

    private func isStopping() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return stopping
    }
}
