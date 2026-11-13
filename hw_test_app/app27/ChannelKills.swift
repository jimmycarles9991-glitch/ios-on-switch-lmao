import Foundation
import HWTestGPU
import Metal
import OSLog

/// Channels the kernel killed under this process's m2v work. NVK finds its channel dead at a later submission and logs
/// "nvk darwin: channel N was killed (error E): channel M replaces it" in this process (nvkmd_darwin.c; E is
/// TegraGM20BABI.h method 11's: 1 an MMU fault, 2 a GR exception such as DATA_ERROR, 4 a hang), while the command
/// buffer the kill took may still complete without an error.
enum ChannelKills {
    struct Scan {
        /// The step's own journal line came back, so the log was read
        var seen = false
        var errors: [Int] = []
        var milliseconds = 0.0
    }

    /// NVK's kill lines since `date`, and whether ProbeJournal's line for `step` of `probe` is among the entries
    static func scan(since date: Date, probe: String, step: String) -> Scan {
        let began = CFAbsoluteTimeGetCurrent()
        var scan = Scan()
        let journalled = "\(probe) starting \(step)"
        if let store = try? OSLogStore(scope: .currentProcessIdentifier),
            let entries = try? store.getEntries(
                at: store.position(date: date),
                matching: NSPredicate(
                    format: "composedMessage CONTAINS %@ OR composedMessage == %@", "was killed", journalled))
        {
            for entry in entries where entry.date >= date {
                let message = entry.composedMessage
                if message == journalled { scan.seen = true }
                guard message.hasPrefix("nvk darwin: channel"), let at = message.range(of: "(error ") else { continue }
                scan.errors.append(Int(message[at.upperBound...].prefix { $0.isNumber }) ?? 0)
            }
        }
        scan.milliseconds = (CFAbsoluteTimeGetCurrent() - began) * 1000
        return scan
    }

    /// Two small submissions after the work under test: a channel its kill left dead is replaced, and the kill logged,
    /// by the first or, where the kill lands late, the second. The first failed one's path ("error:..."), else "".
    static func settle(_ gpu: GPU) async throws -> String {
        let scratch = try gpu.buffer(16)
        var failed = ""
        for _ in 0..<2 {
            let path = try await gpu.run(tolerateError: true) { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.fill(buffer: scratch, range: 0..<16, value: 0xa5)
                blit.endEncoding()
            }.path
            if failed.isEmpty && path.hasPrefix("error:") { failed = path }
        }
        return failed
    }
}
