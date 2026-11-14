import Foundation

/// What one experiment came to, as the bench loop writes it (tools/benchloop/protocol.py decode_result reads it).
public struct BenchOutcome: Sendable {
    public var status = "ok"
    public var why = ""
    /// Per draw count: the GPU time of each timed command buffer from its PTIMER stamps (0: its stamp slot was
    /// reused before it was read) and the host's commit-to-fence time, both in nanoseconds
    public var runs: [(draws: UInt32, gpuNs: [UInt64], wallNs: [UInt64])] = []
    public var texels: Data?
    /// The experiment's storage buffer after its last command buffer
    public var storage: Data?
    /// A `compete` experiment's other channel: the command buffers it finished meanwhile, and why it stopped early
    public var competitorBuffers = 0
    public var competitorWhy = ""
    public var lostError: UInt32 = 0
    public var faultVA: UInt64 = 0
    public var templateKey: UInt64 = 0
    public var milliseconds = 0.0
    /// The bytes a kernel wrote to its output buffer (BenchExperiment.outputBytes)
    public var output: Data?
    /// The compare form's other result (BenchExperiment.compare), or its status and why where it did not run
    public var compareTexels: Data?
    public var compareFailure: (status: String, why: String)?
    /// What the app did, as it did it: the target's compression, the 2D operation's waits, a resolve's samples
    public var form: [String: Int] = [:]
    /// A pack experiment's last command buffer: its prologue launches and their frontiers' bytes in launch order
    public var split: (launches: Int, frontier: Data)?

    public init(status: String = "ok", why: String = "") {
        self.status = status
        self.why = why
    }
}

public enum BenchResult {
    public static let distinctLimit = 64

