import Foundation

public enum InstallerError {
    public static func details(_ error: Error) -> String {
        var current: NSError? = error as NSError
        var visited = Set<ObjectIdentifier>()
        var lines: [String] = []
        while let native = current, visited.count < 16, visited.insert(ObjectIdentifier(native)).inserted {
            var descriptions = [native.localizedDescription]
            for detail in [
                native.localizedFailureReason,
                native.userInfo[NSDebugDescriptionErrorKey] as? String,
                native.localizedRecoverySuggestion,
            ] {
                if let detail, !detail.isEmpty, !descriptions.contains(detail) {
                    descriptions.append(detail)
                }
            }
            lines.append("\(native.domain) (\(native.code)): \(descriptions.joined(separator: " "))")
            current = native.userInfo[NSUnderlyingErrorKey] as? NSError
        }
        return lines.joined(separator: "\n")
    }
}
