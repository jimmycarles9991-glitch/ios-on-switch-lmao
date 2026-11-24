import Foundation

/// A line too long for one os_log message, which keeps about 1 KiB and puts `<…>` in place of the rest, goes out in
/// pieces: the first is the line's start, each next one `HWTEST <id> +<n> <text>` (n from 1) whose text follows the
/// piece before it with nothing between them. A reader rebuilds the line by appending each continuation's text, from
/// after `+<n> `, to the first piece (tools/devicerun/hwtest.py `joined`). Cuts fall before a space where one fits, so
/// the first piece ends on a whole field and a continuation's text then starts with that space.
extension LogLine {
    public static let pieceBytes = 960

    public static func pieces(_ line: String, bytes limit: Int = pieceBytes) -> [String] {
        let bytes = Array(line.utf8)
        guard bytes.count > limit else { return [line] }
        let id = line.split(separator: " ", maxSplits: 2).dropFirst().first.map(String.init) ?? ""
        var pieces: [String] = []
        var start = 0
        while start < bytes.count {
            let head = pieces.isEmpty ? "" : "\(prefix) \(id) +\(pieces.count) "
            let end = cut(bytes, from: start, room: max(limit - head.utf8.count, 8))
            pieces.append(head + String(decoding: bytes[start ..< end], as: UTF8.self))
            start = end
        }
        return pieces
    }

    public static func joined(_ pieces: [String]) -> String {
        pieces.dropFirst().reduce(pieces.first ?? "") { line, piece in
            let parts = piece.split(separator: " ", maxSplits: 3, omittingEmptySubsequences: false)
            return parts.count == 4 ? line + String(parts[3]) : line
        }
    }

    private static func cut(_ bytes: [UInt8], from start: Int, room: Int) -> Int {
        let limit = start + room
        guard limit < bytes.count else { return bytes.count }
        if let space = bytes[(start + 1) ... limit].lastIndex(of: UInt8(ascii: " ")) { return space }
        var end = limit
        while end > start + 1 && bytes[end] & 0xC0 == 0x80 { end -= 1 }
        return end
    }
}
