import Foundation

/// gpu.ca.sweep's reference: each Core Animation state's attachments as this IPSW's QuartzCore draws them on a Mac's
/// GPU (hw_test_app/goldens/ca_<build>.txt, written by `bazel run //hw_test_app:ca_goldens`). One line per state:
///
///     <state key> <short label> c<index>=<signature> ...
///
/// The key is CAStateSweep.key of the corpus line, so a state whose blending, formats or constants change finds no
/// reference instead of a wrong one.
public struct CAGoldens: Sendable {
    public let header: String
    public let states: [String: [Int: Signature]]

    public init(_ text: String) {
        var header = ""
        var states: [String: [Int: Signature]] = [:]
        for line in text.split(separator: "\n") {
            if line.hasPrefix("#") {
                header = String(line.dropFirst()).trimmingCharacters(in: .whitespaces)
                continue
            }
            let fields = line.split(separator: " ")
            guard fields.count >= 3 else { continue }
            var attachments: [Int: Signature] = [:]
            for field in fields.dropFirst(2) {
                guard field.hasPrefix("c"), let equals = field.firstIndex(of: "="),
                    let index = Int(field[field.index(after: field.startIndex)..<equals]),
                    let signature = Signature(text: String(field[field.index(after: equals)...]))
                else { continue }
                attachments[index] = signature
            }
            states[String(fields[0])] = attachments
        }
        self.header = header
        self.states = states
    }

    public static func line(key: String, label: String, _ attachments: [(index: Int, signature: Signature)]) -> String {
        ([key, label] + attachments.map { "c\($0.index)=\($0.signature.text)" }).joined(separator: " ")
    }
}
