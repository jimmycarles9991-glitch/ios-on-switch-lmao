import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// barrier.switch_drain (raw/plan plan_barriers.cpp, nvk_cmd_buffer.c:678-687): a change of subchannel between the 3D
/// engine and the copy engine, the 2D engine or compute waits for the engine left behind, so no WAIT_FOR_IDLE is needed
/// there; the tiled flush and the NO_WFI invalidates stay (nowfi), or the tiled flush goes too (noflush: are the tiled
/// cache's contents visible to the other engine without it). Each trial, in one command buffer: pass A seeds target T,
/// then the copy engine copies T to U, or the 2D engine copies it 1:1, or a compute kernel reads T's codes into a
/// buffer, then (copy and 2D) pass B copies U's bottom-right texels into an atlas tile. Targets rotate between two, so
/// an early or stale read copies another seed's texels. The atlas is read back in a command buffer of its own, so the
/// readback's own engine change is not the arm's. Arms: control (today), nowfi, noflush, none, and hazard, the
/// positive control: pass B taps T straight after pass A with no barrier between (3D to 3D), which the pass barrier
/// probe saw corrupt at these sizes; a cell judges the switch arms only once hazard differed there. Every arm draws pass
/// A with the same spins, so the control's race is the arms' race.
enum X1SwitchDrainProbe {
    static let probe = X1Probe(
        id: "gpu.x1.subchannel_switch_drain", name: "A subchannel change drains the engine left behind",
        probe: "barrier.switch_drain", timeout: 300, body: run)

    static let arms = [
        ("hazard", "pass=none"), ("control", "switch=today"), ("nowfi", "switch=nowfi"),
        ("noflush", "switch=nowfi_noflush"), ("none", "switch=none"),
    ]
    static let batch = 8

    static func spins(from trial: Int, size: Int) -> [Int] {
        (0..<batch).map { [0, 3, 17, 40][(trial + $0) % 4] * (size == 1280 ? 1 : 2) }
    }

    enum Engine: String {
        case ce, twod, compute
    }

    struct Cell {
        let engine: Engine
        let format: MTLPixelFormat
        let width: Int, height: Int
    }

