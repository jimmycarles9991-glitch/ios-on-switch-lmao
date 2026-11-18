import Foundation
import HWTestCore

/// The loop's own proof with no Mac attached, run as the bench screen opens: empty passes are timed, the app's
/// known-answer program writes exactly its shader data to every texel, and a kernel of nothing but EXIT completes.
/// The experiments go through the batch decoder the Mac's batches go through.
enum BenchSelfCheck {
    static func run(_ runner: BenchRunner?) -> String {
        guard let runner else { return "FAIL no raw device" }
        let size = 16
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "selfcheck",
            "experiments": [
                ["id": "noop", "kind": "noop", "target": target(size), "draws": [0], "repeats": 3, "readback": "none"],
                ["id": "known", "kind": "known", "target": target(size), "readback": "full"],
                [
                    "id": "known-bgra8", "kind": "known", "readback": "full",
                    "target": ["format": "bgra8unorm", "width": size, "height": size],
                ],
                [
                    "id": "compute", "kind": "compute", "target": target(size), "readback": "none",
                    "compute": [
                        "code": BenchRunner.knownKernel.base64EncodedString(), "gprs": 1, "threads": [32, 1, 1],
                        "groups": [2, 1, 1],
                    ],
                ],
            ],
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: batch),
            let decoded = try? BenchBatch.decode(data)
        else { return "FAIL the self-check batch does not decode" }
        var parts: [String] = []
        var failed = false
        for entry in decoded.entries {
            guard case .experiment(let e) = entry else {
                return "FAIL \(entry.id) invalid"
            }
            let outcome = runner.run(e)
            let gpu = outcome.runs.first.map { BenchResult.median($0.gpuNs) } ?? 0
            var verdict = outcome.status == "ok" && gpu > 0
            var detail = outcome.status == "ok" ? "\(gpu)ns" : "\(outcome.status):\(LogLine.sanitized(outcome.why))"
            if e.kind == .known, outcome.status == "ok", e.format != "rgba32float" {
                // the template's own format: every texel the same, whatever the store's rounding of 0.5 and 0.25
                let uniform = isUniform(outcome.texels ?? Data(), texelBytes: e.texelBytes)
                verdict = verdict && uniform
                detail += uniform ? ",uniform" : ",not uniform"
            } else if e.kind == .known, outcome.status == "ok" {
                let right = matching(outcome.texels ?? Data(), want: BenchRunner.knownData)
                verdict = verdict && right == size * size
                detail += ",\(right)/\(size * size)"
            }
            failed = failed || !verdict
            parts.append("\(e.id)=\(detail)")
        }
        return (failed ? "FAIL " : "PASS ") + parts.joined(separator: " ")
    }

    private static func target(_ size: Int) -> [String: Any] {
        ["format": "rgba32float", "width": size, "height": size]
    }

    private static func isUniform(_ texels: Data, texelBytes: Int) -> Bool {
        guard texels.count >= texelBytes else { return false }
        let first = texels.prefix(texelBytes)
        return stride(from: 0, to: texels.count, by: texelBytes).allSatisfy {
            texels.subdata(in: $0..<($0 + texelBytes)) == first
        }
    }

    private static func matching(_ texels: Data, want: [UInt32]) -> Int {
        let texel = want.withUnsafeBytes { Data($0) }
        return stride(from: 0, to: texels.count - 15, by: 16).filter { texels.subdata(in: $0..<($0 + 16)) == texel }
            .count
    }
}
