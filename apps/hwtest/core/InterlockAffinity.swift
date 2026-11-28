import Foundation

/// gpu.x1.interlock_affinity's program, readings and verdict. NAK's pixel interlock indexes the driver's ticket buffer
/// as NVIDIA's compiler does (`LOP32I R2 = SR_AFFINITY & 0xff`, `ISCADD (R2 << 8) + (ticket & 0xff)`, `SHL 2`): word
/// (affinity & 0xff) * 256 + (ticket & 0xff) (nak_nir_lower_fs_interlock.c interlock_ticket_word). An affinity byte b
/// therefore reaches (b << 8 | 0xff) * 4 + 4 bytes of the buffer, and a word past its end lands in whatever the GPU VM
/// maps next, with no fault to say so.
public enum InterlockAffinity {
    public static let srAffinity: UInt64 = 0x1c
    public static let srVirtId: UInt64 = 0x03
    public static let width = 1280, height = 720

    /// Each output word is one 16-bit half of a register ORed into 1.0f's bits: a normal float, which RGBA32Float
    /// stores bit for bit, and a mark that the program wrote the pixel
    static let marker: UInt32 = 0x3f80_0000

    static let pt: UInt64 = 7 << 16
    static let nopWord: UInt64 = 0x50b0 << 48 | pt | 0xf << 8
    static let exitWord: UInt64 = 0xe300 << 48 | pt | 0xf

    /// A scheduling slot: stall cycles, the write barrier the instruction sets (7: none), and the barriers it waits on
    static func control(_ stall: UInt64, write: UInt64 = 7, wait: UInt64 = 0) -> UInt64 {
        let noReadBarrier: UInt64 = 7 << 8
        return stall | write << 5 | noReadBarrier | wait << 11
    }

    static func schedule(_ c0: UInt64, _ c1: UInt64, _ c2: UInt64) -> UInt64 { c0 | c1 << 21 | c2 << 42 }

    /// NAK sm50.rs OpS2R: opcode 0xf0c8, the special register at bits 20..27
    static func s2r(_ d: UInt64, _ sr: UInt64) -> UInt64 {
        let opcode: UInt64 = 0xf0c8 << 48
        return opcode | sr << 20 | pt | d
    }

    /// NAK sm50.rs OpLop2 with a 32-bit immediate: opcode 0x0400, the operation at bits 53..54 (0 AND, 1 OR)
    static func lop32i(_ d: UInt64, _ a: UInt64, _ imm: UInt64, or: Bool = false) -> UInt64 {
        let opcode: UInt64 = 0x0400 << 48
        let operation: UInt64 = or ? 1 << 53 : 0
        return opcode | operation | imm << 20 | pt | a << 8 | d
    }

    /// NAK sm50.rs OpShr with an immediate shift: opcode 0x3828
    static func shr(_ d: UInt64, _ a: UInt64, _ shift: UInt64) -> UInt64 {
        let opcode: UInt64 = 0x3828 << 48
        return opcode | shift << 20 | pt | a << 8 | d
    }

    /// tools/benchloop/sm53/affinity.py code(second): R4 = SR_AFFINITY, R5 = `second`, then colour 0's R, G, B, A
    /// = R4's low and high halves and R5's, each ORed into 1.0f. The S2Rs set write barriers 0 and 1 (a
    /// variable-latency result), and the NOP after them waits on every barrier.
    public static func code(second: UInt64) -> [UInt64] {
        let mark = UInt64(marker)
        return [
            schedule(control(1, write: 0), control(2, write: 1), control(15, wait: 0x3f)),
            s2r(4, srAffinity), s2r(5, second), nopWord,
            schedule(control(1), control(1), control(1)),
            lop32i(0, 4, 0xffff), shr(1, 4, 16), lop32i(2, 5, 0xffff),
            schedule(control(15), control(1), control(1)),
            shr(3, 5, 16), lop32i(0, 0, mark, or: true), lop32i(1, 1, mark, or: true),
            schedule(control(1), control(15), control(13, wait: 0x3f)),
            lop32i(2, 2, mark, or: true), lop32i(3, 3, mark, or: true), exitWord,
            schedule(control(2), control(1), control(1)), nopWord, nopWord, nopWord,
        ]
    }

    /// A pixel program's header (SPH v3 type 2, shader type 5, MRT, SASS version 1), position W read as the GPU
    /// requires and colour 0's RGBA written: apps/hwtest/bench/bench_known.h kKnownSph
    public static let sph: [UInt32] = {
        let typePixel: UInt32 = 2, version: UInt32 = 3 << 5, shaderPixel: UInt32 = 5 << 10
        let mrt: UInt32 = 1 << 14, sass: UInt32 = 1 << 17
        var header = [UInt32](repeating: 0, count: 20)
        header[0] = typePixel | version | shaderPixel | mrt | sass
        header[5] = 1 << 31
        header[18] = 0xf
        return header
    }()

