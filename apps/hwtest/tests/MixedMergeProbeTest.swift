import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Whether one draw can give Apple's rule for a four-sample state in a mixed pass (colour where sample 0 passes, depth at
/// every covered passing sample; drivers/TegraGM20BMetal/mixed_samples.h), which the driver draws in two today. NVIDIA's
/// X1 driver offers MERGE (SET_TIR_CONTROL REDUCE_COVERAGE) alone, and the GM20B faults with it off under
/// RASTER_N_TARGET_M, so each row is MERGE plus sample masks: the raster masks SET_SAMPLE_MASK_X0_Y0..X1_Y1 (0x0fbc..),
/// the colour target's mask SET_COLOR_TARGET_SAMPLE_MASK (0x0fa8), and which of them apply, SET_SAMPLE_MASK (0x0fa4:
/// bit 0 raster out, bit 4 colour target). The draw is MixedSamplesCases' at raster 4 with a program that can discard
/// and never does (blend_kill_discard_fragment), so the driver splits it as SplitShaded, the one form that runs the
/// program twice (a state whose program shapes no coverage is split with the pixel stage off and runs it once). It is
/// drawn whole (the facade's TIR probe bit 5) over depths whose sample s is 1 where bit s of the mask is set, for masks
/// 0..15. The driver sets the init context's sample masks back after each such draw, so the probe leaves none behind.
/// `<row>_white`: masks 0..15, 1 where the colour turned white (Apple: 0101010101010101); `<row>_depth_wrong_samples`:
/// bit s where sample s's depth after is not Metal's; `<row>_apple`: both as Apple's; `<row>=killed`: the class refused
/// it. `one_draw_route`: the first row with Apple's result, `none` where no row has it. It decides, not judges: FAIL
/// only where a row could not run.
struct MixedMergeProbeTest: HWTest {
    let id = "gpu.gm20b.mixed_merge_probe"
    let name = "Does MERGE with a sample mask draw a mixed pass's four-sample state as Apple does, in one draw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 180 }

    static let armedWhole: UInt32 = 1 | 1 << 5
    static let apple = "0101010101010101"
    static let rasterMasks: UInt32 = 0x0fbc, sampleMask: UInt32 = 0x0fa4, colourTargetMask: UInt32 = 0x0fa8

    static func raster(_ mask: UInt32) -> [(UInt32, UInt32)] {
        (0 ..< 4).map { (rasterMasks + 4 * UInt32($0), mask) }
    }

    static let rows: [(String, [(UInt32, UInt32)])] = [
        ("merge", []),
        ("raster_s0", raster(0x1111)),
        ("colour_target_s0", [(sampleMask, 0x11), (colourTargetMask, 0x0001)]),
        ("colour_target_s0_all", [(sampleMask, 0x11), (colourTargetMask, 0x1111)]),
        ("colour_target_only_s0", [(sampleMask, 0x10), (colourTargetMask, 0x0001)]),
        ("colour_target_s0_raster_all", [(sampleMask, 0x11), (colourTargetMask, 0x0001)] + raster(0xffff)),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let scene = try MixedSamplesCases.Scene(gpu)
        let shaded = try gpu.render(
            fragment: "blend_kill_discard_fragment", format: .rgba8Unorm, samples: 4, depth: .depth32Float
        )
        var metrics = Metrics()
        var route = "none"
        var unran: [String] = []
        for (name, words) in Self.rows {
            var white = "", wrongSamples = 0
            do {
                for mask in UInt32(0) ..< 16 {
                    let depth = try await scene.filled(mask)
                    let before = try await scene.samples(depth)
                    let colour = try gpu.texture(.rgba8Unorm, MixedSamplesCases.side, MixedSamplesCases.side)
                    let whole = NSNumber(value: Self.armedWhole)
                    let switched = try await ZcProbes.switched(gpu, "switchMetalProbeTir:", whole) {
                        if words.isEmpty {
                            _ = try await gpu.run { Self.encode($0, scene, shaded, colour, depth) }
                        } else {
                            _ = try await GM20BMethods.draw(gpu, before: words, after: []) {
                                Self.encode($0, scene, shaded, colour, depth)
                            }
                        }
                    }
                    guard switched != nil else {
                        return .skip(GM20BFacade.missing("switchMetalProbeTir:", on: gpu.device))
                    }
                    let bytes = try await gpu.read(colour, bytesPerPixel: 4)
                    let centre = (MixedSamplesCases.side / 2 * MixedSamplesCases.side + MixedSamplesCases.side / 2) * 4
                    white += bytes[centre] == 255 ? "1" : "0"
                    let after = try await scene.samples(depth)
                    for s in 0 ..< 4 {
                        let at = centre + s
                        let want: Float = before[at] > 0.5 ? 0.5 : before[at]
                        if after[at] != want { wrongSamples |= 1 << s }
                    }
                }
            } catch {
                metrics.set(name, "killed")
                metrics.set("\(name)_error", String(describing: error).prefix(80).description)
                unran.append(name)
                continue
            }
            let apple = white == Self.apple && wrongSamples == 0
            metrics.set("\(name)_white", white)
            metrics.set("\(name)_depth_wrong_samples", String(wrongSamples, radix: 16))
            metrics.set("\(name)_apple", apple)
            if apple && route == "none" { route = name }
        }
        metrics.set("one_draw_route", route)
        return unran.count == Self.rows.count ? .fail(unran.joined(separator: ","), metrics) : .pass(metrics)
    }

    static func encode(_ commands: MTLCommandBuffer, _ scene: MixedSamplesCases.Scene, _ state: MTLRenderPipelineState,
                       _ colour: MTLTexture, _ depth: MTLTexture)
    {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colour
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .load
        pass.depthAttachment.storeAction = .store
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        e.setRenderPipelineState(state)
        e.setDepthStencilState(scene.less)
        var corners = GPU.fullScreen
        var white = SIMD4<Float>(1, 1, 1, 1)
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        e.setFragmentBytes(&white, length: 16, index: 0)
        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        e.endEncoding()
    }
}