    static var cells: [Cell] {
        var out: [Cell] = []
        for engine in [Engine.ce, .twod, .compute] {
            for format in [MTLPixelFormat.bgra8Unorm, .rgba16Float] {
                for (w, h) in [(64, 64), (320, 180)] {
                    out.append(Cell(engine: engine, format: format, width: w, height: h))
                }
            }
        }
        out.append(Cell(engine: .ce, format: .bgra8Unorm, width: 1280, height: 720))
        return out
    }

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        let journal = ProbeJournal(probe: "gpu.x1.subchannel_switch_drain")
        var ledger = ProbeLedger()
        var kits: [MTLPixelFormat: BarrierKit] = [:]
        var ops = 0
        for (index, cell) in cells.enumerated() {
            let kit = try kits[cell.format] ?? BarrierKit(gpu, cell.format)
            kits[cell.format] = kit
            let size = cell.width == 1280 ? "f" : cell.width == 64 ? "s" : "m"
            let name = "\(cell.engine.rawValue)_\(kit.name)_\(size)"
            let work = try Work(kit, cell)
            journal.starting("\(name) arms=\(arms.map(\.0).joined(separator: "/"))")
            var expected: [Int: [UInt8]] = [:]
            let started = Date()
            let plan = IterationPlan(cells: arms.count, target: cell.width == 1280 ? 600 : 1504, batch: batch)
            var trial = [Int](repeating: 0, count: arms.count)
            for step in plan.order {
                if Date().timeIntervalSince(started) > (cell.width == 1280 ? 9 : 5) && step.cell == 0 { break }
                let (arm, forms) = arms[step.cell]
                if arm == "hazard" && step.round > 4 && !ledger.tally(name, arm).clean { continue }
                let first = trial[step.cell]
                let seeds = (0..<batch).map { probeSeed(trial: first + $0, cell: index, seeds: 16) }
                let spins = spins(from: first, size: cell.width)
                let hazard = arm == "hazard"
                let ran = try await gpu.run(forms: forms) { commands in
                    try work.encode(commands, seeds: seeds, spins: spins, hazard: hazard)
                }
                ops += ran.taken["ops"] ?? 0
                if hazard || cell.engine != .compute {
                    _ = try await gpu.run { commands in
                        kit.readback(commands, work.atlas, into: hazard ? work.hazardOut : work.out)
                    }
                }
                var tally = ledger.tally(name, arm)
                for (k, seed) in seeds.enumerated() {
                    if hazard {
                        work.tapExpected(seed).withUnsafeBytes {
                            tally.compare(
                                kit.tile(work.hazardOut, k), $0, trial: first + k, width: BarrierKit.tile,
                                bytesPerTexel: kit.bytesPerTexel)
                        }
                        continue
                    }
                    if expected[seed] == nil { expected[seed] = work.expected(seed) }
                    expected[seed]!.withUnsafeBytes {
                        tally.compare(
                            work.result(k), $0, trial: first + k, width: work.resultWidth,
                            bytesPerTexel: kit.bytesPerTexel)
                    }
                }
                trial[step.cell] += batch
                ledger.store(name, arm, tally, ran)
            }
        }
        var finding = ledger.finding(
            relaxed: ["nowfi", "noflush", "none"], none: "hazard", control: "control",
            takenKey: "switches", rule: "barrier.switch_drain")
        let fired = ledger.cells.filter { !(ledger.tallies[$0]?["hazard"]?.clean ?? true) }
        finding.metrics.set("hazard_cells", fired.count)
        if fired.isEmpty {
            finding.verdict = .inconclusive
            finding.note = "the positive control never differed: no cell could see a race"
        }
        finding.metrics.set("twod_ops", ops)
        finding.metrics.set(
            "tiled_cache", try RawProbe.facts(gpu.device, try gpu.texture(.bgra8Unorm, 8, 8))["tiled_cache"] ?? -1)
        let control = ledger.cells.compactMap { ledger.tallies[$0]?["control"]?.mismatchedTrials }.reduce(0, +)
        finding.metrics.set("control_mismatched_trials", control)
        if control > 0 {
            finding.verdict = .inconclusive
            finding.note = "today's barriers gave texels other than the CPU's reading"
        }
        return finding
    }

    struct Work {
        let kit: BarrierKit
        let cell: Cell
        let sources: [MTLTexture], copies: [MTLTexture]
        let atlas: MTLTexture
        let out: MTLBuffer, hazardOut: MTLBuffer
        let codes: MTLComputePipelineState?

        init(_ kit: BarrierKit, _ cell: Cell) throws {
            self.kit = kit
            self.cell = cell
            let gpu = kit.gpu
            hazardOut = try gpu.buffer(kit.tileBytes * X1SwitchDrainProbe.batch)
            sources = try (0..<2).map { _ in try gpu.texture(cell.format, cell.width, cell.height) }
            copies =
                cell.engine == .compute
                ? [] : try (0..<2).map { _ in try gpu.texture(cell.format, cell.width, cell.height) }
            atlas = try kit.atlas(X1SwitchDrainProbe.batch)
            let texels = cell.engine == .compute ? cell.width * cell.height : BarrierKit.tile * BarrierKit.tile
            out = try gpu.buffer(texels * kit.bytesPerTexel * X1SwitchDrainProbe.batch)
            codes =
                cell.engine == .compute
                ? try gpu.compute(cell.format == .rgba16Float ? "x1_texture_codes_half" : "x1_texture_codes_unorm")
                : nil
        }

        var resultWidth: Int { cell.engine == .compute ? cell.width : BarrierKit.tile }
        var resultBytes: Int {
            (cell.engine == .compute ? cell.width * cell.height : BarrierKit.tile * BarrierKit.tile) * kit.bytesPerTexel
        }

        func result(_ k: Int) -> UnsafeRawBufferPointer {
            UnsafeRawBufferPointer(start: out.contents() + k * resultBytes, count: resultBytes)
        }

        var base: SIMD2<Int32> {
            SIMD2(Int32(max(0, cell.width - BarrierKit.tile)), Int32(max(0, cell.height - BarrierKit.tile)))
        }

        func tapExpected(_ seed: Int) -> [UInt8] {
            kit.expectedTap(seed: seed, base: base, step: SIMD2(1, 1), width: cell.width, height: cell.height)
        }

        func expected(_ seed: Int) -> [UInt8] {
            guard cell.engine == .compute else { return tapExpected(seed) }
            var bytes: [UInt8] = []
            bytes.reserveCapacity(resultBytes)
            for y in 0..<cell.height {
                for x in 0..<cell.width {
                    if cell.format == .rgba16Float {
                        bytes += kit.texel(x, y, seed: seed)
                    } else {
                        bytes += withUnsafeBytes(
                            of: probeHash(UInt32(x), UInt32(y), UInt32(seed)).littleEndian, Array.init)
                    }
                }
            }
            return bytes
        }

        func encode(_ commands: MTLCommandBuffer, seeds: [Int], spins: [Int], hazard: Bool) throws {
            for (k, seed) in seeds.enumerated() {
                let source = sources[k % 2]
                kit.seed(commands, source, seed: seed, spin: spins[k])
                if hazard {
                    kit.tap(commands, source, into: atlas, index: k, base: base, step: SIMD2(1, 1))
                    continue
                }
                switch cell.engine {
                case .ce, .twod:
                    let copy = copies[k % 2]
                    let blit = commands.makeBlitCommandEncoder()!
                    if cell.engine == .ce {
                        blit.copy(
                            from: source, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                            sourceSize: MTLSize(width: cell.width, height: cell.height, depth: 1), to: copy,
                            destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
                    } else {
                        try RawProbe.engine(
                            commands, ["kind": "scale", "read": source, "written": copy, "unbracketed": 1])
                    }
                    blit.endEncoding()
                    kit.tap(commands, copy, into: atlas, index: k, base: base, step: SIMD2(1, 1))
                case .compute:
                    let encoder = commands.makeComputeCommandEncoder()!
                    encoder.setComputePipelineState(codes!)
                    encoder.setTexture(source, index: 0)
                    encoder.setBuffer(out, offset: k * resultBytes, index: 0)
                    encoder.dispatchThreads(
                        MTLSize(width: cell.width, height: cell.height, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
                    encoder.endEncoding()
                }
            }
        }
    }
}
