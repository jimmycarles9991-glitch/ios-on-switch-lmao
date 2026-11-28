import Foundation

/// gpu.ca.sweep's reference: each Core Animation state's attachments as this IPSW's QuartzCore draws them on a Mac's
/// GPU (apps/hwtest/goldens/ca_<build>.txt, drawn by //apps/hwtest:ca_goldens on a build host with `referenceGPU`).
/// The header names the GPU (`# QuartzCore <build> on <GPU>, ...`), then one line per state:
///
///     <state key> <short label> c<index>=<signature> ...
///
/// Another GPU generation's filtering and rounding move about a third of the states' blocks by a code (M1 against M4,
/// 26 Nov), so a reference of any other GPU is no reference.
///
/// The key is CAStateSweep.key of the corpus line, so a state whose blending, formats or constants change finds no
/// reference instead of a wrong one. A line that gives its key no signature is kept apart in `malformed`, by key.
public struct CAGoldens: Sendable {
    /// The GPU every Core Animation golden is drawn on: MTLDevice.name of the build hosts the writer's test action is
    /// pinned to (apps/hwtest/BUILD.bazel CA_GOLDENS_GPU, the workers' `gpu` property)
    public static let referenceGPU = "Apple M4"

    public let header: String
    public let states: [String: [Int: Signature]]
    public let malformed: [String: String]

    public init(_ text: String) {
        var header = ""
        var states: [String: [Int: Signature]] = [:]
        var malformed: [String: String] = [:]
        for line in text.split(separator: "\n") {
            if line.hasPrefix("#") {
                header = String(line.dropFirst()).trimmingCharacters(in: .whitespaces)
                continue
            }
            let fields = line.split(separator: " ")
            var attachments: [Int: Signature] = [:]
            for field in fields.dropFirst(2) {
                guard field.hasPrefix("c"), let equals = field.firstIndex(of: "="),
                      let index = Int(field[field.index(after: field.startIndex) ..< equals]),
                      let signature = Signature(text: String(field[field.index(after: equals)...]))
                else { continue }
                attachments[index] = signature
            }
            guard let key = fields.first.map(String.init) else { continue }
            if attachments.isEmpty {
                malformed[key] = String(line)
            } else {
                states[key] = attachments
            }
        }
        self.header = header
        self.states = states
        self.malformed = malformed
    }

    /// The GPU the header names: the text between " on " and the next comma
    public var gpu: String? {
        guard let on = header.range(of: " on ") else { return nil }
        return String(header[on.upperBound...].prefix { $0 != "," })
    }

    public static func line(key: String, label: String, _ attachments: [(index: Int, signature: Signature)]) -> String {
        ([key, label] + attachments.map { "c\($0.index)=\($0.signature.text)" }).joined(separator: " ")
    }

    /// Within one code of `metalFormat` from `golden` (GoldenVerdict allows three, for its other scenes).
    public static func withinOneCode(_ ours: Signature, against golden: Signature, metalFormat: UInt) -> Bool {
        let code = Signature.format(metal: metalFormat)?.step() ?? 0
        return ours.matches(golden, absolute: code, relative: 0)
    }
}
