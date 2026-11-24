import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Probe rows of a mixed pass, a one-sample BGRA8 colour beside a four-sample memoryless Depth32Float drawn by a state
/// of 4 raster samples, each row the facade's switchMetalProbeTir: word (drivers/TegraGM20BMetal/mixed_samples.h:
/// kTirProbeZcull .. TirEnd), each in a command buffer of its own: the mixed pass (a near white quad over a far black
/// one under Less with writes) and after it a one-sample pass of another colour with one draw. A row is accepted where
/// its command buffer completed (row_<name>=accepted, row_<name>_wrong the mixed colour's texels not white) and killed
/// where the class refused one of its methods (row_<name>=killed); killed= counts them. Each killed row's class error,
/// in row order, is the kernel's DATA_ERROR line in the test's window (tools/devicerun hwtest.tir-rows-errors).
/// They decide, not judge: FAIL only where a row could not run.
enum TirRows {
    static let side = 64

    struct Row {
        let name: String
        let word: UInt32
    }

    struct Ran {
        var accepted = 0, killed = 0
    }

    /// Each row of `rows` under the switch, into `metrics`; nil where the facade has no such switch
    static func run(_ gpu: GPU, _ rows: [Row], _ metrics: inout Metrics) async throws -> Ran? {
        var ran = Ran()
        for row in rows {
            let outcome = try await ZcProbes.switched(gpu, "switchMetalProbeTir:", NSNumber(value: row.word)) {
                try await Self.scene(gpu)
            }
            guard let outcome else { return nil }
            if let wrong = outcome {
                metrics.set("row_" + row.name, "accepted")
                metrics.set("row_\(row.name)_wrong", wrong)
                ran.accepted += 1
            } else {
                metrics.set("row_" + row.name, "killed")
                ran.killed += 1
            }
        }
        metrics.set("accepted", ran.accepted)
        metrics.set("killed", ran.killed)
        return ran
    }

    /// The mixed colour's texels not white where the command buffer completed, nil where it failed
    static func scene(_ gpu: GPU) async throws -> Int? {
        let colour = try gpu.texture(.bgra8Unorm, side, side)
        let after = try gpu.texture(.bgra8Unorm, side, side)
        let depth = try gpu.texture(.depth32Float, side, side, usage: [.renderTarget], samples: 4, storage: .memoryless)
        let mixed = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, samples: 4, depth: .depth32Float)
        let plain = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm)
        let less = try ZcProbes.depthState(gpu, .less, write: true)
        let path = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .dontCare
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(mixed)
            e.setDepthStencilState(less)
            for (z, white) in [(Float(0.25), true), (Float(0.75), false)] {
                GPU.draw(
                    e, corners: GPU.pixelQuad(0, 0, side, side, width: side, height: side, z: z),
                    color: white ? SIMD4(1, 1, 1, 1) : SIMD4(0, 0, 0, 1)
                )
            }
            e.endEncoding()
            let next = MTLRenderPassDescriptor()
            next.colorAttachments[0].texture = after
            next.colorAttachments[0].loadAction = .clear
            next.colorAttachments[0].storeAction = .store
            let f = commands.makeRenderCommandEncoder(descriptor: next)!
            f.setRenderPipelineState(plain)
            GPU.draw(f, corners: GPU.pixelQuad(0, 0, side, side, width: side, height: side), color: SIMD4(1, 1, 1, 1))
            f.endEncoding()
        }.path
        guard !path.hasPrefix("error:") else { return nil }
        return ZcProbes.texelsOff(try await gpu.read(colour, bytesPerPixel: 4), [255, 255, 255, 255])
    }
}

/// The pass-end orders of a mixed pass (mixed_samples.h TirEnd): boot 2's mixed_samples died at the end's
/// SET_ANTI_ALIAS_RASTER 1X1 with INVALID_VALUE (a listed value, so the class compares it with state still in force).
/// Rows: the mode then the raster count (end_mode, today's), the raster count first (end_raster_first), the zeta
/// deselected first (end_zeta_off), the mode alone (end_mode_only), the rasteriser off around them
/// (end_raster_disabled), and the class header's other raster values written with the mode off (end_value_<v>, then the
/// zeta deselected and 1X1). The accepted row is the order the pass end takes.
struct MixedEndRowsTest: HWTest {
    let id = "gpu.gm20b.mixed_end_rows"
    let name = "Mixed-sample pass end probe: which order of leaving target-independent rasterisation GM20B takes"
    let subsystem = Subsystem.metal

    static func row(_ end: UInt32, raster: UInt32 = 0) -> UInt32 { 1 | end << 8 | raster << 12 }

    static let rows: [TirRows.Row] = [
        TirRows.Row(name: "end_mode", word: row(0)),
        TirRows.Row(name: "end_raster_first", word: row(1)),
        TirRows.Row(name: "end_zeta_off", word: row(2)),
        TirRows.Row(name: "end_mode_only", word: row(3)),
        TirRows.Row(name: "end_raster_disabled", word: row(4)),
    ] + [2, 4, 5, 6].map { TirRows.Row(name: "end_value_\($0)", word: row(5, raster: UInt32($0))) }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        guard try await TirRows.run(gpu, Self.rows, &metrics) != nil else {
            return .skip(GM20BFacade.missing("switchMetalProbeTir:", on: gpu.device))
        }
        return Outcome.pass(metrics, proof: gpu.proof)
    }
}
