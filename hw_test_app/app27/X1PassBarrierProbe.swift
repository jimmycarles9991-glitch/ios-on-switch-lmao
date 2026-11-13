import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// barrier.pass_pixel (raw/plan plan_hazard.cpp CrossPassPixel): between pass A, which renders into a target, and
/// pass B, which samples it, the tiled flush A ends with and PIXEL_SHADER_BARRIER with its pixel-ordered texture
/// invalidate (0x0f74) order B's reads after A's writes, with no WAIT_FOR_IDLE or shader-cache flush. Each trial is A
/// then B in one command buffer, eight a batch; the targets rotate between two, so a read too early, or through a
/// texture cache still holding the last trial's lines, copies another seed's texels. Arms: control (today's
/// switch_gm20b_pass_barrier), pixel (the planner's form), pixel_nowfi (0x1288 after the barrier), none (no barrier:
/// the arm that shows a cell can see the hazard). Cells: format x layout x read shape x size. F-v1 read pixel_nowfi wrong
/// in every 64 x 64 cell (bgra8_bl_same_s 623 of 1,008 trials); T-v2, with CROP/ZROP counting both L2 slices (regdump
/// 0x408958 = 0x408850 = 2), read it clean in all 16 held cells on 144 trials at least, so the 64 x 64 cells run four
/// times as long (the F-v1 count).
enum X1PassBarrierProbe {
    static let probe = X1Probe(
        id: "gpu.x1.pass_pixel_barrier",
        name: "A pixel barrier between passes orders a pass's samples after the last's",
        probe: "barrier.pass_pixel", timeout: 420, body: run)

    static let arms = [
        ("control", "pass=today"), ("pixel", "pass=pixel"), ("pixel_nowfi", "pass=pixel_nowfi"),
        ("none", "pass=none"),
    ]
    static let batch = 8
    static let seeds = 16

    enum Shape: String {
        case same, stride, flip, pyramid, waw
    }

    struct Cell {
        let format: MTLPixelFormat
        let layout: BarrierKit.Layout
        let shape: Shape
        let width: Int, height: Int
        let trials: Int
        let seconds: Double

        var size: String { width == 1280 ? "f" : width == 64 ? "s" : "m" }
    }

    static var cells: [Cell] {
        var out: [Cell] = []
        for format in [MTLPixelFormat.bgra8Unorm, .rgba16Float] {
            for shape in [Shape.same, .stride, .flip, .pyramid] {
                out.append(
                    Cell(
                        format: format, layout: .blockLinear, shape: shape, width: 64, height: 64, trials: 1504,
                        seconds: 8))
                out.append(
                    Cell(
                        format: format, layout: .blockLinear, shape: shape, width: 320, height: 180,
                        trials: 1504, seconds: shape == .stride ? 14 : 4))
            }
            out.append(
                Cell(
                    format: format, layout: .blockLinear, shape: .stride, width: 1280, height: 720,
                    trials: 600, seconds: 36))
            out.append(
                Cell(
                    format: format, layout: .blockLinear, shape: .pyramid, width: 1280, height: 720,
                    trials: 600, seconds: 9))
            for layout in [BarrierKit.Layout.pitch, .compressed] {
                for shape in [Shape.same, .pyramid] {
                    out.append(
                        Cell(
                            format: format, layout: layout, shape: shape, width: 320, height: 180,
                            trials: 1504, seconds: 4))
                }
            }
            out.append(
                Cell(
                    format: format, layout: .blockLinear, shape: .waw, width: 320, height: 180, trials: 1504,
                    seconds: 4))
        }
        return out
    }

