import Darwin
import Foundation
import GameController
import HWTestCore

/// The Joy-Con rails' transport for a few seconds with nobody touching anything, from the rail driver's counters
/// (kern.switch_joycon, debug kernels). Each running rail asks its Joy-Con for a report every 15 ms, so its replies
/// show how many requests went unanswered; in the window no packet may be dropped, no Joy-Con may fall silent or be
/// published again, RX DMA has to stay on, and no game controller may disconnect.
struct RailTest: HWTest {
    let id = "input.rail"
    let name = "Joy-Con rails: replies, drops and resets over 5 s"
    let subsystem = Subsystem.input

    static let seconds = 5.0
    static let requestsPerSecond = 1000.0 / 15.0
    static let resets = ["silences", "recoveries", "republished"]

    struct Rail {
        let running: Bool
        let dma: Bool
        let counters: [String: Int]

        func delta(_ name: String, since before: Rail) -> Int {
            (counters[name] ?? 0) - (before.counters[name] ?? 0)
        }
    }

    /// kern.switch_joycon by rail letter: "R running=1 dma=on replies=… ; L …". Nil on a kernel without it.
    static func read() -> [String: Rail]? {
        var size = 0
        guard sysctlbyname("kern.switch_joycon", nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var bytes = [CChar](repeating: 0, count: size)
        guard sysctlbyname("kern.switch_joycon", &bytes, &size, nil, 0) == 0 else { return nil }
        var rails: [String: Rail] = [:]
        for entry in String(cString: bytes).components(separatedBy: "; ") {
            let fields = entry.split(separator: " ")
            guard let letter = fields.first else { continue }
            var values: [String: String] = [:]
            for field in fields.dropFirst() {
                let pair = field.split(separator: "=", maxSplits: 1)
                if pair.count == 2 { values[String(pair[0])] = String(pair[1]) }
            }
            rails[String(letter)] = Rail(
                running: values["running"] == "1", dma: values["dma"] == "on",
                counters: values.compactMapValues { Int($0) })
        }
        return rails
    }

    final class Disconnects {
        var count = 0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let before = Self.read() else { return .skip("the kernel has no kern.switch_joycon (a release kernel)") }
        let disconnects = Disconnects()
        let observer = NotificationCenter.default.addObserver(
            forName: .GCControllerDidDisconnect, object: nil, queue: .main
        ) { _ in disconnects.count += 1 }
        let start = Date()
        try await Task.sleep(nanoseconds: UInt64(Self.seconds * 1e9))
        let elapsed = Date().timeIntervalSince(start)
        NotificationCenter.default.removeObserver(observer)
        let disconnected = await MainActor.run { disconnects.count }
        guard let after = Self.read() else { return .fail("kern.switch_joycon went away during the test") }
        let rails = after.keys.sorted().filter { after[$0]?.running == true && before[$0]?.running == true }
        guard !rails.isEmpty else { return .skip("no Joy-Con running on a rail for the whole window") }

        var metrics = Metrics()
        metrics.set("rails", rails.joined(separator: ","))
        metrics.set("disconnects", disconnected)
        var answered = true, dropped = 0, resets = 0, dmaOn = true
        for letter in rails {
            guard let now = after[letter], let then = before[letter] else { continue }
            let rate = Double(now.delta("replies", since: then)) / elapsed
            metrics.set("\(letter)_replies_hz", rate, digits: 1)
            for name in ["late", "tails", "errors", "breaks", "overflows", "dropped"] + Self.resets {
                metrics.set("\(letter)_\(name)", now.delta(name, since: then))
            }
            answered = answered && rate >= 0.9 * Self.requestsPerSecond
            dropped += now.delta("dropped", since: then) + now.delta("overflows", since: then)
            resets += Self.resets.map { now.delta($0, since: then) }.reduce(0, +)
            dmaOn = dmaOn && now.dma
        }
        return .judged(
            [
                ("each rail answered at least 90% of its requests", answered),
                ("no rail packet dropped or overflowed", dropped == 0),
                ("no Joy-Con fell silent or was published again", resets == 0),
                ("RX DMA stayed on", dmaOn),
                ("no game controller disconnected", disconnected == 0),
            ],
            metrics
        )
    }
}
