import Foundation
import os

/// A probe's steps on the card before each starts (Documents/hwtest/x1-probe-steps.log, F_FULLFSYNC'd) and in the
/// log: a hang or a panic names the cell and arm that ran, not only the probe.
public struct ProbeJournal {
    let probe: String
    private static let logger = Logger(subsystem: "com.brunostjohn.switch.hwtest", category: "x1probe")

    public init(probe: String) { self.probe = probe }

    public func starting(_ step: String) {
        let line = "\(Date().timeIntervalSince1970) \(probe) starting \(step)\n"
        Self.logger.log("\(probe, privacy: .public) starting \(step, privacy: .public)")
        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            return
        }
        let directory = documents.appendingPathComponent("hwtest")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try? DurableFile.append(line, to: directory.appendingPathComponent("x1-probe-steps.log"))
    }
}
