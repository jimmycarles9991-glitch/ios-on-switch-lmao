import Foundation

/// The render server's glass memory probe (DEVELOPMENT builds) as two notify state words: `mem`
/// (com.brunostjohn.switch.debug.glass-mem) counts what it found in the compositor's command buffers, `pool`
/// (com.brunostjohn.switch.debug.glass-mem.pool) describes the render server's GPU pool. A release build publishes
/// neither and both read 0.
public struct GlassMemWords: Equatable, Sendable {
    public let mem: UInt64
    public let pool: UInt64

    public init(mem: UInt64, pool: UInt64) {
        self.mem = mem
        self.pool = pool
    }

    public var published: Bool { mem != 0 || pool != 0 }
}

/// One bit field of a probe word: a counter that wraps at its width, or one that stops at its maximum.
public struct WordField: Sendable {
    public let shift: UInt64
    public let bits: UInt64
    public let wraps: Bool

    public var maximum: UInt64 { (1 << bits) - 1 }

    public func value(_ word: UInt64) -> UInt64 { word >> shift & maximum }

    /// How far the counter went from `before` to `after`; nil for a saturating counter already at its maximum, whose
    /// rise cannot show. A saturating counter that went down started again (the render server restarted).
    public func rise(_ before: UInt64, _ after: UInt64) -> UInt64? {
        let (from, to) = (value(before), value(after))
        if wraps { return (to &- from) & maximum }
        if from == maximum { return nil }
        return to >= from ? to - from : to
    }
}

/// What the glass-mem word's counters did over one phase.
public struct GlassMemRise: Equatable, Sendable {
    public static let commandBuffers = WordField(shift: 0, bits: 20, wraps: true)
    /// Samples of a reused blur texture whose contents changed with no write since.
    public static let changed = WordField(shift: 20, bits: 12, wraps: false)
    public static let nonFinite = WordField(shift: 32, bits: 12, wraps: false)
    /// Textures whose memory or texture header moved since they were last written.
    public static let moved = WordField(shift: 44, bits: 12, wraps: false)
    /// Samples of a reused texture reading all zero (black).
    public static let zero = WordField(shift: 56, bits: 8, wraps: false)

    public let commandBuffers: UInt64
    public let changed: UInt64?
    public let nonFinite: UInt64?
    public let moved: UInt64?
    public let zero: UInt64?

    public init(_ before: UInt64, _ after: UInt64) {
        commandBuffers = Self.commandBuffers.rise(before, after) ?? 0
        changed = Self.changed.rise(before, after)
        nonFinite = Self.nonFinite.rise(before, after)
        moved = Self.moved.rise(before, after)
        zero = Self.zero.rise(before, after)
    }

    public var faults: Bool { [changed, nonFinite, moved].contains { ($0 ?? 0) > 0 } }

    /// "cbs/changed/nonfinite/moved/zero", "?" where a saturated counter cannot show a rise
    public var text: String {
        let counts = [changed, nonFinite, moved, zero].map { $0.map(String.init) ?? "?" }
        return ([String(commandBuffers)] + counts).joined(separator: "/")
    }
}

/// The glass-mem.pool word: gauges of the render server's GPU pool and a wrapping count of compressed buffers made.
public struct GlassPool: Equatable, Sendable {
    public let megabytes: UInt64
    public let chunks: UInt64
    /// Uncompressed backings held, and their megabytes.
    public let backings: UInt64
    public let backingMegabytes: UInt64
    /// Compressed buffers held, and made since the render server opened the device (wrapping).
    public let compressed: UInt64
    public let made: UInt64

    public init(word: UInt64) {
        megabytes = WordField(shift: 0, bits: 12, wraps: false).value(word)
        chunks = WordField(shift: 12, bits: 8, wraps: false).value(word)
        backings = WordField(shift: 20, bits: 8, wraps: false).value(word)
        backingMegabytes = WordField(shift: 28, bits: 12, wraps: false).value(word)
        compressed = WordField(shift: 40, bits: 8, wraps: false).value(word)
        made = WordField(shift: 48, bits: 16, wraps: true).value(word)
    }

    /// "MB,chunks,backings,backingMB,compressed,made"
    public var text: String {
        [megabytes, chunks, backings, backingMegabytes, compressed, made].map(String.init).joined(separator: ",")
    }

    /// The driver's idle reclaim shows between two reads as backings, their megabytes or the pool's chunks going
    /// down, or as compressed buffers made again afterwards.
    public static func reclaimed(from before: GlassPool, to later: GlassPool) -> Bool {
        later.backings < before.backings || later.backingMegabytes < before.backingMegabytes
            || later.chunks < before.chunks || later.made != before.made
    }
}
