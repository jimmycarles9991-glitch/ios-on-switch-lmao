import Foundation

/// gpu.x1.l2_flush_writeback's arms, comparison and verdict. The GPU's L2 is not coherent with the CPU, and every CPU
/// read of L2-cached GPU writes, like the display's completion promise, rests on a host L2_FLUSH_DIRTY having written
/// the dirty lines back (drivers/TegraGM20B/gpu_exec.c chan_fence_job). The kernel issues it as MEM_OP_C/D (clb06f.h:
/// "MEM_OP_A and MEM_OP_B have been removed for gm20x"); deko3d issues it on the same chip as MEM_OP_B. Each round the
/// 3D engine draws a pattern over a write-combined pitch target, an arm ends the job, and the CPU reads the target
/// through its uncached mapping with no lease, so no other flush stands between the GPU's writes and the read.
public enum L2Writeback {
    public enum Arm: String, CaseIterable, Sendable {
        /// No methods, an Internal job end: what the L2 holds at the end stays there (the control)
        case noFlush = "no_flush"
        /// WFI and FB_FLUSH with no MEM_OP: whether the flush arms' bracket alone writes the L2 back
        case bracket
        /// The bracket around MEM_OP_C 0, MEM_OP_D L2_FLUSH_DIRTY, the kernel's words
        case memOpD = "mem_op_d"
        /// No methods, an External job end: the kernel's own flush where it issues it
        case jobEnd = "job_end"
        /// The bracket around MEM_OP_B L2_FLUSH_DIRTY, deko3d's form. It runs last: a PBDMA that rejects the method
        /// kills the channel
        case memOpB = "mem_op_b"

        /// The arm claims a writeback, so each of its reads must be exact
        public var flushes: Bool { self == .memOpD || self == .jobEnd || self == .memOpB }
        /// Its methods ride the second pass's draw
        public var carriesMethods: Bool { self == .bracket || self == .memOpD || self == .memOpB }
    }

    /// 64 KiB of RGBA32Float, 512 lines of 128 bytes: a quarter of the 256 KiB L2 public Tegra X1 specifications give
    /// GM20B (nothing in this tree states its size), so no line needs to leave the L2 before the job ends
    public static let width = 64, height = 64
    public static let rounds = 8

    /// Normal floats (exponent 0x80, 2.0 to 4.0) distinct for each arm, round and channel
    public static func pattern(_ arm: Arm, round: Int) -> [UInt32] { words(0x4000_0000, arm, round) }

    /// What the CPU fills the target with first (8.0 to 16.0): a word still holding it never left the L2
    public static func sentinel(_ arm: Arm, round: Int) -> [UInt32] { words(0x4100_0000, arm, round) }

    static func words(_ base: UInt32, _ arm: Arm, _ round: Int) -> [UInt32] {
        let armBits = UInt32(Arm.allCases.firstIndex(of: arm) ?? 0) << 16
        let roundBits = UInt32(round & 0xff) << 8
        return (0 ..< 4).map { base | armBits | roundBits | UInt32($0) }
    }

    public struct Comparison: Equatable, Sendable {
        public var words = 0, wrong = 0
        /// Wrong words that hold the CPU's fill
        public var sentinel = 0
        public var firstWrong: Int?
        public var firstSeen: UInt32 = 0

        public init() {}
    }

    /// `words`: RGBA32Float texels, four words each, against `pattern` in every texel
    public static func compare(_ words: [UInt32], pattern: [UInt32], sentinel: [UInt32]) -> Comparison {
        var c = Comparison()
        c.words = words.count
        for (i, word) in words.enumerated() where word != pattern[i & 3] {
            c.wrong += 1
            if word == sentinel[i & 3] { c.sentinel += 1 }
            if c.firstWrong == nil {
                c.firstWrong = i
                c.firstSeen = word
            }
        }
        return c
    }

