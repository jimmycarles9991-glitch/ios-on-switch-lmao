import Foundation

public enum InstallerInput {
    public static let maximumDownloadSize: Int64 = 8 * 1024 * 1024 * 1024

    public static func downloadURL(_ text: String) throws -> URL {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let components = URLComponents(string: trimmed),
              components.scheme?.lowercased() == "https",
              let host = components.host, !host.isEmpty,
              components.user == nil, components.password == nil,
              let url = components.url
        else {
            throw InstallerFailure("Enter a direct HTTPS link to an IPA file.")
        }
        return url
    }

    public static func checkResponse(_ response: URLResponse?) throws {
        guard let response = response as? HTTPURLResponse else {
            throw InstallerFailure("The server did not return an HTTP response.")
        }
        guard (200 ..< 300).contains(response.statusCode) else {
            throw InstallerFailure(
                "The download server returned HTTP \(response.statusCode). Check the link and try again."
            )
        }
        guard response.expectedContentLength <= maximumDownloadSize else {
            throw InstallerFailure("This IPA is larger than the 8 GB download limit.")
        }
    }
}

public struct InstallerFailure: LocalizedError {
    public let message: String

    public init(_ message: String) {
        self.message = message
    }

    public var errorDescription: String? { message }
}
