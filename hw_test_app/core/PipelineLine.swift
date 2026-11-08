import Foundation

/// A render pipeline state as the Metal frontend journals it and the raw pack's corpus keeps it
/// (drivers/TegraGM20BMetal/precompile/corpus.log: "R ok=1 label=... samples=1 topo=0 ds=0/0 c0=... a0=... | v <name>
/// <constants> | f <name> <constants> |"), so a test can make the exact state Core Animation made.
public struct PipelineLine: Equatable, Sendable {
    /// cN=format:writeMask:blending:srcRGB:dstRGB:rgbOp:srcAlpha:dstAlpha:alphaOp
    public struct Color: Equatable, Sendable {
        public let index: Int
        public let fields: [UInt]

        public var format: UInt { fields[0] }
        public var writeMask: UInt { fields[1] }
        public var blending: Bool { fields[2] != 0 }
    }

    /// aN=format:offset:buffer:stride:stepFunction:stepRate
    public struct Attribute: Equatable, Sendable {
        public let index: Int
        public let fields: [UInt]
    }

    /// name:MTLDataType:value bytes in hex, little-endian
    public struct Constant: Equatable, Sendable {
        public let name: String
        public let type: UInt
        public let bytes: [UInt8]
    }

    public struct Function: Equatable, Sendable {
        public let name: String
        public let constants: [Constant]
    }

    public let label: String
    public let samples: Int
    public let topology: UInt
    public let depthFormat: UInt
    public let stencilFormat: UInt
    public let colors: [Color]
    public let attributes: [Attribute]
    public let vertex: Function
    public let fragment: Function

    /// The label's last dotted part ("PBGRAXm_A2Xhf" of com.apple.coreanimation.PBGRAXm_A2Xhf).
    public var shortLabel: String { label.split(separator: ".").last.map(String.init) ?? label }

    public init?(_ line: String) {
        let parts = line.components(separatedBy: " | ").map {
            $0.trimmingCharacters(in: CharacterSet(charactersIn: " |"))
        }
        guard parts.count >= 3, line.hasPrefix("R ") else { return nil }
        var fields: [String: String] = [:]
        var colors: [Color] = []
        var attributes: [Attribute] = []
        for token in parts[0].split(separator: " ").dropFirst() {
            guard let equals = token.firstIndex(of: "=") else { return nil }
            let (key, value) = (String(token[..<equals]), String(token[token.index(after: equals)...]))
            let numbers = value.split(separator: ":").compactMap { UInt($0) }
            if key.count >= 2, let index = Int(key.dropFirst()), key.first == "c" {
                colors.append(Color(index: index, fields: numbers))
            } else if key.count >= 2, let index = Int(key.dropFirst()), key.first == "a" {
                attributes.append(Attribute(index: index, fields: numbers))
            } else {
                fields[key] = value
            }
        }
        let depths = (fields["ds"] ?? "0/0").split(separator: "/").compactMap { UInt($0) }
        guard colors.allSatisfy({ $0.fields.count == 9 }), attributes.allSatisfy({ $0.fields.count == 6 }),
            depths.count == 2, let vertex = Self.function(parts[1], "v"), let fragment = Self.function(parts[2], "f")
        else { return nil }
        label = fields["label"] ?? "-"
        samples = Int(fields["samples"] ?? "1") ?? 1
        topology = UInt(fields["topo"] ?? "0") ?? 0
        depthFormat = depths[0]
        stencilFormat = depths[1]
        self.colors = colors
        self.attributes = attributes
        self.vertex = vertex
        self.fragment = fragment
    }

    static func function(_ part: String, _ stage: String) -> Function? {
        let tokens = part.split(separator: " ").map(String.init)
        guard tokens.count >= 2, tokens[0] == stage else { return nil }
        var constants: [Constant] = []
        for token in tokens.dropFirst(2) where !token.hasPrefix("lib=") {
            let pieces = token.split(separator: ":").map(String.init)
            guard pieces.count == 3, let type = UInt(pieces[1]), pieces[2].count % 2 == 0 else { return nil }
            let hex = Array(pieces[2])
            let bytes = stride(from: 0, to: hex.count, by: 2).compactMap {
                UInt8(String(hex[$0..<($0 + 2)]), radix: 16)
            }
            guard bytes.count * 2 == hex.count else { return nil }
            constants.append(Constant(name: pieces[0], type: type, bytes: bytes))
        }
        return Function(name: tokens[1], constants: constants)
    }
}