    /// The full-screen draw twice: the second register SR_AFFINITY again (a reading on its own, should the other trap),
    /// then SR_VIRTID, whose SM and warp fields are the control
    public static func experiments() throws -> [BenchExperiment] {
        let sphBytes = sph.withUnsafeBytes { Data($0) }.base64EncodedString()
        let draws: [(id: String, second: UInt64)] = [("affinity.screen", srAffinity), ("affinity.virtid", srVirtId)]
        let list = draws.map { draw -> [String: Any] in
            let words = code(second: draw.second)
            let program: [String: Any] = [
                "sph": sphBytes, "code": words.withUnsafeBytes { Data($0) }.base64EncodedString(),
                "data": Data(count: 16).base64EncodedString(), "gprs": 6,
            ]
            return [
                "id": draw.id, "kind": "fragment", "readback": "full", "program": program,
                "target": ["format": "rgba32float", "width": width, "height": height] as [String: Any],
            ]
        }
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "interlock-affinity", "experiments": list,
        ]
        let entries = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch)).entries
        return try entries.map { entry -> BenchExperiment in
            switch entry {
            case .experiment(let e): return e
            case .invalid(let id, _, let why): throw TestFailure("\(id) does not decode: \(why)")
            }
        }
    }

    /// The bytes of the ticket buffer the interlock reaches with affinity byte `byte`: its last word's end
    public static func bytesReached(byte: UInt32) -> Int { Int(byte << 8 | 0xff) * 4 + 4 }

    /// The largest affinity byte whose ticket words a buffer of `ticketBytes` holds; -1 for none
    public static func largestByte(fitting ticketBytes: Int) -> Int { ticketBytes / 1024 - 1 }

    public struct Pair: Hashable, Sendable {
        public let first: UInt32, second: UInt32

        public init(_ first: UInt32, _ second: UInt32) { (self.first, self.second) = (first, second) }
    }

    /// One draw's texels: pixels by their two registers, and those without the program's mark in every word
    public struct Reading: Equatable, Sendable {
        public var pixels = 0, unmarked = 0
        public var pairs: [Pair: Int] = [:]

        public init() {}

        public var affinity: [UInt32: Int] { pairs.reduce(into: [:]) { $0[$1.key.first, default: 0] += $1.value } }
        public var second: [UInt32: Int] { pairs.reduce(into: [:]) { $0[$1.key.second, default: 0] += $1.value } }
        public var lowBytes: [UInt32] { Set(pairs.keys.map { $0.first & 0xff }).sorted() }
        public var secondDiffers: Int { pairs.filter { $0.key.first != $0.key.second }.values.reduce(0, +) }
        /// SR_VIRTID's SM (bits 20..28) and warp (bits 8..14) fields, as NAK reads them (nak_nir.c load_sm_id_nv)
        public var sms: [UInt32: Int] {
            pairs.reduce(into: [:]) { $0[$1.key.second >> 20 & 0x1ff, default: 0] += $1.value }
        }
        public var warps: Int { Set(pairs.keys.map { $0.second >> 8 & 0x7f }).count }
        public var affinityBySm: [UInt32: Set<UInt32>] {
            pairs.keys.reduce(into: [:]) { $0[$1.second >> 20 & 0x1ff, default: []].insert($1.first) }
        }
    }

    /// RGBA32Float texels, 16 bytes each
    public static func read(_ texels: Data) -> Reading {
        var reading = Reading()
        texels.withUnsafeBytes { raw in
            for at in stride(from: 0, to: raw.count - 15, by: 16) {
                let r = raw.loadUnaligned(fromByteOffset: at, as: UInt32.self)
                let g = raw.loadUnaligned(fromByteOffset: at + 4, as: UInt32.self)
                let b = raw.loadUnaligned(fromByteOffset: at + 8, as: UInt32.self)
                let a = raw.loadUnaligned(fromByteOffset: at + 12, as: UInt32.self)
                reading.pixels += 1
                guard (r | g | b | a) & 0xffff_0000 == marker, r & g & b & a & 0xffff_0000 == marker else {
                    reading.unmarked += 1
                    continue
                }
                let pair = Pair(r & 0xffff | (g & 0xffff) << 16, b & 0xffff | (a & 0xffff) << 16)
                reading.pairs[pair, default: 0] += 1
            }
        }
        return reading
    }

    /// "value:pixels" for the most common `limit` values, then "+<n>" for the rest
    public static func histogram(_ counts: [UInt32: Int], limit: Int = 8) -> String {
        let sorted = counts.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
        var text = sorted.prefix(limit).map { "0x" + String($0.key, radix: 16) + ":\($0.value)" }.joined(separator: ",")
        if sorted.count > limit { text += ",+\(sorted.count - limit)" }
        return text.isEmpty ? "-" : text
    }

    /// The claim: every affinity byte the draws saw fits the ticket buffer, FAIL naming the largest that does not. The
    /// readings count only where S2R provably reached the target: every pixel carries the program's mark, the two
    /// SR_AFFINITY reads agree, and SR_VIRTID shows one SM id per TPC GetInfo counts and more than one warp, or
    /// failing that SR_AFFINITY itself takes more than one value across the screen. Bytes that fit without that are
    /// INCONCLUSIVE.
    public static func judge(
        screen: Result<Reading, TestFailure>, control: Result<Reading, TestFailure>, tpcs: Int, ticketBytes: Int
    ) -> ProbeFinding {
        var metrics = Metrics()
        metrics.set("tpcs", tpcs)
        metrics.set("ticket_bytes", ticketBytes)
        metrics.set("byte_limit", largestByte(fitting: ticketBytes))
        let readings = [screen, control].compactMap { try? $0.get() }
        let bytes = Array(Set(readings.flatMap(\.lowBytes))).sorted()
        let unmarked = readings.map(\.unmarked).reduce(0, +)
        var blocking = report(screen: screen, into: &metrics)
        let (virtidHeld, controlNotes) = report(control: control, tpcs: tpcs, into: &metrics)
        let affinityVaries = ((try? screen.get())?.affinity.count ?? 0) > 1
        metrics.set("control", virtidHeld ? "virtid" : affinityVaries ? "affinity_varies" : "none")
        metrics.set("unmarked_px", unmarked)
        if unmarked > 0 { blocking.append("\(unmarked) px without the program's mark") }
        if !virtidHeld && !affinityVaries { blocking.append("no control held") }
        if let low = bytes.first, let high = bytes.last {
            metrics.set("affinity_low_min", Int(low))
            metrics.set("affinity_low_max", Int(high))
            metrics.set("bytes_reached", bytesReached(byte: high))
        }
        if ticketBytes > 0, let worst = bytes.last(where: { bytesReached(byte: $0) > ticketBytes }) {
            let line = "affinity byte 0x\(String(worst, radix: 16)) reaches \(bytesReached(byte: worst)) bytes of the "
                + "\(ticketBytes)-byte ticket buffer (TegraGM20BMetal/gpu.cpp Device::init, allocControl of tickets_)"
            return ProbeFinding(.fail, metrics, note: ([line] + blocking + controlNotes).joined(separator: "; "))
        }
        if ticketBytes <= 0 { blocking.insert("no ticket buffer size from the bench's device", at: 0) }
        if bytes.isEmpty { blocking.append("no affinity read") }
        let verdict: ClaimVerdict = blocking.isEmpty ? .pass : .inconclusive
        return ProbeFinding(verdict, metrics, note: (blocking + controlNotes).joined(separator: "; "))
    }

    /// The screen draw's fields; what stops it proving anything
    static func report(screen: Result<Reading, TestFailure>, into metrics: inout Metrics) -> [String] {
        switch screen {
        case .success(let r):
            metrics.set("affinity_px", r.pixels)
            metrics.set("affinity_values", histogram(r.affinity))
            metrics.set("second_read_differs", r.secondDiffers)
            return r.secondDiffers > 0 ? ["\(r.secondDiffers) px read SR_AFFINITY twice and got two values"] : []
        case .failure(let why):
            return ["affinity draw: \(why)"]
        }
    }

    /// The control draw's fields, SR_VIRTID's whole values too, so a run whose fields sit elsewhere shows where;
    /// whether it held, and why not
    static func report(
        control: Result<Reading, TestFailure>, tpcs: Int, into metrics: inout Metrics
    ) -> (held: Bool, notes: [String]) {
        let r: Reading
        switch control {
        case .success(let reading): r = reading
        case .failure(let why): return (false, ["control draw: \(why)"])
        }
        metrics.set("virtid_values", histogram(r.second))
        metrics.set("sm_ids", histogram(r.sms))
        metrics.set("warp_ids", r.warps)
        let perSm = r.affinityBySm.sorted { $0.key < $1.key }.map { sm -> String in
            "\(sm.key)=" + sm.value.sorted().map { "0x" + String($0, radix: 16) }.joined(separator: "/")
        }
        metrics.set("affinity_by_sm", perSm.joined(separator: ","))
        var notes: [String] = []
        if r.sms.count != tpcs || tpcs < 1 {
            notes.append("SR_VIRTID shows \(r.sms.count) SM ids where GetInfo counts \(tpcs) TPCs")
        }
        if r.warps < 2 { notes.append("SR_VIRTID shows \(r.warps) warp ids") }
        return (notes.isEmpty && r.unmarked == 0, notes)
    }
}