    public struct Tally: Equatable, Sendable {
        public var rounds = 0, staleRounds = 0, wordsOff = 0, sentinelWords = 0
        /// The first stale round's first wrong word: its byte offset in the target's packed rows, and what it held
        public var firstOffset: Int?
        public var firstSeen: UInt32 = 0
        /// Why the arm stopped (a refused or killed round, its methods reaching no draw)
        public var failure: String?

        public init() {}

        public mutating func add(_ c: Comparison) {
            rounds += 1
            guard c.wrong > 0 else { return }
            staleRounds += 1
            wordsOff += c.wrong
            sentinelWords += c.sentinel
            if firstOffset == nil, let first = c.firstWrong {
                firstOffset = first * 4
                firstSeen = c.firstSeen
            }
        }
    }

    static func hex(_ value: Int) -> String { "0x" + String(value, radix: 16) }

    static func exact(_ tally: Tally?, rounds: Int) -> Bool {
        guard let tally else { return false }
        return tally.failure == nil && tally.rounds == rounds && tally.staleRounds == 0
    }

    /// Every flush arm reads exact in every round, or FAIL with its words off and first offset. Proven only where both
    /// controls went stale at least once: the no-flush end shows the L2 held the writes at the job's end (not a
    /// write-through), and the bracket shows WFI and FB_FLUSH alone do not write them back, so the MEM_OP did. The
    /// kernel's own job end is the positive control for the hand-emitted words: mem_op_d stale where job_end is exact
    /// is the probe's placement, not the hardware, and leaves the claim INCONCLUSIVE.
    public static func judge(_ tallies: [Arm: Tally], rounds: Int = L2Writeback.rounds) -> ProbeFinding {
        var metrics = Metrics()
        var failed: [String] = []
        var unproven: [String] = []
        metrics.set("rounds", rounds)
        for arm in Arm.allCases {
            let t = tallies[arm] ?? Tally()
            let key = arm.rawValue
            metrics.set("\(key)_stale", "\(t.staleRounds)/\(t.rounds)")
            metrics.set("\(key)_words_off", t.wordsOff)
            if let offset = t.firstOffset {
                metrics.set("\(key)_first_off", hex(offset))
                metrics.set("\(key)_first_seen", "0x" + String(t.firstSeen, radix: 16))
                metrics.set("\(key)_fill_words", t.sentinelWords)
            }
            if let failure = t.failure { metrics.set("\(key)_failed", failure) }
            let ran = t.failure.map { ": \($0)" } ?? ": ran \(t.rounds) of \(rounds) rounds"
            if arm.flushes {
                if t.failure != nil || t.rounds < rounds {
                    failed.append(key + ran)
                } else if t.staleRounds > 0 {
                    let first = t.firstOffset.map { hex($0) } ?? "-"
                    failed.append(
                        "\(key): \(t.wordsOff) words off in \(t.staleRounds)/\(rounds) rounds, first at byte \(first)"
                    )
                }
            } else if t.failure != nil || t.rounds < rounds {
                unproven.append("control \(key)" + ran)
            } else if t.staleRounds == 0 {
                let control = arm == .noFlush ? "the control" : "the WFI and FB_FLUSH bracket alone"
                unproven.append("unproven: \(control) read exact")
            }
        }
        if (tallies[.memOpD]?.staleRounds ?? 0) > 0, exact(tallies[.jobEnd], rounds: rounds) {
            let lead = "the hand-emitted methods ran before the pattern reached the L2 "
                + "(mem_op_d stale, job_end exact): mem_op_b unjudged"
            return ProbeFinding(.inconclusive, metrics, note: ([lead] + failed + unproven).joined(separator: "; "))
        }
        if !failed.isEmpty { return ProbeFinding(.fail, metrics, note: (failed + unproven).joined(separator: "; ")) }
        return ProbeFinding(unproven.isEmpty ? .pass : .inconclusive, metrics, note: unproven.joined(separator: "; "))
    }
}
