import Foundation
import os
import QuartzCore

/// tools/perf's scenario marks (windows.py MARK): "SwitchMark: <label> <ms> ms", the span of `ms` ending at the line,
/// then what the test measured in it, so a report cuts every other process's lines of that span out of the log.
enum SwitchMarks {
    private static let log = Logger(subsystem: "com.brunostjohn.switch.hwtest", category: "marks")

    /// The span from `start` (CACurrentMediaTime's clock) to now, named `label`, with `detail` after it.
    static func mark(_ label: String, since start: Double, _ detail: String = "") {
        let ms = Int(max(0, (CACurrentMediaTime() - start) * 1000).rounded())
        let text = detail.isEmpty ? "" : " " + detail
        log.log("SwitchMark: \(label, privacy: .public) \(ms) ms\(text, privacy: .public)")
    }
}