    public static func encode(batch: String, entry: BenchBatch.Entry, outcome: BenchOutcome) -> Data {
        var top: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": batch, "id": entry.id, "status": outcome.status,
            "why": outcome.why, "elapsed_ms": (outcome.milliseconds * 1000).rounded() / 1000,
            "runs": outcome.runs.map {
                ["draws": $0.draws, "gpu_ns": $0.gpuNs, "wall_ns": $0.wallNs] as [String: Any]
            },
        ]
        switch entry {
        case .experiment(let e):
            top["kind"] = e.kind.rawValue
            if let texels = outcome.texels, e.readback != .none {
                top["texels"] = texelFields(texels, e)
            }
            if let storage = outcome.storage, let s = e.storage {
                top["storage"] = storageFields(storage, full: s.readback == .full)
            }
            if let other = outcome.compareTexels, let texels = outcome.texels, e.readback != .none {
                top["compare"] = [
                    "with": e.compare ?? "", "status": "ok", "texels": texelFields(other, e),
                    "histogram": BenchDifference.histogram(texels, other, format: e.format, texelBytes: e.texelBytes),
                ]
            } else if let failure = outcome.compareFailure {
                top["compare"] = ["with": e.compare ?? "", "status": failure.status, "why": failure.why]
            }
            if let output = outcome.output, e.readback != .none {
                var fields: [String: Any] = [
                    "bytes": output.count, "crc32": hex(UInt64(crc32(output)), digits: 8),
                    "distinct": distinct(output, width: output.count / 4, texelBytes: 4),
                ]
                if e.readback == .full { fields["data"] = output.base64EncodedString() }
                top["output"] = fields
            }
            if !outcome.form.isEmpty { top["form"] = outcome.form }
            if let split = outcome.split {
                top["split"] =
                    [
                        "launches": split.launches, "bytes": split.frontier.count,
                        "crc32": hex(UInt64(crc32(split.frontier)), digits: 8),
                        "frontier": split.frontier.base64EncodedString(),
                    ] as [String: Any]
            }
            if e.compete {
                top["competitor"] = ["buffers": outcome.competitorBuffers, "why": outcome.competitorWhy]
            }
        case .invalid(_, let kind, _):
            top["kind"] = kind
        }
        if outcome.templateKey != 0 { top["template"] = hex(outcome.templateKey) }
        if outcome.status == "killed" {
            top["channel"] = ["lost_error": outcome.lostError, "fault_va": hex(outcome.faultVA)]
        }
        let bytes = try? JSONSerialization.data(withJSONObject: top, options: [.sortedKeys])
        return (bytes ?? Data()) + Data("\n".utf8)
    }

    /// Every different 32-bit word of the storage buffer with how many there are, none left out (a lost update is one
    /// odd word among millions), and with `full` the words themselves
    static func storageFields(_ storage: Data, full: Bool) -> [String: Any] {
        var counts: [UInt32: Int] = [:]
        storage.withUnsafeBytes { raw in
            for at in stride(from: 0, to: raw.count - raw.count % 4, by: 4) {
                counts[UInt32(littleEndian: raw.loadUnaligned(fromByteOffset: at, as: UInt32.self)), default: 0] += 1
            }
        }
        var fields: [String: Any] = [
            "bytes": storage.count,
            "distinct": counts.keys.sorted().map { ["value": hex(UInt64($0), digits: 8), "count": counts[$0]!] },
        ]
        if full { fields["data"] = storage.base64EncodedString() }
        return fields
    }

    static func texelFields(_ texels: Data, _ e: BenchExperiment) -> [String: Any] {
        var fields: [String: Any] = [
            "format": e.format, "width": e.width, "height": e.height, "crc32": hex(UInt64(crc32(texels)), digits: 8),
            "distinct": distinct(texels, width: Int(e.width), texelBytes: e.texelBytes),
        ]
        if e.readback == .full { fields["data"] = texels.base64EncodedString() }
        return fields
    }

    /// Each different texel in raster order with how many there are and where the first is: a timing experiment's
    /// pixels checked without sending them all.
    public static func distinct(_ texels: Data, width: Int, texelBytes: Int) -> [[String: Any]] {
        var order: [Data] = [], counts: [Data: Int] = [:], first: [Data: Int] = [:]
        for index in 0..<(texels.count / texelBytes) {
            let texel = texels.subdata(in: (index * texelBytes)..<((index + 1) * texelBytes))
            if counts[texel] == nil {
                guard order.count < distinctLimit else { continue }
                order.append(texel)
                first[texel] = index
            }
            counts[texel, default: 0] += 1
        }
        return order.map { texel in
            let at = first[texel]!
            return [
                "value": texel.map { String(format: "%02x", $0) }.joined(), "count": counts[texel]!,
                "first": [at % width, at / width],
            ]
        }
    }

    /// zlib's CRC-32, which the host's zlib.crc32 checks
    public static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = crc & 1 == 1 ? (crc >> 1) ^ 0xEDB8_8320 : crc >> 1 }
        }
        return ~crc
    }

    /// The os_log line per experiment, the return path when no file comes back:
    /// `BENCH <batch> <id> <status> runs=<draws>:<median GPU ns>,... crc=<texels> ms=<elapsed> [why=<text>]`
    public static func logLine(batch: String, id: String, outcome: BenchOutcome) -> String {
        var fields = ["BENCH", batch, id, outcome.status]
        let runs = outcome.runs.map { "\($0.draws):\(median($0.gpuNs))" }
        if !runs.isEmpty { fields.append("runs=" + runs.joined(separator: ",")) }
        if let texels = outcome.texels { fields.append("crc=" + hex(UInt64(crc32(texels)), digits: 8)) }
        fields.append("ms=" + String(format: "%.1f", outcome.milliseconds))
        if !outcome.why.isEmpty { fields.append("why=" + LogLine.sanitized(outcome.why)) }
        return fields.joined(separator: " ")
    }

    public static func median(_ values: [UInt64]) -> UInt64 {
        let sorted = values.sorted()
        return sorted.isEmpty ? 0 : sorted[sorted.count / 2]
    }

    static func hex(_ value: UInt64, digits: Int = 16) -> String {
        let text = String(value, radix: 16)
        return String(repeating: "0", count: max(0, digits - text.count)) + text
    }
}

/// The batch's journal, appended and flushed before and after each experiment: `starting <id>` then `done <id>
/// <status>`. An id started and never done ended the app's process (or the OS) while it ran.
public enum BenchJournal {
    public static func starting(_ id: String) -> String { "starting \(id)\n" }
    public static func done(_ id: String, _ status: String) -> String { "done \(id) \(status)\n" }

    public static func crashed(_ text: String) -> [String] {
        var open: [String] = []
        for line in text.split(separator: "\n") {
            let words = line.split(separator: " ").map(String.init)
            guard words.count >= 2 else { continue }
            if words[0] == "starting" { open.append(words[1]) }
            if words[0] == "done" { open.removeAll { $0 == words[1] } }
        }
        return open
    }

    public static func finished(_ text: String) -> Set<String> {
        Set(
            text.split(separator: "\n").compactMap { line in
                let words = line.split(separator: " ")
                return words.count >= 2 && words[0] == "done" ? String(words[1]) : nil
            })
    }
}
