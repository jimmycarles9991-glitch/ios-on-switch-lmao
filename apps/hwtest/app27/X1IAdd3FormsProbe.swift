import Foundation
import HWTestCore
import HWTestKit

/// Which of IADD3's operand negations GM20B runs (bits 49, 50 and 51 negate C, B and A in every form). The
/// constant-buffer form with A and B negated (0x4ccc000800270707) raised ILLEGAL_INSTR_ENCODING in pixel programs on
/// the device, while envytools, Ryujinx and maxas decode three independent negations and plain IADD reads its two as
/// .PO. Each form is a warp's kernel on a bench raw device of its own, so a trap kills that channel alone: R5 = ±a ± b
/// ± c over R5 holding a sentinel, b from R3 or c[1][0x14], stored with a marker after it. `<form>=ran:<R5>:<R5 as
/// ±a±b±c+k>`, ran:unmarked:<R5> when the marker is missing, or trapped:e<the channel's error>. The unnegated form
/// runs first and must store a+b+c, else the kernel is wrong and the probe fails; the forms that negate A and B
/// together run last. claim_verdict=PASS: the register form with C negated and with A and C negated, which NAK emits,
/// store their exact sums.
enum X1IAdd3FormsProbe {
    struct Form {
        let name: String
        let cbuf: Bool
        let negA: Bool, negB: Bool, negC: Bool
    }

    static let forms: [Form] = [
        Form(name: "plain", cbuf: false, negA: false, negB: false, negC: false),
        Form(name: "r_nc", cbuf: false, negA: false, negB: false, negC: true),
        Form(name: "r_na_nc", cbuf: false, negA: true, negB: false, negC: true),
        Form(name: "r_nb_nc", cbuf: false, negA: false, negB: true, negC: true),
        Form(name: "c_na_nc", cbuf: true, negA: true, negB: false, negC: true),
        Form(name: "c_nb", cbuf: true, negA: false, negB: true, negC: false),
        Form(name: "c_nb_nc", cbuf: true, negA: false, negB: true, negC: true),
        Form(name: "r_na_nb", cbuf: false, negA: true, negB: true, negC: false),
        Form(name: "c_na_nb", cbuf: true, negA: true, negB: true, negC: false),
    ]
    static let emitted = ["r_nc", "r_na_nc", "r_nb_nc", "c_na_nc"]

    /// Every ±a±b±c and its neighbours within two are distinct, so negation, bitwise NOT and .PO read apart
    static let a: UInt32 = 0x1000, b: UInt32 = 0x100, c: UInt32 = 0x10
    static let sentinel: UInt32 = 0x5e5e_5e5e, marker: UInt32 = 0x3c3c_3c3c
    /// c[1]: the output's address in bytes 0-7 (the bench writes it), then a, b, c, the sentinel and the marker
    static let data: [UInt32] = [0, 0, 0, 0, a, b, c, sentinel, marker, 0, 0, 0]
    static let aAt: UInt64 = 0x10, bAt: UInt64 = 0x14, cAt: UInt64 = 0x18, sentinelAt: UInt64 = 0x1c
    static let markerAt: UInt64 = 0x20

    static let regA: UInt64 = 2, regB: UInt64 = 3, regC: UInt64 = 4, dst: UInt64 = 5, markerReg: UInt64 = 6
    static let pt: UInt64 = 7 << 16
    static let dataBank: UInt64 = 1
    static let movCbufOp: UInt64 = 0x4c98 << 48, allLanes: UInt64 = 0xf << 39
    static let iadd3Reg: UInt64 = 0x5cc0 << 48, iadd3Cbuf: UInt64 = 0x4cc0 << 48
    static let stg32: UInt64 = 0xeedc << 48, extendedAddress: UInt64 = 1 << 45
    static let exitWord: UInt64 = 0xe300 << 48 | 7 << 16 | 0xf
    static let nopWord: UInt64 = 0x50b0 << 48 | 7 << 16 | 0xf << 8

