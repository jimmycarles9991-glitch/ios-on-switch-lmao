import Darwin
import HWTestCore

/// The DFLL's rows asked from above and from below (drivers/SwitchPower/SwitchThermalDfllLanding.h, debug kernels):
/// the kernel asks each case's first rate, lets it settle, asks the row and measures the CPU's clock where it runs
/// 2, 10, 50 and 200 ms later, with the loop's registers beside each reading. Judged at 200 ms (clk-dfll.c's Vmin
/// calibration has run): every row at most 0.25 % over the rate asked and at most 0.8 % under it, and each row the
/// same from above and from below. The skipper lands up to 1/256 under and the probe reads up to 0.3 % low; boot 21's
/// one-step-fast landings read 0.48 % and 0.56 % over. The earlier readings are the settling, reported only.
struct CPUDfllLandingTest: HWTest {
    let id = "cpu.dfll_landing"
    let name = "DFLL rows land on the rate asked, from above and from below"
    let subsystem = Subsystem.cpu

    static let sysctl = "debug.switch_perf_dfll_landing"
    static let settledMs = 200

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
        metrics.set("dfll", fields["dfll"] ?? "-")
        if let reason = fields["reason"] { metrics.set("reason", reason) }
        if let restore = fields["restore"] { metrics.set("restore", restore) }
        var landed: [Int: [Double]] = [:]
        var errors: [String] = []
        var index = 0
        while let rates = fields["\(index)"] {
            let pair = rates.split(separator: ">").compactMap { Int($0) }
            if let error = fields["\(index).err"] { errors.append("\(rates): \(error)") }
            for (key, value) in fields where key.hasPrefix("\(index).") && key != "\(index).err" {
                metrics.set("c\(index)_\(rates)_\(key.dropFirst("\(index).".count))ms", value)
            }
            if pair.count == 2, let settled = fields["\(index).\(Self.settledMs)"],
                let khz = Double(settled.split(separator: ",").first ?? "")
            {
                landed[pair[1], default: []].append(khz / 1000)
            }
            index += 1
        }
        var checks: [(String, Bool)] = [
            ("the probe ran on the DFLL (dfll=ran)", fields["dfll"] == "ran"),
            ("every rate was taken (\(errors.joined(separator: "; ")))", errors.isEmpty),
            ("every case was measured", index > 0 && landed.values.map(\.count).reduce(0, +) == index),
        ]
        for (asked, readings) in landed.sorted(by: { $0.key < $1.key }) {
            let target = Double(asked)
            let over = ((readings.max() ?? 0) - target) / target * 100
            let under = (target - (readings.min() ?? 0)) / target * 100
            let spread = ((readings.max() ?? 0) - (readings.min() ?? 0)) / target * 100
            metrics.set("over_pct_\(asked)", over, digits: 2)
            metrics.set("under_pct_\(asked)", under, digits: 2)
            metrics.set("spread_pct_\(asked)", spread, digits: 2)
            checks.append(("\(asked) MHz at most 0.25% over the rate asked at \(Self.settledMs) ms", over <= 0.25))
            checks.append(("\(asked) MHz at most 0.8% under the rate asked at \(Self.settledMs) ms", under <= 0.8))
            checks.append(("\(asked) MHz the same from above and below (within 0.5%)", spread <= 0.5))
        }
        return .judged(checks, metrics)
    }

    /// "key=value" tokens.
    static func fields(_ line: String) -> [String: String] {
        var fields: [String: String] = [:]
        for token in line.split(separator: " ") {
            guard let equals = token.firstIndex(of: "=") else { continue }
            fields[String(token[..<equals])] = String(token[token.index(after: equals)...])
        }
        return fields
    }
}
