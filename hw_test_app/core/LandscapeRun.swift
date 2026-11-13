import Foundation

/// The display's landscape run as HW Test reads it (drivers/NativeMobileDisplay/SwitchLandscapeRunABI.h,
/// little-endian): a sample of frame ends with DC A's CRC and mode at each, and what scanned and what it cost over it.
public struct LandscapeSample: Sendable, Equatable {
    public static let magic: UInt32 = 0x444c_5356
    public static let force: UInt32 = 1
    public static let sample: UInt32 = 2
    public static let query: UInt32 = 3
    public static let done: UInt32 = 2
    public static let maxFrames = 240
    public static let size = 96 + 5 * maxFrames
    public static let columnScanYuv: UInt32 = 2
    static let landscapeBit: UInt8 = 1
    static let switchingBit: UInt8 = 2

    public var state: UInt32 = 0
    public var error: Int32 = 0
    public var traits: UInt32 = 0
    public var switches = 0
    public var overlayLatches = 0
    public var uiLatches = 0
    public var underflows = [0, 0, 0]
    /// By who asked: Core Animation's display server (backboardd, the route under test), this test's own process,
    /// everyone else (SWITCH_DISPLAY_LANDSCAPE_ORIGIN_*).
    public var vicJobs = [0, 0, 0]
    public var vicTurns = [0, 0, 0]
    public var vicEventTurns = [0, 0, 0]
    public static let displayServer = 0
    public static let test = 1
    public static let other = 2
    public var displayNs: UInt64 = 0
    public var vicNs: UInt64 = 0
    public var crc: [UInt32] = []
    public var mode: [UInt8] = []

    public init() {}

    /// The request's 16 bytes: magic, op, value, 0.
    public static func request(op: UInt32, value: UInt32) -> [UInt8] {
        [magic, op, value, 0].flatMap { word in (0..<4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) } }
    }

    public init?(_ bytes: [UInt8]) {
        guard bytes.count == Self.size, Self.word(bytes, 0) == Self.magic else { return nil }
        state = Self.word(bytes, 4)
        let frames = min(Int(Self.word(bytes, 8)), Self.maxFrames)
        error = Int32(bitPattern: Self.word(bytes, 12))
        traits = Self.word(bytes, 16)
        switches = Int(Self.word(bytes, 20))
        overlayLatches = Int(Self.word(bytes, 24))
        uiLatches = Int(Self.word(bytes, 28))
        underflows = (0..<3).map { Int(Self.word(bytes, 32 + 4 * $0)) }
        vicJobs = (0..<3).map { Int(Self.word(bytes, 44 + 4 * $0)) }
        vicTurns = (0..<3).map { Int(Self.word(bytes, 56 + 4 * $0)) }
        vicEventTurns = (0..<3).map { Int(Self.word(bytes, 68 + 4 * $0)) }
        displayNs = UInt64(Self.word(bytes, 80)) | UInt64(Self.word(bytes, 84)) << 32
        vicNs = UInt64(Self.word(bytes, 88)) | UInt64(Self.word(bytes, 92)) << 32
        crc = (0..<frames).map { Self.word(bytes, 96 + 4 * $0) }
        mode = Array(bytes[(96 + 4 * Self.maxFrames)..<(96 + 4 * Self.maxFrames + frames)])
    }

    public var frames: Int { crc.count }
    public var landscapeFrames: Int { mode.filter { $0 & Self.landscapeBit != 0 }.count }
    public var allLandscape: Bool { !mode.isEmpty && landscapeFrames == mode.count }
    public var noLandscape: Bool { landscapeFrames == 0 }
    public var underflowFrames: Int { underflows.reduce(0, +) }

    /// The CRC the sample ends on when its last `count` readings agree (a held picture), else nil. DC A's CRC lags the
    /// latch by up to two frame ends, and 0 is never a reading (the CRC unit just switched on reads 0 for a while).
    public func settled(last count: Int = 10) -> UInt32? {
        let readings = crc.filter { $0 != 0 }
        guard readings.count >= count, let last = readings.last else { return nil }
        return readings.suffix(count).allSatisfy { $0 == last } ? last : nil
    }

    /// Readings (after the CRC unit's first two frames) that are neither `before` nor `after`: a frame the change of
    /// mode showed half done, or black.
    public func strayFrames(between before: UInt32, and after: UInt32) -> Int {
        crc.dropFirst(2).filter { $0 != 0 && $0 != before && $0 != after }.count
    }

    static func word(_ bytes: [UInt8], _ at: Int) -> UInt32 {
        (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }
    }
}

