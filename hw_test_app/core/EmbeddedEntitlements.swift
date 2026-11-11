import Foundation

/// The entitlements a thin 64-bit Mach-O is signed with: the XML plist blob (slot 5) of its LC_CODE_SIGNATURE
/// SuperBlob. Empty when the signature carries none; nil when the file is no such Mach-O or its signature is unreadable.
public enum EmbeddedEntitlements {
    static let machO64: UInt32 = 0xFEED_FACF
    static let codeSignature: UInt32 = 0x1D
    static let superBlob: UInt32 = 0xFADE_0CC0
    static let entitlementsBlob: UInt32 = 0xFADE_7171
    static let entitlementsSlot: UInt32 = 5
    static let headerSize = 32

    public static func of(_ image: Data) -> [String: Any]? {
        guard word(image, 0, bigEndian: false) == machO64, let count = word(image, 16, bigEndian: false) else {
            return nil
        }
        var at = headerSize
        for _ in 0..<count {
            guard let command = word(image, at, bigEndian: false), let size = word(image, at + 4, bigEndian: false),
                size >= 8
            else { return nil }
            if command == codeSignature {
                return word(image, at + 8, bigEndian: false).flatMap { signature(image, at: Int($0)) }
            }
            at += Int(size)
        }
        return nil
    }

    static func signature(_ image: Data, at start: Int) -> [String: Any]? {
        guard word(image, start, bigEndian: true) == superBlob, let count = word(image, start + 8, bigEndian: true)
        else { return nil }
        for index in 0..<Int(count) {
            let entry = start + 12 + 8 * index
            guard let slot = word(image, entry, bigEndian: true), let offset = word(image, entry + 4, bigEndian: true)
            else { return nil }
            let blob = start + Int(offset)
            guard slot == entitlementsSlot else { continue }
            guard word(image, blob, bigEndian: true) == entitlementsBlob,
                let length = word(image, blob + 4, bigEndian: true).map(Int.init), length >= 8,
                blob + length <= image.count
            else { return nil }
            let plist = image.subdata(in: (image.startIndex + blob + 8)..<(image.startIndex + blob + length))
            return (try? PropertyListSerialization.propertyList(from: plist, format: nil)) as? [String: Any]
        }
        return [:]
    }

    static func word(_ data: Data, _ at: Int, bigEndian: Bool) -> UInt32? {
        guard at >= 0, at + 4 <= data.count else { return nil }
        let start = data.startIndex + at
        let value = data[start..<(start + 4)].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
        return bigEndian ? value : value.byteSwapped
    }
}