    /// An instruction's scheduling bits: its delay, no barriers set, and with `waitAll` a wait on all six
    static func control(_ delay: UInt64, waitAll: Bool = false) -> UInt64 {
        let noBarriers: UInt64 = 7 << 5 | 7 << 8
        return delay | noBarriers | (waitAll ? 0x3f << 11 : 0)
    }

    static func schedule(_ c0: UInt64, _ c1: UInt64, _ c2: UInt64) -> UInt64 { c0 | c1 << 21 | c2 << 42 }

    static func cbuf(_ offset: UInt64) -> UInt64 { dataBank << 34 | (offset / 4) << 20 }

    static func movCbuf(_ d: UInt64, _ offset: UInt64) -> UInt64 { movCbufOp | allLanes | cbuf(offset) | pt | d }

    static func iadd3(_ f: Form) -> UInt64 {
        let operandB = f.cbuf ? iadd3Cbuf | cbuf(bAt) : iadd3Reg | regB << 20
        var word = operandB | regC << 39 | pt | regA << 8 | dst
        if f.negC { word |= 1 << 49 }
        if f.negB { word |= 1 << 50 }
        if f.negA { word |= 1 << 51 }
        return word
    }

    /// STG.E.32 [R0 + offset]: R0 and R1 hold the output's address
    static func stg(_ source: UInt64, offset: UInt64) -> UInt64 { stg32 | extendedAddress | offset << 20 | pt | source }

    static func code(_ f: Form) -> Data {
        let words: [UInt64] = [
            schedule(control(1), control(1), control(1)), movCbuf(0, 0), movCbuf(1, 4), movCbuf(regA, aAt),
            schedule(control(1), control(1), control(1)), movCbuf(regB, bAt), movCbuf(regC, cAt),
            movCbuf(dst, sentinelAt),
            schedule(control(15), control(15), control(1)), movCbuf(markerReg, markerAt), iadd3(f),
            stg(dst, offset: 0),
            schedule(control(1), control(13, waitAll: true), control(2)), stg(markerReg, offset: 4), exitWord, nopWord,
        ]
        return words.withUnsafeBytes { Data($0) }
    }

    static func sum(_ negA: Bool, _ negB: Bool, _ negC: Bool) -> UInt32 {
        (negA ? 0 &- a : a) &+ (negB ? 0 &- b : b) &+ (negC ? 0 &- c : c)
    }

    /// `value` as ±a±b±c plus at most two, e.g. "-a-b+c+1"; "?" when it is none of them
    static func reading(_ value: UInt32) -> String {
        for k: Int32 in [0, 1, -1, 2, -2] {
            for signs in 0 ..< 8 {
                let (na, nb, nc) = (signs & 4 != 0, signs & 2 != 0, signs & 1 != 0)
                guard sum(na, nb, nc) &+ UInt32(bitPattern: k) == value else { continue }
                let terms = (na ? "-a" : "a") + (nb ? "-b" : "+b") + (nc ? "-c" : "+c")
                return terms + (k == 0 ? "" : k > 0 ? "+\(k)" : "\(k)")
            }
        }
        return "?"
    }

