import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// METALMAP d14: the ROP's iterated blend (SET_ITERATED_BLEND 0x0dd0, LOAD_ITERATED_BLEND_INSTRUCTION 0x0df4), which NVN
/// uses for advanced blending. The draw raises ITERATED_BLEND_WITH_PER_TARGET_BLEND (class error 0x5d, NVIDIA
/// open-gpu-doc gv100 dev_graphics.ref.txt) while SET_BLEND_STATE_PER_TARGET is TRUE, which NVK's context init sets
/// and raw's channels replay, and ITERATED_BLEND_TARGET_COUNT (0x59: the bench's draws with two colour targets bound):
/// every arm draws into one target, clears per-target blend and gives the common blend block (0x133c-0x1358) the alpha blend
/// NVN pairs with the program. Programs are NVN's, as Ryujinx's AdvancedBlendFunctions table records them; each ends by
/// writing TEMP0, which is the blend's result. Each program arm sets SET_ITERATED_BLEND_OPTIMIZATION (0x02f4) to NEVER:
/// the facade's source-over states set NOOP_SOURCE_RGBA_0000 (raw_blend_kill.h), whose drop of a zero source is right for
/// the common block's source-over but not for a program that ignores the destination (T-v2's pass arm kept the
/// destination on its row of zero source, 510 fields up to 255 codes off). One arm a command buffer, journaled before it
/// runs, NVN's own sequences first and the arms expected to be refused last; a refusal ends the arm, not the probe. The
/// kernel reports no class error for those arms on the chip, so their pixels say what ran (`pixels=`): the destination
/// untouched (dst), the common block's blend alone (common), the program's pass (pass), or none of them (other).
enum X1IteratedBlendProbe {
    static let size = 256
    static let maxRefusals = 3

    static let perTarget: UInt32 = 0x12e4, separateAlpha: UInt32 = 0x133c, colorOp: UInt32 = 0x1340
    static let colorSource: UInt32 = 0x1344, colorDest: UInt32 = 0x1348, alphaOp: UInt32 = 0x134c
    static let alphaSource: UInt32 = 0x1350, alphaDest: UInt32 = 0x1358, constantRed: UInt32 = 0x1480
    static let add: UInt32 = 0x8006, one: UInt32 = 0x4001, oneMinusSourceAlpha: UInt32 = 0x4303

    struct Arm {
        let name: String
        var program: [UInt32] = []
        var enable: UInt32 = 1
        var alphaDestFactor: UInt32 = oneMinusSourceAlpha
        var constants: [UInt16] = []
        var perTargetOff = true
        var separate: UInt32 = 1
        /// Whether the claim rests on this arm: its words decoded field by field against clb197.h here (overlay's and
        /// plus-clamped's are Ryujinx's words with formulas of ours: reported, not judged)
        var judged = true
        /// RGB of a premultiplied source over a premultiplied destination, (s, sa, d, da) per channel; nil: refusal
        /// expected, no picture judged
        let rgb: ((Double, Double, Double, Double) -> Double)?
        var alpha: (Double, Double) -> Double = { sa, da in sa + da * (1 - sa) }
    }

