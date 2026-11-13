import Darwin
import Foundation
import HWTestCore

/// What a rail Joy-Con does when the rail stops asking (debug kernels): writing debug.switch_joycon_probe runs the
/// stages of SwitchJoyConProbe.h on every running Joy-Con, about nine seconds, and reading it gives each stage's
/// answer, the packets the Joy-Con sent unasked, and whether it kept HID mode. Each finished stage is also one kernel
/// log line ("SwitchJoyCon: probe Joy-Con (L) rate30_answered=1 rate30_status=1 ..."). In the "simple" stage (input
/// report mode 0x3F) the test asks for button presses: a Joy-Con that pushes reports on a press shows them as
/// simple_unasked. The verdict is only that the probe ran and the rail is running again; the answers are the metrics.
struct JoyConProbeTest: HWTest {
    let id = "input.joycon_probe"
    let name = "Joy-Con rail protocol probe: unasked reports, report intervals, 0x3F on the rail"
    let subsystem = Subsystem.input
    let interactive = true

    static let sysctl = "debug.switch_joycon_probe"
    static let stages = [
        "silence", "rate15", "rate5", "rate30", "rate100", "getrate", "wake", "restore", "simple", "full",
    ]
    /// Each stage's metric, one letter per field: a answered, s status, d answer data, u packets unasked, f the first
    /// of them (ms into the phase, -1 none), p of them 0x3F reports, k keepalives answered, of keepalives sent,
    /// r resumed (the 1F after the window was answered).
    static let fields: [(tag: String, name: String)] = [
        ("a", "answered"), ("s", "status"), ("d", "data"), ("u", "unasked"), ("f", "first_ms"), ("p", "simple"),
        ("k", "kept"), ("of", "keepalives"), ("r", "resumed"),
    ]

    static func start() -> Int32 {
        var one: [CChar] = Array("1".utf8CString)
        return sysctlbyname(sysctl, nil, nil, &one, one.count) == 0 ? 0 : errno
    }

    static func state() -> String {
        if case .value(let text) = KernelSysctl.string(sysctl) { return text }
        return ""
    }

    static func waitWhile(seconds: Double, _ condition: (String) -> Bool) async throws -> String {
        let deadline = Date().addingTimeInterval(seconds)
        var text = state()
        while condition(text) && Date() < deadline {
            try await Task.sleep(nanoseconds: 50_000_000)
            text = state()
        }
        return text
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let error = Self.start()
        guard error == 0 else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(error))
            if error == ENODEV { return .skip("no Joy-Con running on a rail", metrics) }
            if error == EBUSY { return .fail("a probe was already running", metrics) }
            return .skip(KernelSysctl.unreadable(Self.sysctl, error), metrics)
        }
        _ = try await Self.waitWhile(seconds: 15) { $0.hasPrefix("running") && !$0.contains("stage=simple") }
        let pressed: Bool? = await context.interact(
            "Press and release a button on a rail Joy-Con a few times, until this goes away", timeout: 15
        ) { waiter in
            Task {
                _ = try? await Self.waitWhile(seconds: 15) { $0.contains("stage=simple") }
                waiter.resume(true)
            }
        }
        let text = try await Self.waitWhile(seconds: 15) { $0.hasPrefix("running") }
        try await Task.sleep(nanoseconds: 3_000_000_000)
        var metrics = Metrics()
        metrics.set("prompted", pressed == true)
        let entries = text.components(separatedBy: "; ")
        let rails = entries.dropFirst().compactMap { entry -> String? in
            let fields = entry.split(separator: " ")
            guard let letter = fields.first?.lowercased() else { return nil }
            var values: [String: String] = [:]
            for field in fields.dropFirst() {
                let pair = field.split(separator: "=", maxSplits: 1)
                if pair.count == 2 { values[String(pair[0])] = String(pair[1]) }
            }
            for stage in Self.stages {
                let summary = Self.fields.map { "\($0.tag)\(values["\(stage)_\($0.name)"] ?? "?")" }.joined()
                metrics.set("\(letter)_\(stage)", summary)
            }
            return letter
        }
        metrics.set("rails", rails.joined(separator: ","))
        var running = false
        if case .value(let after) = RailTest.read() {
            running = after.values.contains { $0.running }
        }
        return .judged(
            [
                ("the probe finished", entries.first == "done" && !rails.isEmpty),
                ("a Joy-Con runs on a rail again after it", running),
            ],
            metrics
        )
    }
}