    /// Pass A's extra work per texel, rotating per trial. T-v2's none arm fired in 16 cells and was blind in 14, all of
    /// them 320 x 180 or larger, where B's taps meet texels A finished long before: there A spins up to three times longer
    static func spinLadder(_ cell: Cell) -> [Int] {
        switch cell.size {
        case "s": return [0, 6, 34, 80]
        case "m": return [0, 12, 60, 240]
        default: return [0, 3, 17, 120]
        }
    }

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        let journal = ProbeJournal(probe: "gpu.x1.pass_pixel_barrier")
        var ledger = ProbeLedger()
        var skipped: [String] = []
        var controlVsCpu = 0
        var kits: [MTLPixelFormat: BarrierKit] = [:]
        for (index, cell) in cells.enumerated() {
            let kit = try kits[cell.format] ?? BarrierKit(gpu, cell.format)
            kits[cell.format] = kit
            let name = "\(kit.name)_\(cell.layout.rawValue)_\(cell.shape.rawValue)_\(cell.size)"
            guard let work = try CellWork(kit, cell) else {
                skipped.append(name)
                continue
            }
            journal.starting("\(name) arms=\(arms.map(\.0).joined(separator: "/"))")
            var references: [Int: [UInt8]] = [:]
            let started = Date()
            let plan = IterationPlan(cells: arms.count, target: cell.trials, batch: batch)
            var trial = [Int](repeating: 0, count: arms.count)
            for step in plan.order {
                if Date().timeIntervalSince(started) > cell.seconds && step.cell == 0 { break }
                let (arm, forms) = arms[step.cell]
                if arm == "none" && !ledger.tally(name, arm).clean && step.round > 4 { continue }
                let first = trial[step.cell]
                let trialSeeds = (0..<batch).map { probeSeed(trial: first + $0, cell: index, seeds: seeds) }
                let spins = (0..<batch).map { spinLadder(cell)[(first + $0) % 4] }
                let ran = try await gpu.run(forms: forms) { commands in
                    work.encode(commands, seeds: trialSeeds, spins: spins)
                }
                var tally = ledger.tally(name, arm)
                for k in 0..<batch {
                    let got = kit.tile(work.readback, k)
                    let seed = trialSeeds[k]
                    if arm == "control" {
                        if references[seed] == nil {
                            references[seed] = Array(got)
                            if let expected = work.expected(seed: seed), !expected.elementsEqual(got) {
                                controlVsCpu += 1
                            }
                        }
                    }
                    guard let want = references[seed] else { continue }
                    want.withUnsafeBytes {
                        tally.compare(
                            got, $0, trial: first + k, width: BarrierKit.tile, bytesPerTexel: kit.bytesPerTexel)
                    }
                }
                trial[step.cell] += batch
                ledger.store(name, arm, tally, ran)
            }
        }
        var finding = ledger.finding(
            relaxed: ["pixel", "pixel_nowfi"], none: "none", control: "control",
            takenKey: "passes", rule: "barrier.pass_pixel")
        finding.metrics.set("control_vs_cpu", controlVsCpu)
        finding.metrics.set("skipped", skipped.isEmpty ? "-" : skipped.joined(separator: ","))
        if controlVsCpu > 0 {
            finding.verdict = .inconclusive
            finding.note = "the control's reads are not the CPU's reading of the pattern"
        }
        return finding
    }

    /// One cell's targets and what a batch of trials draws through them.
    struct CellWork {
        let kit: BarrierKit
        let cell: Cell
        let targets: [MTLTexture]
        let views: [MTLTexture]
        let levels: [MTLTexture]
        let atlas: MTLTexture
        let readback: MTLBuffer

        init?(_ kit: BarrierKit, _ cell: Cell) throws {
            self.kit = kit
            self.cell = cell
            var targets: [MTLTexture] = []
            for _ in 0..<2 {
                guard let t = try kit.target(cell.width, cell.height, cell.layout) else { return nil }
                targets.append(t)
            }
            self.targets = targets
            views = cell.shape == .waw ? targets.compactMap { $0.makeTextureView(pixelFormat: kit.format) } : []
            var levels: [MTLTexture] = []
            if cell.shape == .pyramid {
                var (w, h) = (cell.width, cell.height)
                for _ in 0..<3 {
                    (w, h) = (max(1, w / 2), max(1, h / 2))
                    levels.append(try kit.gpu.texture(kit.format, w, h, usage: [.renderTarget, .shaderRead]))
                }
            }
            self.levels = levels
            atlas = try kit.atlas(X1PassBarrierProbe.batch)
            readback = try kit.gpu.buffer(kit.tileBytes * X1PassBarrierProbe.batch)
        }

        /// The tap's base and step for the cell's shape over a source of width x height
        func taps(_ width: Int, _ height: Int) -> (SIMD2<Int32>, SIMD2<Int32>) {
            let t = BarrierKit.tile
            let stride = SIMD2<Int32>(Int32(max(1, width / t)), Int32(max(1, height / t)))
            switch cell.shape {
            case .same, .waw: return (SIMD2(Int32(max(0, width - t)), Int32(max(0, height - t))), SIMD2(1, 1))
            case .stride:
                // The last tap is the bottom-right texel, which pass A writes last: from the top-left the taps end
                // short of A's last strip at 320 x 180 and 1280 x 720 and never meet the hazard.
                let end = SIMD2<Int32>(Int32(width - 1), Int32(height - 1)) &- stride &* Int32(t - 1)
                return (SIMD2(max(0, end.x), max(0, end.y)), stride)
            case .pyramid: return (SIMD2(0, 0), stride)
            case .flip: return (SIMD2(Int32(width - 1), Int32(height - 1)), SIMD2<Int32>(0, 0) &- stride)
            }
        }

        func encode(_ commands: MTLCommandBuffer, seeds: [Int], spins: [Int]) {
            for (k, seed) in seeds.enumerated() {
                let target = targets[k % 2]
                kit.seed(commands, target, seed: seed, spin: spins[k])
                var source = target
                if cell.shape == .waw, k % 2 < views.count {
                    let t = BarrierKit.tile
                    kit.seed(
                        commands, views[k % 2], seed: seed ^ 0x5555, spin: 0,
                        window: (cell.width - t, cell.height - t, cell.width, cell.height))
                }
                for level in levels {
                    kit.level(commands, source, into: level)
                    source = level
                }
                let (base, step) = taps(source.width, source.height)
                kit.tap(commands, source, into: atlas, index: k, base: base, step: step)
            }
            kit.readback(commands, atlas, into: readback)
        }

        /// The CPU's reading where the tile copies texels of one seed's pattern
        func expected(seed: Int) -> [UInt8]? {
            guard cell.shape != .pyramid && cell.shape != .waw else { return nil }
            let (base, step) = taps(cell.width, cell.height)
            return kit.expectedTap(seed: seed, base: base, step: step, width: cell.width, height: cell.height)
        }
    }
}