    static func experiments() throws -> [BenchExperiment] {
        let shaderData = data.withUnsafeBytes { Data($0) }.base64EncodedString()
        let target: [String: Any] = ["format": "rgba32float", "width": 1, "height": 1]
        let list: [[String: Any]] = forms.map { form -> [String: Any] in
            let compute: [String: Any] = [
                "code": code(form).base64EncodedString(), "data": shaderData, "gprs": 8, "threads": [32, 1, 1],
                "groups": [1, 1, 1], "output": 16,
            ]
            return [
                "id": "iadd3." + form.name, "kind": "compute", "target": target, "readback": "none", "compute": compute,
            ]
        }
        let batch: [String: Any] = ["protocol": BenchProtocol.version, "batch": "iadd3-forms", "experiments": list]
        return try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch)).entries.map {
            entry -> BenchExperiment in
            switch entry {
            case .experiment(let e): return e
            case .invalid(let id, _, let why): throw TestFailure("\(id) does not decode: \(why)")
            }
        }
    }

    struct Ran {
        let cell: String
        let exact: Bool
    }

    /// One form on a bench raw device of its own; any status but ok or a kill while it ran (a commit the kernel refused
    /// reads killed too) is the probe's fault and throws
    static func measure(_ form: Form, _ experiment: BenchExperiment) throws -> Ran {
        let runner: BenchRunner
        do {
            runner = try BenchRunner.open()
        } catch {
            throw TestFailure("the bench did not open for \(form.name): \(error)")
        }
        let outcome = runner.run(experiment)
        switch outcome.status {
        case "ok":
            guard let output = outcome.output, output.count >= 8 else {
                throw TestFailure("\(form.name) ran and returned no output")
            }
            let value = output.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) }
            let stored = output.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: UInt32.self) }
            let hex = String(format: "0x%08x", value)
            guard stored == marker else { return Ran(cell: "ran:unmarked:\(hex)", exact: false) }
            return Ran(cell: "ran:\(hex):\(reading(value))", exact: value == sum(form.negA, form.negB, form.negC))
        case "killed" where outcome.why != "the kernel refused the submission":
            let at = outcome.faultVA == 0 ? "" : String(format: "@0x%llx", outcome.faultVA)
            return Ran(cell: "trapped:e\(outcome.lostError)\(at)", exact: false)
        default:
            throw TestFailure("\(form.name) \(outcome.status): \(outcome.why)")
        }
    }

    static let probe = X1Probe(
        id: "gpu.x1.iadd3_forms", name: "IADD3's operand negations: the register and constant-buffer forms GM20B runs",
        probe: "iadd3", risky: true
    ) { _ in
        let journal = ProbeJournal(probe: "gpu.x1.iadd3_forms")
        let batch = try experiments()
        var metrics = Metrics()
        metrics.set("inputs", String(format: "a:0x%x,b:0x%x,c:0x%x", a, b, c))
        var exact: Set<String> = []
        var cells: [String: String] = [:]
        var measured: [String] = []
        var trapped: [String] = []
        for (form, experiment) in zip(forms, batch) {
            journal.starting("\(form.name) word=\(String(iadd3(form), radix: 16))")
            let ran: Ran
            do {
                ran = try measure(form, experiment)
            } catch {
                let before = measured.isEmpty ? "-" : measured.joined(separator: ",")
                throw TestFailure("\(error); measured before: \(before)")
            }
            if form.name == "plain" && !ran.exact {
                throw TestFailure("the unnegated IADD3 kernel did not store a+b+c (\(ran.cell)): the kernel is wrong")
            }
            if ran.exact { exact.insert(form.name) }
            if ran.cell.hasPrefix("trapped") { trapped.append(form.name) }
            cells[form.name] = ran.cell
            measured.append("\(form.name)=\(ran.cell)")
            metrics.set(form.name, ran.cell)
        }
        metrics.set("trapped", trapped.isEmpty ? "-" : trapped.joined(separator: ","))
        var notes: [String] = trapped.isEmpty ? [] : ["trapped: \(trapped.joined(separator: ","))"]
        if !trapped.contains("c_na_nb") { notes.append("c_na_nb ran in a kernel, where pixel programs trapped") }
        let missed = emitted.filter { !exact.contains($0) }
        guard missed.isEmpty else {
            let why = missed.map { "\($0) \(cells[$0] ?? "-")" }.joined(separator: ", ")
            notes.insert("NAK's forms missed their sums: \(why)", at: 0)
            return ProbeFinding(.fail, metrics, note: notes.joined(separator: "; "))
        }
        return ProbeFinding(.pass, metrics, note: notes.joined(separator: "; "))
    }
}
