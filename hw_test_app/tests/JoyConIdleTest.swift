import Darwin
import Foundation
import GameController
import HWTestCore

/// The Joy-Con rails left alone, from the rail driver's counters (kern.switch_joycon, debug kernels): a Joy-Con with
/// no input for a second is asked every 30 ms instead of 15 (SwitchJoyConCadence.h), each request costs two UART
/// interrupts, its reply's EORDs after the reply's header pause and at its end (the request fits the TX FIFO and takes
/// no THR-empty interrupt), the RX DMA channel keeps up with every reply, and no report reaches its HID device
/// (SwitchJoyConForward.h). Nobody may touch the pads.
struct JoyConIdleTest: HWTest {
    let id = "input.joycon_idle"
    let name = "Joy-Con rails left alone: a request every 30 ms, two interrupts each, nothing forwarded"
    let subsystem = Subsystem.input

    static let settle = 1.5
    static let seconds = 5.0
    static let requestsPerSecond = 1000.0 / 30.0
    static let interruptsPerRequest = 2.0

    func run(_ context: TestContext) async throws -> Outcome {
        let first = RailTest.read()
        guard case .value = first else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(first.error))
            return .skip(KernelSysctl.unreadable("kern.switch_joycon", first.error), metrics)
        }
        try await Task.sleep(nanoseconds: UInt64(Self.settle * 1e9))
        guard case .value(let before) = RailTest.read() else {
            return .fail("kern.switch_joycon went away during the test")
        }
        let disconnects = RailTest.Disconnects()
        let observer = NotificationCenter.default.addObserver(
            forName: .GCControllerDidDisconnect, object: nil, queue: .main
        ) { _ in disconnects.count += 1 }
        let start = Date()
        try await Task.sleep(nanoseconds: UInt64(Self.seconds * 1e9))
        let elapsed = Date().timeIntervalSince(start)
        NotificationCenter.default.removeObserver(observer)
        let disconnected = await MainActor.run { disconnects.count }
        guard case .value(let after) = RailTest.read() else {
            return .fail("kern.switch_joycon went away during the test")
        }
        let rails = after.keys.sorted().filter { after[$0]?.running == true && before[$0]?.running == true }
        guard !rails.isEmpty else { return .skip("no Joy-Con running on a rail for the whole window") }

        var metrics = Metrics()
        metrics.set("rails", rails.joined(separator: ","))
        metrics.set("disconnects", disconnected)
        var paced = true, single = true, forwarded = 0, tails = 0, quiet = true, dmaOn = true
        for letter in rails {
            guard let now = after[letter], let then = before[letter] else { continue }
            let rail = letter.lowercased()
            let requests = now.delta("requests", since: then)
            let rate = Double(requests) / elapsed
            let perRequest = Double(now.delta("interrupts", since: then)) / Double(max(requests, 1))
            metrics.set("\(rail)_requests_hz", rate, digits: 1)
            metrics.set("\(rail)_interrupts_per_request", perRequest, digits: 2)
            metrics.set("\(rail)_replies_hz", Double(now.delta("replies", since: then)) / elapsed, digits: 1)
            metrics.set("\(rail)_suppressed_hz", Double(now.delta("suppressed", since: then)) / elapsed, digits: 1)
            for name in ["forwarded", "unsolicited", "tails", "late"] {
                metrics.set("\(rail)_\(name)", now.delta(name, since: then))
            }
            metrics.set("\(rail)_deadzone", now.counters["deadzone"] ?? 0)
            metrics.set("\(rail)_calibrated", now.counters["calibrated"] ?? 0)
            metrics.set("\(rail)_gamepad", now.counters["gamepad"] ?? 0)
            paced = paced && abs(rate - Self.requestsPerSecond) <= 0.15 * Self.requestsPerSecond
            single = single && perRequest <= Self.interruptsPerRequest + 0.1
            forwarded += now.delta("forwarded", since: then)
            tails += now.delta("tails", since: then)
            quiet = quiet && now.counters["quiet"] == 1
            dmaOn = dmaOn && now.dma
        }
        return .judged(
            [
                ("each rail was asked about every 30 ms", paced),
                ("at most two UART interrupts per request", single),
                ("no report reached a HID device", forwarded == 0),
                ("every reply was whole at its interrupt (no tail left to a tick)", tails == 0),
                ("each rail stayed quiet", quiet),
                ("RX DMA stayed on", dmaOn),
                ("no game controller disconnected", disconnected == 0),
            ],
            metrics
        )
    }
}
