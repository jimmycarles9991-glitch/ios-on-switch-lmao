import Foundation

/// A display window probe's reply (drivers/NativeMobileDisplay/DisplayWindowProbeABI.h, little-endian): 32 bytes of
/// header, a 64-byte reason, then up to eight 28-byte steps. The kernel judges the claim (DisplayWindowProbeRun.h
/// judge); this carries its verdict and each step's CRC onto the test's line.
public struct DisplayWindowProbeReply: Sendable {
    public struct Step: Sendable, Equatable {
        public let name: String
        public let crc: UInt32
        public let settled: Bool
        public let underflows: Int
        public let changes: Int
        public let mismatches: Int
        public let check: Int
        public let against: Int

        public var clean: Bool { settled && underflows == 0 && changes == 0 && mismatches == 0 }
    }

    public static let magic: UInt32 = 0x4457_5052
    public static let size = 320
    public static let start: UInt32 = 1
    public static let query: UInt32 = 2
    public static let done: UInt32 = 2
    static let stepsOffset = 96
    static let stepSize = 28
    static let checkEqual = 1
    static let checkDiffer = 2
    static let checkReport = 3

    public let probe: UInt32
    public let state: UInt32
    public let verdict: UInt32
    public let restored: Bool
    public let frames: Int
    public let error: Int32
    public let why: String
    public let steps: [Step]

    /// The request's 16 bytes: magic, op, probe, 0.
    public static func request(op: UInt32, probe: UInt32) -> [UInt8] {
        [magic, op, probe, 0].flatMap { word in (0..<4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) } }
    }

    public init?(_ bytes: [UInt8]) {
        guard bytes.count == Self.size, Self.word(bytes, 0) == Self.magic else { return nil }
        probe = Self.word(bytes, 4)
        state = Self.word(bytes, 8)
        verdict = Self.word(bytes, 12)
        let count = min(Int(Self.word(bytes, 16)), (Self.size - Self.stepsOffset) / Self.stepSize)
        restored = Self.word(bytes, 20) != 0
        frames = Int(Self.word(bytes, 24))
        error = Int32(bitPattern: Self.word(bytes, 28))
        why = Self.text(bytes, 32, 64)
        steps = (0..<count).map { i in
            let at = Self.stepsOffset + i * Self.stepSize
            return Step(
                name: Self.text(bytes, at + 12, 16), crc: Self.word(bytes, at), settled: bytes[at + 4] != 0,
                underflows: Int(bytes[at + 5]), changes: Int(bytes[at + 6]), mismatches: Int(bytes[at + 7]),
                check: Int(bytes[at + 8]), against: Int(bytes[at + 9]))
        }
    }

    /// claim_verdict from the kernel's judgement; each step's CRC (<name>_crc), and for each judged or reported step
    /// whether it showed its reference's CRC (<name>_same) and, when it was not stable, underflow-free and as written,
    /// what was wrong (<name>_unclean: unsettled/underflows/changes/mismatches).
    public var finding: ProbeFinding {
        var metrics = Metrics()
        metrics.set("restored", restored)
        metrics.set("frames", frames)
        for step in steps {
            metrics.set("\(step.name)_crc", String(format: "%08x", step.crc))
            if [Self.checkEqual, Self.checkDiffer, Self.checkReport].contains(step.check), step.against < steps.count {
                metrics.set("\(step.name)_same", step.crc == steps[step.against].crc)
            }
            if !step.clean {
                metrics.set(
                    "\(step.name)_unclean",
                    "\(step.settled ? 1 : 0)/\(step.underflows)/\(step.changes)/\(step.mismatches)")
            }
        }
        let claim: ClaimVerdict = verdict == 0 ? .pass : verdict == 1 ? .fail : .inconclusive
        return ProbeFinding(claim, metrics, note: claim == .pass ? "" : why)
    }

    static func word(_ bytes: [UInt8], _ at: Int) -> UInt32 {
        (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }
    }

    static func text(_ bytes: [UInt8], _ at: Int, _ count: Int) -> String {
        let field = bytes[at..<(at + count)]
        return String(decoding: field.prefix { $0 != 0 }, as: UTF8.self)
    }
}
