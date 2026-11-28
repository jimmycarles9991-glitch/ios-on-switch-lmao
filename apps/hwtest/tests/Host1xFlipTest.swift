import Foundation
import HWTestCommon
import HWTestCore
import HWTestKit
import HWTestShared

/// Window A flipped by host1x instead of the CPU (drivers/NativeMobileDisplay/SwitchHost1xFlipDisplay.h, debug
/// kernels): each flip is a class 0x70 stream on the display's own host1x channel, held behind its producer's syncpoint
/// (the kernel stands in for the producer), and judged by the display's latch check at the next frame end. The two
/// surfaces are the same pages at two addresses, so the panel does not change. The teardown arm fails the middle
/// flip's producer: the channel is torn down before that stream can write, the display's work loop writes the flip,
/// and the restarted channel carries the rest. d_host1x_flips is kern.switch_flips_host1x across the run. Runs after
/// the other display tests: it holds the swaps for about a second.
///
/// Two probes (host1x items 2 and 4) are judged on the run alone; what they measure is reported. window_syncpt: each
/// stream also raises window A's REG_WR_SAFE and LINE_FLIPPED syncpoints, counted before/at/later/never against the
/// frame end that saw the flip latched. vblank: each stream waits two vblanks on syncpoint 9, and the vblank count at
/// the latch less the one waited for is counted below/0/1/2+.
struct Host1xFlipTest: HWTest {
    let id: String
    let name: String
    let flips: Int
    let forced: Bool
    var modes: Int32 = 0
    let subsystem = Subsystem.display
    var timeout: TimeInterval { 30 }

    static let sysctl = "debug.switch_display_host1x_flip"
    static let counter = "kern.switch_flips_host1x"
    static let forcedFailure: Int32 = 1 << 16
    static let windowIncrements: Int32 = 1 << 17
    static let vblankWait: Int32 = 1 << 18

    static let trial = Host1xFlipTest(
        id: "display.x1.host1x_flip", name: "Window A flipped by host1x behind its producer's syncpoint", flips: 60,
        forced: false
    )
    static let windowSyncpoints = Host1xFlipTest(
        id: "display.x1.host1x_flip_window_syncpt",
        name: "Window A's REG_WR_SAFE and LINE_FLIPPED syncpoints against each host1x flip's latch", flips: 60,
        forced: false, modes: windowIncrements
    )
    static let vblank = Host1xFlipTest(
        id: "display.x1.host1x_flip_vblank",
        name: "Host1x flips held for vblank syncpoint 9: the vblank each latches in",
        flips: 30, forced: false, modes: vblankWait
    )
    static let teardown = Host1xFlipTest(
        id: "display.x1.host1x_flip_teardown",
        name: "A failed producer's host1x flip torn down and written by the display instead", flips: 30, forced: true
    )

    func run(_ context: TestContext) async throws -> Outcome {
        guard case .value(let before) = KernelSysctl.integer(Self.counter, as: Int32.self) else {
            let error = KernelSysctl.integer(Self.counter, as: Int32.self).error
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(error))
            return .skip(KernelSysctl.unreadable(Self.counter, error), metrics)
        }
        var request = Int32(flips) | (forced ? Self.forcedFailure : 0) | modes
        let started = sysctlbyname(Self.sysctl, nil, nil, &request, MemoryLayout<Int32>.size) == 0 ? 0 : errno
        var line = ""
        for _ in 0 ..< 150 {
            guard case .value(let read) = KernelSysctl.string(Self.sysctl) else { break }
            line = read
            if started != 0 || !line.hasPrefix("host1x_flip=running") { break }
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        let after = KernelSysctl.integer(Self.counter, as: Int32.self)
        let fields = DcHost1xClassTest.fields(line)
        let value = { (key: String) in Int(fields[key] ?? "") ?? -1 }
        var metrics = Metrics()
        metrics.set("start", started == 0 ? "ok" : KernelSysctl.code(started))
        for key in [
            "host1x_flip", "reason", "flips", "host1x", "gate", "latched", "not_as_written", "unwritten", "teardowns",
            "after_teardown", "written", "display_delta", "underflows", "waited", "stop", "modes", "reg_wr_safe",
            "line_flipped", "vblank",
        ] {
            if let field = fields[key] { metrics.set(key, field) }
        }
        var delta = -1
        if case .value(let now) = after { delta = Int(now) - Int(before) }
        metrics.set("d_host1x_flips", delta)
        let host1x = value("host1x")
        var checks: [(String, Bool)] = [
            (
                "the run ended with window A on its front (host1x_flip = done, stop = none)",
                fields["host1x_flip"] == "done" && fields["stop"] == "none"
            ),
            (
                "every flip latched as written (latched = \(flips), not_as_written = 0)",
                value("latched") == flips && value("not_as_written") == 0
            ),
            ("kern.switch_flips_host1x rose by the flips host1x wrote (d_host1x_flips = host1x)", delta == host1x),
            (
                "one display syncpoint step per host1x flip, none for a torn-down stream (display_delta = host1x)",
                value("display_delta") == host1x
            ),
            ("no stream left unwritten (unwritten = 0)", value("unwritten") == 0),
            ("no underflow (underflows = 0)", value("underflows") == 0),
        ]
        let counts = { (key: String) in (fields[key] ?? "").split(separator: "/").map { Int($0) ?? -1 } }
        if modes & Self.windowIncrements != 0 {
            checks.append(
                (
                    "both window conditions are counted once per host1x flip (reg_wr_safe and line_flipped sum to host1x)",
                    counts("reg_wr_safe").reduce(0, +) == host1x && counts("line_flipped").reduce(0, +) == host1x
                )
            )
        }
        if modes & Self.vblankWait != 0 {
            checks.append(
                (
                    "every host1x flip's latch is counted against its vblank (vblank sums to host1x)",
                    counts("vblank").reduce(0, +) == host1x
                )
            )
        }
        if forced {
            checks += [
                (
                    "the failed producer's flip was torn down and written by the display (teardowns = 1, gate = 1)",
                    value("teardowns") == 1 && value("gate") == 1
                ),
                ("host1x wrote every other flip (host1x = \(flips - 1))", host1x == flips - 1),
                (
                    "the restarted channel carried the flips after it (after_teardown = \(flips - 1 - flips / 2))",
                    value("after_teardown") == flips - 1 - flips / 2
                ),
            ]
        } else {
            checks += [
                (
                    "host1x wrote every flip (host1x = \(flips), d_host1x_flips = \(flips))",
                    host1x == flips && delta == flips
                ),
                (
                    "no flip went through the display's work loop (gate = 0, teardowns = 0)",
                    value("gate") == 0 && value("teardowns") == 0
                ),
            ]
        }
        return .judged(checks, metrics)
    }
}
