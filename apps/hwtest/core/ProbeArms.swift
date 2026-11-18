import Foundation

/// How many clean trials a probe needs to catch a race of a given rate: a race that shows once in `1 / rate` trials
/// escapes N trials with probability (1 - rate)^N.
public enum ProbeConfidence {
    /// The trials that catch a race of `rate` with probability `confidence`: ln(1 - c) / ln(1 - rate), rounded up.
    public static func trials(rate: Double, confidence: Double) -> Int {
        Int((log(1 - confidence) / log(1 - rate)).rounded(.up))
    }

    /// The probability that `trials` clean trials would have shown a race of `rate`.
    public static func confidence(trials: Int, rate: Double) -> Double {
        1 - pow(1 - rate, Double(trials))
    }

    /// The highest rate `trials` clean trials leave open at 95%: 1 - 0.05^(1/N), about 3 / N.
    public static func bound95(trials: Int) -> Double {
        trials > 0 ? 1 - pow(0.05, 1 / Double(trials)) : 1
    }
}

/// One arm of one cell against its reference, trial by trial: how many trials differed, the bytes that did, and the
/// first difference as "trial:x,y:got>want" (bytes in hex, the texel's first bytes).
public struct ArmTally: Sendable, Equatable {
    public private(set) var trials = 0
    public private(set) var mismatchedTrials = 0
    public private(set) var mismatchedBytes = 0
    public private(set) var first: String?
    public var gpuMilliseconds = 0.0
    public var batches = 0

    public init() {}

    public var clean: Bool { mismatchedTrials == 0 }

    /// `got` and `want` are one trial's texels, rows `width` texels of `bytesPerTexel` packed.
    public mutating func compare(
        _ got: UnsafeRawBufferPointer, _ want: UnsafeRawBufferPointer, trial: Int, width: Int,
        bytesPerTexel: Int
    ) {
        trials += 1
        let count = min(got.count, want.count)
        guard count != got.count || count != want.count || memcmp(got.baseAddress!, want.baseAddress!, count) != 0
        else { return }
        mismatchedTrials += 1
        var wrong = abs(got.count - want.count)
        var firstAt = -1
        for i in 0..<count where got[i] != want[i] {
            wrong += 1
            if firstAt < 0 { firstAt = i }
        }
        mismatchedBytes += wrong
        if first == nil, firstAt >= 0 {
            let texel = firstAt / bytesPerTexel
            let start = texel * bytesPerTexel
            let hex = { (b: UnsafeRawBufferPointer) in
                (start..<min(start + bytesPerTexel, b.count)).map { String(format: "%02x", b[$0]) }.joined()
            }
            first = "\(trial):\(texel % width),\(texel / width):\(hex(got))>\(hex(want))"
        }
    }

    public mutating func compare(_ got: [UInt8], _ want: [UInt8], trial: Int, width: Int, bytesPerTexel: Int) {
        got.withUnsafeBytes { g in
            want.withUnsafeBytes { w in compare(g, w, trial: trial, width: width, bytesPerTexel: bytesPerTexel) }
        }
    }
}

/// What one cell says about a relaxed form: it held (matched the control in every trial while the arm with no
/// barrier differed at least once, so the cell can see the hazard), it failed (differed), or the cell is blind (no
/// arm differed: nothing it ran could have shown the hazard).
public enum CellVerdict: String, Sendable {
    case holds = "H"
    case fails = "F"
    case blind = "B"

    public static func of(relaxed: ArmTally, unguarded: ArmTally?) -> CellVerdict {
        if !relaxed.clean { return .fails }
        if let unguarded, !unguarded.clean { return .holds }
        return .blind
    }

    /// The probe's verdict over its cells: FAIL when any failed; PASS when none failed and the blind cells are a strict
    /// minority, which the line names (the rule is proven for the held cells' shapes only); INCONCLUSIVE otherwise.
    public static func claim(_ cells: [CellVerdict]) -> ClaimVerdict {
        if cells.contains(.fails) { return .fail }
        let blind = cells.filter { $0 == .blind }.count
        return !cells.isEmpty && blind * 2 < cells.count ? .pass : .inconclusive
    }
}

/// A probe's trials over its cells within a time budget: each cell gets `target` trials in batches of `batch`, the
/// cells taken in turn so a budget that runs out leaves every cell with some, never some cells with none.
public struct IterationPlan: Sendable {
    public let cells: Int
    public let target: Int
    public let batch: Int

    public init(cells: Int, target: Int, batch: Int) {
        self.cells = cells
        self.target = target
        self.batch = max(1, batch)
    }

    /// Batches per cell for the target.
    public var rounds: Int { (target + batch - 1) / batch }

    /// (cell, round) in the order they run: round by round across the cells.
    public var order: [(cell: Int, round: Int)] {
        (0..<rounds).flatMap { round in (0..<cells).map { (cell: $0, round: round) } }
    }
}

/// A seed per trial that differs from the trial before's, so a read of stale memory sees another seed's texels:
/// one of `seeds` (at least 2, not a multiple of 7), the cell's own offset.
public func probeSeed(trial: Int, cell: Int, seeds: Int) -> Int {
    (trial * 7 + cell * 3) % seeds
}

/// The texel hash the probes' seeded passes draw (X1Probes.metal x1_hash), for the CPU's reference.
public func probeHash(_ x: UInt32, _ y: UInt32, _ seed: UInt32) -> UInt32 {
    var v = (x &* 73_856_093) ^ (y &* 19_349_663) ^ (seed &* 83_492_791)
    v ^= v >> 13
    v = v &* 0x5bd1_e995
    v ^= v >> 15
    return v
}