    static let arms: [Arm] = [
        Arm(name: "common_only", rgb: { s, sa, d, _ in s + d * (1 - sa) }),
        Arm(name: "pass", program: [0x0000_2029], rgb: { s, _, _, _ in s }),
        Arm(
            name: "plus", program: [0x0010_5029], alphaDestFactor: one, rgb: { s, _, d, _ in s + d },
            alpha: { sa, da in sa + da }),
        Arm(
            name: "multiply", program: [0x0000_5001, 0x3021_7001, 0x0060_9029],
            rgb: { s, sa, d, da in s * d + s * (1 - da) + d * (1 - sa) }),
        Arm(
            name: "screen", program: [0x3019_6001, 0x0028_1e09, 0x3021_7001, 0x0060_9029],
            rgb: { s, _, d, _ in s + d - s * d }),
        Arm(
            name: "overlay",
            program: [
                0x3000_0421, 0x2000_c001, 0x3000_0621, 0x1000_c201, 0x8068_c031, 0x0056_ac05, 0x0050_1036,
                0x3058_1036, 0x3064_c806, 0x0060_1036, 0x1021_7001, 0x3000_6401, 0x000d_c801,
            ],
            constants: [0x3800, 0x3800, 0x3800], judged: false,
            rgb: { s, sa, d, da in
                (2 * d <= da ? 2 * s * d : sa * da - 2 * (da - d) * (sa - s)) + s * (1 - da) + d * (1 - sa)
            }),
        Arm(
            name: "plus_clamped_rgba", program: [0x3010_5029, 0x0000_1e11, 0x3030_3029, 0x1280_1e11, 0x0000_9029],
            enable: 3, judged: false, rgb: { s, _, d, _ in min(s + d, 1) }, alpha: { sa, da in min(sa + da, 1) }),
        Arm(
            name: "multiply_two", program: [0x0038_5001, 0x000c_4201],
            rgb: { s, sa, d, da in s * d + s * (1 - da) + d * (1 - sa) }),
        Arm(name: "ends_in_pbr", program: [0x3000_1001], rgb: nil),
        Arm(name: "per_target_left", program: [0x0000_2029], perTargetOff: false, rgb: nil),
        Arm(name: "no_separate_alpha", program: [0x0000_2029], separate: 0, rgb: nil),
    ]

    static func words(_ arm: Arm) -> [(UInt32, UInt32)] {
        var w: [(UInt32, UInt32)] = []
        if arm.perTargetOff { w.append((perTarget, 0)) }
        w += [
            (separateAlpha, arm.separate), (colorOp, add), (colorSource, one), (colorDest, oneMinusSourceAlpha),
            (alphaOp, add), (alphaSource, one), (alphaDest, arm.alphaDestFactor),
        ]
        for (j, c) in arm.constants.enumerated() {
            w.append((constantRed + UInt32(16 * (j / 3) + 4 * (j % 3)), UInt32(c)))
        }
        guard !arm.program.isEmpty else { return w }
        w.append((X1RawProbes.setIteratedBlendOptimization, 0))
        w.append((X1RawProbes.loadIteratedBlendPointer, 0))
        w += arm.program.map { (X1RawProbes.loadIteratedBlendInstruction, $0) }
        return w + [
            (X1RawProbes.setIteratedBlendPass, UInt32(arm.program.count)), (X1RawProbes.setIteratedBlend, arm.enable),
        ]
    }

    /// Legal: PASS keeps its count (0 is INVALID_VALUE), per-target blend back on for raw's own draws
    static let restore: [(UInt32, UInt32)] = [(X1RawProbes.setIteratedBlend, 0), (perTarget, 1)]

    /// The destination's premultiplied codes at (x, y): (blue, green, red, alpha) in memory order.
    static func destination(_ x: Int, _ y: Int) -> [Int] {
        let (r, g, b) = (y, 255 - y, x)
        return [b, g, r, max(r, g, b)]
    }

    static func source(_ x: Int, _ y: Int) -> [Double] {
        let u = Float(x) / 255, a = Float(y) / 255
        let rgba = [u * a, (1 - u) * a, 0.5 * a, a].map { Double(Float16($0)) }
        return [rgba[2], rgba[1], rgba[0], rgba[3]]
    }

    struct Judged {
        var inexact = 0, beyondOne = 0, maxDiff = 0
    }

    static func judge(_ arm: Arm, _ got: [UInt8], rgb: (Double, Double, Double, Double) -> Double) -> Judged {
        var j = Judged()
        for p in 0..<(size * size) {
            let (x, y) = (p % size, p / size)
            let s = source(x, y)
            let d = destination(x, y).map { Double($0) / 255 }
            for c in 0..<4 {
                let value = c == 3 ? arm.alpha(s[3], d[3]) : rgb(s[c], s[3], d[c], d[3])
                let diff = abs(Int(got[p * 4 + c]) - X1Reference.unorm(value, top: 255))
                if diff > 0 { j.inexact += 1 }
                if diff > 1 { j.beyondOne += 1 }
                j.maxDiff = max(j.maxDiff, diff)
            }
        }
        return j
    }