/// display.x1.landscape_video's verdict from its five samples: a held frame on the VIC-turned route, the change into
/// landscape on that same frame, the video playing in landscape, the held frame in landscape again, and the change
/// back. A held frame changes its CRC only when the route changes, so every frame of a change must show one route's
/// CRC or the other's.
public enum LandscapeJudge {
    public struct Samples: Sendable {
        public var vicHeld = LandscapeSample()
        public var enter = LandscapeSample()
        public var playing = LandscapeSample()
        public var landscapeHeld = LandscapeSample()
        public var leave = LandscapeSample()

        public init() {}
    }

    public static func judge(_ s: Samples, dropped: Int) -> ([(String, Bool)], Metrics) {
        var metrics = Metrics()
        let vic = s.vicHeld.settled()
        let column = s.enter.settled()
        let back = s.leave.settled()
        metrics.set("crc_vic", vic.map { String(format: "%08x", $0) } ?? "unsettled")
        metrics.set("crc_landscape", column.map { String(format: "%08x", $0) } ?? "unsettled")
        if let vic, let column { metrics.set("crc_same", vic == column) }
        let enterStray = vic.flatMap { v in column.map { s.enter.strayFrames(between: v, and: $0) } } ?? -1
        let leaveStray = vic.flatMap { v in column.map { s.leave.strayFrames(between: $0, and: v) } } ?? -1
        metrics.set("stray_enter", enterStray)
        metrics.set("stray_leave", leaveStray)
        let videoFrames = max(s.playing.overlayLatches, 1)
        let server = LandscapeSample.displayServer
        metrics.set("vic_jobs_per_frame", Double(s.playing.vicJobs[server]) / Double(videoFrames), digits: 3)
        metrics.set("vic_turns", s.playing.vicTurns[server])
        metrics.set("vic_event_turns", s.playing.vicEventTurns[server] + s.enter.vicEventTurns[server])
        metrics.set("vic_jobs_test", s.playing.vicJobs[LandscapeSample.test])
        metrics.set("vic_jobs_other", s.playing.vicJobs[LandscapeSample.other])
        metrics.set("video_frames", s.playing.overlayLatches)
        metrics.set("switches", s.enter.switches + s.leave.switches)
        let all = [s.vicHeld, s.enter, s.playing, s.landscapeHeld, s.leave]
        for (i, window) in ["a", "b", "c"].enumerated() {
            metrics.set("underflows_\(window)", all.reduce(0) { $0 + $1.underflows[i] })
        }
        metrics.set("dropped", dropped)
        let frames = Double(max(s.playing.frames, 1))
        metrics.set("display_cpu_us_per_frame", Double(s.playing.displayNs) / frames / 1000)
        metrics.set("vic_cpu_us_per_frame", Double(s.playing.vicNs) / frames / 1000)
        let checks: [(String, Bool)] = [
            ("the VIC route holds one frame (crc_vic settled, portrait)", vic != nil && s.vicHeld.noLandscape),
            (
                "the display went landscape on that frame (switches, crc_landscape settled)",
                column != nil && s.enter.switches >= 1 && s.enter.mode.last.map { $0 & 1 != 0 } == true
            ),
            ("no stray frame going landscape (stray_enter 0)", enterStray == 0),
            ("landscape held while the video played", s.playing.allLandscape),
            (
                "no VIC job from the display server for the landscape video (vic_jobs_per_frame 0)",
                s.playing.overlayLatches > 0 && s.playing.vicJobs[server] == 0
            ),
            (
                "the display server's requests were recorded as quarter turns (vic_turns)",
                s.playing.vicTurns[server] > 0
            ),
            (
                "the held frame scans the same in landscape again",
                column != nil && s.landscapeHeld.settled() == column && s.landscapeHeld.allLandscape
            ),
            (
                "back to the VIC route's very frame (crc_vic again, stray_leave 0)",
                back != nil && back == vic && leaveStray == 0 && s.leave.mode.last.map { $0 & 1 == 0 } == true
            ),
            ("no window underflowed", all.allSatisfy { $0.underflowFrames == 0 }),
            ("no frame dropped", dropped == 0),
        ]
        return (checks, metrics)
    }
}
