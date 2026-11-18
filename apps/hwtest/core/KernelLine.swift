import Foundation

/// A debug kernel's sysctl line of "key=value" fields, as the tests that ask for one read it.
public enum KernelLine {
    /// "key=value" tokens.
    public static func fields(_ line: String) -> [String: String] {
        var fields: [String: String] = [:]
        for token in line.split(separator: " ") {
            guard let equals = token.firstIndex(of: "=") else { continue }
            fields[String(token[..<equals])] = String(token[token.index(after: equals)...])
        }
        return fields
    }

    /// Each of `keys` the line carries, as the metric of its name; a name the HWTEST line keeps for itself
    /// (LogLine.reservedKeys: a kernel's why=, say) as kernel_<name>.
    public static func metrics(_ fields: [String: String], keys: [String]) -> Metrics {
        var metrics = Metrics()
        for key in keys {
            guard let value = fields[key] else { continue }
            metrics.set(LogLine.reservedKeys.contains(key) ? "kernel_" + key : key, value)
        }
        return metrics
    }
}
