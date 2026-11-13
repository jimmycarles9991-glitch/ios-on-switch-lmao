import Darwin
import HWTestCore

/// Whether host1x routes a class 0x70 stream to DC A, and whether a WAIT_SYNCPT ahead of it holds the DC writes
/// (drivers/NativeMobileDisplay/SwitchDcHost1xClassProbe.h, debug kernels): the kernel submits it on NVJPG's channel,
/// increments the gate syncpoint itself, and reports what landed. A finding, not a judgement of the driver:
/// class70=routes with held=1 means a producer's syncpoint could latch a flip with no CPU (the next build step);
/// ignored means host1x ran past the words and DC A took none; faults means the channel stopped at them (CBSTAT's
/// class and offset say where). Runs after every other shared test: it may stop a host1x channel for 50 ms.
struct DcHost1xClassTest: HWTest {
    let id = "gpu.x1.dc_host1x_class"
    let name = "host1x class 0x70 stream to DC A behind a WAIT_SYNCPT"
    let subsystem = Subsystem.display

    static let sysctl = "debug.switch_display_dc_host1x_class"
    static let verdicts: Set<String> = ["routes", "ignored", "faults", "partial"]

    func run(_ context: TestContext) async throws -> Outcome {
        var run: Int32 = 1
        guard sysctlbyname(Self.sysctl, nil, nil, &run, MemoryLayout<Int32>.size) == 0 else {
            let error = errno
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(error))
            return .skip(KernelSysctl.unreadable(Self.sysctl, error), metrics)
        }
        guard case .value(let line) = KernelSysctl.string(Self.sysctl) else {
            return .skip(KernelSysctl.unreadable(Self.sysctl, errno), Metrics())
        }
        let fields = Self.fields(line)
        var metrics = Metrics()
        for key in ["class70", "held", "dc_syncpt", "scratch", "scratch_cpu", "gate", "reason"] {
            if let value = fields[key] { metrics.set(key, value) }
        }
        let channel = [("DMAGET", "dmaget"), ("class", "cb_class"), ("offset", "cb_offset"), ("CBREAD", "cbread")]
        for (key, label) in channel {
            if let value = fields[key] { metrics.set(label, value) }
        }
        return .judged(
            [
                (
                    "the probe ran and gave a verdict (class70 = routes, ignored, faults or partial)",
                    Self.verdicts.contains(fields["class70"] ?? "")
                )
            ], metrics)
    }

    /// "key=value" tokens, and "NAME value" pairs of the channel's description.
    static func fields(_ line: String) -> [String: String] {
        let tokens = line.split(separator: " ").map(String.init)
        var fields: [String: String] = [:]
        for (index, token) in tokens.enumerated() {
            if let equals = token.firstIndex(of: "=") {
                fields[String(token[..<equals])] = String(token[token.index(after: equals)...])
            } else if index + 1 < tokens.count, !tokens[index + 1].contains("=") {
                fields[token] = tokens[index + 1]
            }
        }
        return fields
    }
}
