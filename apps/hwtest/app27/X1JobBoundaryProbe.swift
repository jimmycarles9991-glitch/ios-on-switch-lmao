import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// barrier.entry_no_drain (raw/plan plan_barriers.cpp entry): a job that starts after the one before it ended (its end
/// fence released with RELEASE_WFI, after the kernel's WAIT_FOR_IDLE and L2 flush) needs no flush or wait at its entry,
/// only invalidates of what it reads (planner) or none of the data caches at all (bare). Each trial: job 0 reads a small
/// texture or buffer (so its lines sit in the caches), job 1 or the CPU or the copy channel writes it with a new seed,
/// job 2 reads it; every job of an arm opens with the arm's entry and no barrier before its first encoder, and its
/// trials (eight a job, each its own texture or buffer) share one pass or one concurrent compute encoder, so no
/// barrier inside a job drops what job 0 left in the caches (job 1's passes write separate targets: none between
/// them, as the planner places none where nothing depends). A trial that
/// reads a line job 0 left copies the last seed. Writers: the ROP, the copy engine, the SMs' stores, the CPU, the copy
/// channel's replaceRegion (a second GPU channel). Readers: the texture unit, the SMs' data cache, the constant cache, and
/// the copy engine reading what the SMs stored (sto_ce: what the planner's entry drops against today's, the flush of the
/// SMs' data, shows only where something outside the SMs reads their stores). hdr_tex is the hazard every entry but
/// entry_none invalidates for: between job 0 and job 2 the CPU rewrites the header job 0 sampled a texture through to
/// describe another image (the raw device's own contract: a header written between submissions is visible at the next
/// one's header invalidate), so job 2 samples the new image, or with a header cache that kept job 0's entry the old one.
/// entry_none, the positive control at a job's entry, opens each job with no invalidate at all (not even the header
/// and sampler caches bare keeps) and no barrier before its first encoder: where a cache holds job 0's lines across the
/// job boundary, job 2 reads them there, so a cell judges the relaxed entries only once entry_none differed in it, CPU
/// and copy-channel writers too. injob_none runs the three steps in one command buffer with no barrier between them and
/// is reported (injob_none_mismatched_trials), as before. A GPU writer's three jobs are committed back to back, so job
/// 2's entry follows job 1's end on the GPU, not seconds later. rule names no entry form: production's (rule_entries=
/// the jobs it opened relaxed). Where entry_none never differs the hazard is unobservable at a job's entry and the line
/// says so (observable=0): the end of each job (RELEASE_WFI, the kernel's WAIT_FOR_IDLE and L2 flush) leaves the entry
/// nothing to find.
enum X1JobBoundaryProbe {
    static let probe = X1Probe(
        id: "gpu.x1.job_boundary_idle", name: "A job after the last one's fence needs no drain at its entry",
        probe: "barrier.entry_no_drain", timeout: 240, body: run)

    static let arms = [
        ("control", "entry=today pass=none switch=today"), ("planner", "entry=planner pass=none switch=today"),
        ("bare", "entry=bare pass=none switch=today"), ("entry_none", "entry=none pass=none switch=today"),
        ("rule", "pass=none switch=today"), ("injob_none", "pass=none switch=none"),
    ]
    static let batch = 8
    static let side = 16
    static let words = 256

    enum Writer: String {
        case rop, ce, sto, cpu, chan, hdr
    }

    enum Reader: String {
        case tex, sto, const, ce
    }

