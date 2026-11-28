import Foundation

/// A clear value as a ZBC colour entry holds it: the NV9097 format and the SET_COLOR_CLEAR_VALUE words, each the
/// float the facade quantises the channel to (drivers/TegraGM20BMetal/clear.h clearValue).
public struct ZbcValue: Equatable, Sendable {
    public let name: String
    public let format: UInt32
    public let channels: [Float]

    public init(name: String, format: UInt32, channels: [Float]) {
        self.name = name
        self.format = format
        self.channels = channels
    }

    public static func unorm(_ name: String, format: UInt32, _ clear: [Double], bits: [Int]) -> ZbcValue {
        ZbcValue(
            name: name, format: format,
            channels: zip(clear, bits).map { value, bits in
                let top = Double((1 << bits) - 1)
                return Float((Swift.min(Swift.max(Double(Float(value)), 0), 1) * top).rounded() / top)
            }
        )
    }

    public static func half(_ name: String, format: UInt32, _ clear: [Double]) -> ZbcValue {
        ZbcValue(name: name, format: format, channels: clear.map { Float(Float16(Float($0))) })
    }

    public static func float(_ name: String, format: UInt32, _ clear: [Double]) -> ZbcValue {
        ZbcValue(name: name, format: format, channels: clear.map { Float($0) })
    }

    /// Within one float rounding of the other: an sRGB channel's linear value comes out of pow on either side.
    func same(_ other: ZbcValue) -> Bool {
        format == other.format && channels.count == other.channels.count
            && zip(channels, other.channels).allSatisfy { abs($0 - $1) <= 1e-6 }
    }
}

/// The GPU's ZBC colour table as a debug kernel answers it (drivers/TegraGM20B/TegraGM20BPathsABI.h TegraGM20BPathsOpZbc):
/// every process's entries, never emptied, so a test that adds entries has only the room the boot and the other
/// processes left it.
public struct ZbcTable: Equatable, Sendable {
    public let used: Int
    public let slots: Int
    public let colours: [ZbcValue]

    public init(used: Int, slots: Int, colours: [ZbcValue]) {
        self.used = used
        self.slots = slots
        self.colours = colours
    }

    public var free: Int { max(0, slots - used) }

    public func holds(_ value: ZbcValue) -> Bool { colours.contains { $0.same(value) } }

    /// "colour=U/N depth=U/N c=FF:DS0,DS1,DS2,DS3;... d=FF:DS;...", words in hex; nil for anything else.
    public static func parse(_ line: String) -> ZbcTable? {
        var fields: [String: Substring] = [:]
        for field in line.split(separator: " ") {
            guard let equals = field.firstIndex(of: "=") else { continue }
            fields[String(field[..<equals])] = field[field.index(after: equals)...]
        }
        guard let occupancy = fields["colour"]?.split(separator: "/").compactMap({ Int($0) }), occupancy.count == 2
        else { return nil }
        var colours: [ZbcValue] = []
        for entry in (fields["c"] ?? "").split(separator: ";") {
            let parts = entry.split(separator: ":")
            guard parts.count == 2, let format = UInt32(parts[0], radix: 16) else { return nil }
            let words = parts[1].split(separator: ",").compactMap { UInt32($0, radix: 16) }
            guard words.count == 4 else { return nil }
            colours.append(ZbcValue(name: "", format: format, channels: words.map(Float.init(bitPattern:))))
        }
        return ZbcTable(used: occupancy[0], slots: occupancy[1], colours: colours)
    }
}

/// Which of a test's clear values get the table's room: every `required` value must fit, `optional` ones take what
/// is left in order. A value the table holds already, or one an earlier value of the test asks for too, takes none.
public struct ZbcBudget: Equatable, Sendable {
    public let used: Int
    public let free: Int
    /// New entries the required values need.
    public let required: Int
    /// The optional values that run; the others found no room.
    public let run: [String]
    public let noRoom: [String]

    public var requiredFits: Bool { required <= free }

    public init(_ table: ZbcTable, required: [ZbcValue], optional: [ZbcValue]) {
        used = table.used
        free = table.free
        var asked: [ZbcValue] = []
        func new(_ value: ZbcValue) -> Bool {
            !table.holds(value) && !asked.contains { $0.same(value) }
        }
        var needed = 0
        for value in required where new(value) {
            asked.append(value)
            needed += 1
        }
        self.required = needed
        var room = max(0, free - needed)
        var run: [String] = [], noRoom: [String] = []
        for value in optional {
            if !new(value) {
                run.append(value.name)
            } else if room > 0 {
                asked.append(value)
                room -= 1
                run.append(value.name)
            } else {
                noRoom.append(value.name)
            }
        }
        self.run = run
        self.noRoom = noRoom
    }

    /// Why the required values cannot be judged, for a FAIL; nil when they fit. `boot`: the entries the kernel adds
    /// at boot.
    public func refusal(boot: Int) -> String? {
        guard !requiredFits else { return nil }
        return "the GPU's ZBC colour table (every process's, never emptied) has \(free) free slots and this test "
            + "needs \(required) new entries: \(max(0, used - boot)) entries past the boot's \(boot) were added earlier"
    }
}
