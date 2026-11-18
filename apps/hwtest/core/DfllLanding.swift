import Foundation

/// HW Test cpu.dfll_landing's verdict on the kernel's line (drivers/SwitchPower/SwitchThermalDfllLanding.h): case i is
/// "i=<from MHz>><to MHz>", its readings "i.<ms>=<probe kHz>,<FREQ_REQ>,<TUNE1>,<OUTPUT_CFG>,<MONITOR_DATA>,<I2C_STS>".
/// Judged at 200 ms, once clk-dfll.c's Vmin calibration has run: every row at most 0.8 % under the rate asked, and over
/// it, and apart from above and from below, by no more than the DFLL holds where its loop stood.
/// A loop pinned at DFLL_OUTPUT_CFG MIN runs the DVCO floor skipped down: 0.25 % over and 0.5 % apart, so a landing a
/// skipper step fast (boot 21: 0.48 % and 0.56 % over) fails. A loop above MIN regulates the DVCO by LUT output steps,
/// and T210B01's runs without its integral term (cf 6, ci 0, cg 2: TegraDFLL/platform.c; Tegra X1 TRM 6.1.3 CI_PARAM),
/// so it rests anywhere within one step of its target or alternates between two: one step of DVCO at the skipper's
/// (SCALE + 1) / 256. A step is 15 mV of LUT at this console's 2.04 MHz/mV (clk-tegra124-dfll-fcpu.c's CPUB01 rows
/// 1581 and 1683 MHz at speedo 1659: 834 and 884 mV), 30 MHz, as TegraDFLL's landing model takes it; boot 22 measured
/// 20.6 to 23.4 MHz between outputs 17 and 18 at MULT 76, and held 1581 MHz at output 14 0.36 % over.
public enum DfllLanding {
    public static let settledMs = 200

    /// One reading's probe and where its loop stood (Tegra X1 TRM 6.1.6 DFLL_FREQ_REQ SCALE, 6.1.9 DFLL_OUTPUT_CFG MIN;
    /// clk-dfll.c's DFLL_I2C_STS last output).
    struct Reading {
        static let outputStepKhz = 30000.0

        let khz: Double
        let scale, outMin, lastOutput: UInt32

        init?(_ value: String) {
            let parts = value.split(separator: ",")
            guard parts.count == 6, let khz = Double(parts[0]) else { return nil }
            let registers = parts.dropFirst().compactMap { UInt32($0, radix: 16) }
            guard registers.count == 5 else { return nil }
            self.khz = khz
            scale = (registers[0] >> 8) & 0xff
            outMin = (registers[2] >> 8) & 0x3f
            lastOutput = (registers[4] >> 1) & 0x3f
        }

        var pinned: Bool { lastOutput <= outMin }

        func overBoundPct(asked mhz: Int) -> Double { pinned ? 0.25 : stepPct(asked: mhz) }

        func spreadBoundPct(asked mhz: Int) -> Double { pinned ? 0.5 : stepPct(asked: mhz) }

        func stepPct(asked mhz: Int) -> Double {
            Self.outputStepKhz * Double(scale + 1) / 256 / Double(mhz * 1000) * 100
        }
    }

    public static func judge(_ fields: [String: String]) -> (checks: [(String, Bool)], metrics: Metrics) {
        var metrics = Metrics()
        metrics.set("dfll", fields["dfll"] ?? "-")
        if let reason = fields["reason"] { metrics.set("reason", reason) }
        if let restore = fields["restore"] { metrics.set("restore", restore) }
        var landed: [Int: [Landed]] = [:]
        var errors: [String] = []
        var index = 0
        while let rates = fields["\(index)"] {
            let pair = rates.split(separator: ">").compactMap { Int($0) }
            if let error = fields["\(index).err"] { errors.append("\(rates): \(error)") }
            metrics.set("c\(index)", rates.replacingOccurrences(of: ">", with: "_to_"))
            for (key, value) in fields.sorted(by: { $0.key < $1.key }) where key.hasPrefix("\(index).") {
                let ms = key.dropFirst("\(index).".count)
                guard ms.allSatisfy(\.isNumber) else { continue }
                metrics.set("c\(index)_\(ms)", value)
                if ms != "\(settledMs)" { metrics.keepInReportOnly("c\(index)_\(ms)") }
            }
            if pair.count == 2, let settled = fields["\(index).\(settledMs)"],
                let khz = Double(settled.split(separator: ",").first ?? "")
            {
                landed[pair[1], default: []].append((khz, Reading(settled)))
            }
            index += 1
        }
        var checks: [(String, Bool)] = [
            ("the probe ran on the DFLL (dfll=ran)", fields["dfll"] == "ran"),
            ("every rate was taken (\(errors.joined(separator: "; ")))", errors.isEmpty),
            ("every case was measured", index > 0 && landed.values.map(\.count).reduce(0, +) == index),
        ]
        for (asked, readings) in landed.sorted(by: { $0.key < $1.key }) {
            checks += judge(asked, readings, &metrics)
        }
        return (checks, metrics)
    }

    typealias Landed = (khz: Double, reading: Reading?)

    static func judge(_ asked: Int, _ readings: [Landed], _ metrics: inout Metrics) -> [(String, Bool)] {
        let target = Double(asked)
        let mhz = readings.map { $0.khz / 1000 }
        let overs = mhz.map { ($0 - target) / target * 100 }
        let under = (target - (mhz.min() ?? 0)) / target * 100
        let spread = ((mhz.max() ?? 0) - (mhz.min() ?? 0)) / target * 100
        metrics.set("over_pct_\(asked)", overs.max() ?? 0, digits: 2)
        metrics.set("under_pct_\(asked)", under, digits: 2)
        metrics.set("spread_pct_\(asked)", spread, digits: 2)
        let bounds = readings.map { $0.reading?.overBoundPct(asked: asked) ?? 0.25 }
        let worst = overs.indices.max { overs[$0] - bounds[$0] < overs[$1] - bounds[$1] } ?? 0
        let how = { (reading: Reading?) in
            reading?.pinned ?? true ? "pinned at OUTPUT_CFG MIN" : "one LUT output step, the loop above MIN"
        }
        let spreadBound = readings.map { $0.reading?.spreadBoundPct(asked: asked) ?? 0.5 }.min() ?? 0.5
        let regulating = readings.allSatisfy { !($0.reading?.pinned ?? true) }
        let percent = { (value: Double) in String(format: "%.2f", value) }
        return [
            (
                "\(asked) MHz at most \(percent(bounds[worst]))% over the rate asked at \(settledMs) ms "
                    + "(\(how(readings[worst].reading)))",
                zip(overs, bounds).allSatisfy { $0 <= $1 }
            ),
            ("\(asked) MHz at most 0.8% under the rate asked at \(settledMs) ms", under <= 0.8),
            (
                "\(asked) MHz the same from above and below (within \(percent(spreadBound))%, "
                    + "\(regulating ? "one LUT output step" : "a loop pinned at MIN"))",
                spread <= spreadBound
            ),
        ]
    }
}