    static let cells: [(Writer, Reader)] = [
        (.rop, .tex), (.ce, .tex), (.sto, .sto), (.sto, .const), (.sto, .ce), (.cpu, .sto), (.cpu, .const),
        (.chan, .tex), (.hdr, .tex),
    ]

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        let journal = ProbeJournal(probe: "gpu.x1.job_boundary_idle")
        let kit = try BarrierKit(gpu, .bgra8Unorm)
        let pipelines = try [
            "seed": gpu.compute("x1_buffer_seed"), "copy": gpu.compute("x1_buffer_copy"),
            "const": gpu.compute("x1_constant_copy"),
        ]
        var judged = ProbeLedger()
        for (index, (writer, reader)) in cells.enumerated() {
            let name = "\(writer.rawValue)_\(reader.rawValue)"
            let work = try Work(kit, pipelines, writer, reader)
            let gpuWritten = writer != .cpu && writer != .chan && writer != .hdr
            let cellArms = gpuWritten ? arms : arms.filter { $0.0 != "injob_none" }
            journal.starting("\(name) arms=\(cellArms.map(\.0).joined(separator: "/"))")
            let started = Date()
            let plan = IterationPlan(cells: cellArms.count, target: 1504, batch: batch)
            var trial = [Int](repeating: 0, count: cellArms.count)
            for step in plan.order {
                if Date().timeIntervalSince(started) > 6 && step.cell == 0 { break }
                let (arm, forms) = cellArms[step.cell]
                if (arm == "injob_none" || arm == "entry_none") && step.round > 8 && !judged.tally(name, arm).clean {
                    continue
                }
                let first = trial[step.cell]
                let seeds = (0..<batch).map { probeSeed(trial: first + $0, cell: index, seeds: 16) }
                let ran =
                    arm == "injob_none"
                    ? try await work.inJob(forms: forms, seeds: seeds) : try await work.jobs(forms: forms, seeds: seeds)
                var tally = judged.tally(name, arm)
                for k in 0..<batch {
                    let want = work.expected(seeds[k])
                    want.withUnsafeBytes {
                        tally.compare(
                            work.result(k), $0, trial: first + k, width: work.resultWidth,
                            bytesPerTexel: 4)
                    }
                }
                trial[step.cell] += batch
                judged.store(name, arm, tally, ran)
            }
        }
        var finding = judged.finding(
            relaxed: ["planner", "bare", "rule"], none: "entry_none", control: "control",
            takenKey: "entries", rule: "barrier.entry_no_drain")
        let observable = judged.cells.contains { !(judged.tallies[$0]?["entry_none"]?.clean ?? true) }
        finding.metrics.set("observable", observable ? 1 : 0)
        if !observable {
            finding.note = "unobservable: entry_none never differed, so no entry form can be judged here"
        }
        let injob = judged.cells.compactMap { judged.tallies[$0]?["injob_none"] }
        finding.metrics.set("injob_none_mismatched_trials", injob.map(\.mismatchedTrials).reduce(0, +))
        if judged.failed("rule", none: "entry_none") > 0 {
            finding.verdict = .fail
            finding.note = "production's entry read other texels than today's"
        }
        let controlWrong = judged.cells.compactMap { judged.tallies[$0]?["control"]?.mismatchedTrials }.reduce(0, +)
        finding.metrics.set("control_mismatched_trials", controlWrong)
        if controlWrong > 0 {
            finding.verdict = .inconclusive
            finding.note = "today's entry gave texels other than the CPU's reading"
        }
        return finding
    }

    /// One cell's memory: a texture or a buffer per trial, what job 0 reads into, and what job 2 reads into.
    struct Work {
        let kit: BarrierKit
        let pipelines: [String: MTLComputePipelineState]
        let writer: Writer, reader: Reader
        let textures: [MTLTexture]
        let images: [MTLTexture]  // hdr: the images the textures' headers are rewritten to describe
        let buffers: [MTLBuffer]
        let staging: [MTLBuffer]
        let primeAtlas: MTLTexture, atlas: MTLTexture
        let primeOut: MTLBuffer, out: MTLBuffer

        init(
            _ kit: BarrierKit, _ pipelines: [String: MTLComputePipelineState], _ writer: Writer, _ reader: Reader
        )
            throws
        {
            self.kit = kit
            self.pipelines = pipelines
            self.writer = writer
            self.reader = reader
            let gpu = kit.gpu, n = X1JobBoundaryProbe.batch, side = X1JobBoundaryProbe.side
            textures =
                reader == .tex
                ? try (0..<n).map { _ in
                    try gpu.texture(
                        .bgra8Unorm, side, side, usage: [.renderTarget, .shaderRead],
                        storage: writer == .chan || writer == .hdr ? .shared : .private)
                } : []
            images =
                writer == .hdr
                ? try (0..<n).map { _ in
                    try gpu.texture(.bgra8Unorm, side, side, usage: [.shaderRead], storage: .shared)
                }
                : []
            buffers = reader == .tex ? [] : try (0..<n).map { _ in try gpu.buffer(X1JobBoundaryProbe.words * 4) }
            staging = writer == .ce ? try (0..<n).map { _ in try gpu.buffer(side * side * 4) } : []
            primeAtlas = try kit.atlas(n)
            atlas = try kit.atlas(n)
            primeOut = try gpu.buffer(n * X1JobBoundaryProbe.words * 4)
            out = try gpu.buffer(max(n * X1JobBoundaryProbe.words * 4, kit.tileBytes * n))
        }

        var resultWidth: Int { reader == .tex ? BarrierKit.tile : X1JobBoundaryProbe.words }

        func result(_ k: Int) -> UnsafeRawBufferPointer {
            reader == .tex
                ? kit.tile(out, k)
                : UnsafeRawBufferPointer(
                    start: out.contents() + k * X1JobBoundaryProbe.words * 4,
                    count: X1JobBoundaryProbe.words * 4)
        }

        func expected(_ seed: Int) -> [UInt8] {
            if reader == .tex {
                return kit.expectedTap(
                    seed: seed, base: SIMD2(0, 0), step: SIMD2(1, 1), width: X1JobBoundaryProbe.side,
                    height: X1JobBoundaryProbe.side)
            }
            return (0..<X1JobBoundaryProbe.words).flatMap { i -> [UInt8] in
                withUnsafeBytes(of: probeHash(UInt32(i), 0, UInt32(seed)).littleEndian, Array.init)
            }
        }

        func pattern(_ seed: Int) -> [UInt8] {
            let side = X1JobBoundaryProbe.side
            return (0..<side * side).flatMap { kit.texel($0 % side, $0 / side, seed: seed) }
        }

        /// Reads every trial's memory: into the prime outputs (job 0) or the results (job 2)
        func read(_ commands: MTLCommandBuffer, results: Bool, concurrent: MTLComputeCommandEncoder? = nil) {
            if reader == .tex {
                kit.tapAll(commands, textures, into: results ? atlas : primeAtlas)
                if results { kit.readback(commands, atlas, into: out) }
                return
            }
            if reader == .ce {
                let blit = commands.makeBlitCommandEncoder()!
                for (k, b) in buffers.enumerated() {
                    blit.copy(
                        from: b, sourceOffset: 0, to: results ? out : primeOut,
                        destinationOffset: k * X1JobBoundaryProbe.words * 4, size: X1JobBoundaryProbe.words * 4)
                }
                blit.endEncoding()
                return
            }
            let encoder = concurrent ?? commands.makeComputeCommandEncoder(dispatchType: .concurrent)!
            encoder.setComputePipelineState(pipelines[reader == .sto ? "copy" : "const"]!)
            for (k, b) in buffers.enumerated() {
                encoder.setBuffer(b, offset: 0, index: 0)
                encoder.setBuffer(results ? out : primeOut, offset: k * X1JobBoundaryProbe.words * 4, index: 1)
                encoder.dispatchThreads(
                    MTLSize(width: X1JobBoundaryProbe.words, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            }
            if concurrent == nil { encoder.endEncoding() }
        }

        /// The GPU writers' work for `seeds`; false where the writer is the CPU or the copy channel
        func write(_ commands: MTLCommandBuffer, _ seeds: [Int], concurrent: MTLComputeCommandEncoder? = nil) {
            switch writer {
            case .rop:
                for (k, t) in textures.enumerated() {
                    kit.seed(commands, t, seed: seeds[k], spin: [0, 6, 34, 80][k % 4])
                }
            case .ce:
                let blit = commands.makeBlitCommandEncoder()!
                for (k, t) in textures.enumerated() {
                    blit.copy(
                        from: staging[k], sourceOffset: 0, sourceBytesPerRow: X1JobBoundaryProbe.side * 4,
                        sourceBytesPerImage: 0,
                        sourceSize: MTLSize(width: X1JobBoundaryProbe.side, height: X1JobBoundaryProbe.side, depth: 1),
                        to: t, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
                }
                blit.endEncoding()
            case .sto:
                let encoder = concurrent ?? commands.makeComputeCommandEncoder(dispatchType: .concurrent)!
                encoder.setComputePipelineState(pipelines["seed"]!)
                for (k, b) in buffers.enumerated() {
                    var seed = UInt32(seeds[k])
                    encoder.setBuffer(b, offset: 0, index: 0)
                    encoder.setBytes(&seed, length: 4, index: 1)
                    encoder.dispatchThreads(
                        MTLSize(width: X1JobBoundaryProbe.words, height: 1, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
                }
                if concurrent == nil { encoder.endEncoding() }
            case .cpu, .chan, .hdr:
                break
            }
        }

        /// The CPU's writes (the CPU itself, the copy channel's replaceRegion, the copy engine's staging)
        func writeFromCPU(_ seeds: [Int]) {
            let side = X1JobBoundaryProbe.side
            for k in 0..<seeds.count {
                switch writer {
                case .cpu:
                    let words = buffers[k].contents().bindMemory(to: UInt32.self, capacity: X1JobBoundaryProbe.words)
                    for i in 0..<X1JobBoundaryProbe.words { words[i] = probeHash(UInt32(i), 0, UInt32(seeds[k])) }
                case .chan:
                    textures[k].replace(
                        region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0, withBytes: pattern(seeds[k]),
                        bytesPerRow: side * 4)
                case .ce:
                    let bytes = pattern(seeds[k])
                    staging[k].contents().copyMemory(from: bytes, byteCount: bytes.count)
                case .hdr:
                    images[k].replace(
                        region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0, withBytes: pattern(seeds[k]),
                        bytesPerRow: side * 4)
                    textures[k].replace(
                        region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0,
                        withBytes: pattern(seeds[k] ^ 0x5a5a),
                        bytesPerRow: side * 4)
                default:
                    break
                }
            }
        }

        /// Three jobs under the arm's entry: job 0 reads, job 1 (or the CPU) writes, job 2 reads; job 2's GPU time
        func jobs(forms: String, seeds: [Int]) async throws -> ArmBatch {
            let gpu = kit.gpu
            if writer == .rop || writer == .ce || writer == .sto {
                writeFromCPU(seeds)
                return try gpu.runBackToBack(
                    forms: forms, [{ read($0, results: false) }, { write($0, seeds) }, { read($0, results: true) }])
            }
            if writer == .hdr { writeFromCPU(seeds) }
            _ = try await gpu.run(forms: forms) { read($0, results: false) }
            if writer == .hdr {
                for (t, image) in zip(textures, images) { try RawProbe.retarget(gpu.device, t, to: image) }
                defer { for t in textures { try? RawProbe.retarget(gpu.device, t, to: nil) } }
                return try await gpu.run(forms: forms) { read($0, results: true) }
            }
            writeFromCPU(seeds)
            return try await gpu.run(forms: forms) { read($0, results: true) }
        }

        /// The three steps in one command buffer with no barrier between them (compute: one concurrent encoder)
        func inJob(forms: String, seeds: [Int]) async throws -> ArmBatch {
            writeFromCPU(seeds)
            return try await kit.gpu.run(forms: forms) { commands in
                if reader == .tex || reader == .ce {
                    read(commands, results: false)
                    write(commands, seeds)
                    read(commands, results: true)
                    return
                }
                let encoder = commands.makeComputeCommandEncoder(dispatchType: .concurrent)!
                read(commands, results: false, concurrent: encoder)
                write(commands, seeds, concurrent: encoder)
                read(commands, results: true, concurrent: encoder)
                encoder.endEncoding()
            }
        }
    }
}