    /// What an arm the class should have refused drew, from its pixels
    static func drew(_ got: [UInt8]) -> String {
        let readings: [(String, Arm)] = [
            ("pass", arms[1]), ("common", arms[0]),
            ("dst", Arm(name: "dst", rgb: { _, _, d, _ in d }, alpha: { _, da in da })),
        ]
        for (name, arm) in readings where judge(arm, got, rgb: arm.rgb!).beyondOne == 0 { return name }
        return "other"
    }

    static let probe = X1Probe(
        id: "gpu.x1.iterated_blend", name: "Iterated blend: NVN's advanced-blend programs with per-target blend off",
        probe: "d14", risky: true, timeout: 180
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.iterated_blend")
        let initial = (0..<size * size).flatMap { destination($0 % size, $0 / size).map(UInt8.init) }
        let state = try gpu.render(fragment: "x1_ramp_source", format: .bgra8Unorm, configure: X1RawProbes.sourceOver)
        var metrics = Metrics()
        metrics.set("rule", "fetch.iterated")
        var paths: [String] = []
        var emitted = 0, refusals = 0
        var outcomes: [String] = []
        var nvnAccepted = 0, nvnWithinOne = 0, nvnRun = 0
        for arm in arms {
            if refusals >= maxRefusals {
                outcomes.append("\(arm.name):skipped")
                continue
            }
            journal.starting(
                "\(arm.name) words=\(words(arm).map { String(format: "%04x=%x", $0.0, $0.1) }.joined(separator: ","))")
            let target = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
            try await gpu.upload(initial, to: target, bytesPerRow: size * 4)
            let isNVN = !arm.program.isEmpty && arm.rgb != nil && arm.judged
            if isNVN { nvnRun += 1 }
            do {
                let drawn = try await RawMethods.draw(gpu, before: words(arm), after: restore) { commands in
                    let encoder = commands.makeRenderCommandEncoder(descriptor: X1RawProbes.pass(target, load: .load))!
                    encoder.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    encoder.endEncoding()
                }
                paths.append(drawn.path)
                emitted += drawn.emitted
                let got = try await gpu.read(target, bytesPerPixel: 4)
                guard let rgb = arm.rgb else {
                    outcomes.append("\(arm.name):accepted:pixels=\(drew(got))")
                    continue
                }
                let j = judge(arm, got, rgb: rgb)
                outcomes.append("\(arm.name):accepted:\(j.inexact):\(j.beyondOne):\(j.maxDiff)")
                if isNVN {
                    nvnAccepted += 1
                    if j.beyondOne == 0 { nvnWithinOne += 1 }
                }
            } catch {
                refusals += 1
                outcomes.append("\(arm.name):refused")
                metrics.set("\(arm.name)_why", LogLine.sanitized(String(String(describing: error).prefix(80))))
            }
        }
        metrics.set("nvn_run", nvnRun)
        metrics.set("nvn_accepted", nvnAccepted)
        metrics.set("nvn_within_one", nvnWithinOne)
        metrics.set("refusals", refusals)
        metrics.set("arms", outcomes.joined(separator: ","))
        let offPath = paths.filter { $0 != "raw" }
        let verdict: ClaimVerdict =
            emitted == 0 || !offPath.isEmpty
            ? .inconclusive : nvnAccepted == nvnRun && nvnWithinOne == nvnRun ? .pass : .fail
        metrics.set("emitted", emitted)
        let note =
            emitted == 0
            ? "the probe's methods reached no draw"
            : !offPath.isEmpty
                ? "not on the raw path: \(offPath.joined(separator: ","))"
                : verdict == .fail ? "NVN's programs refused or beyond one code: see arms" : ""
        return ProbeFinding(verdict, metrics, note: note)
    }
}
