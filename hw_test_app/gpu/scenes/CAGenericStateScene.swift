import Foundation
import HWTestCore
import Metal

/// Core Animation's generic state (the uber shader) declares color_cube0, color_cube1 and lut_tex as texture3d
/// arguments at fragment indices 12, 13 and 14. Boot 15's first key press drew it with a 1-texel BGRA8 texture at index
/// 12; the raw facade sent the command buffer to m2v, whose pipelines for the keyboard took 18-20 s to make while the
/// render thread waited for it to be scheduled. Each case draws the state into a 256x256 BGRA8 target (its second
/// colour memoryless) after binding what that case leaves at indices 12-14: nothing, a 2D texture, one cube, one cube
/// at all three, three distinct cubes. On the raw facade path_<case>= must be "raw" for every case (wait_ms_<case>= is
/// the command buffer's commit to completion). Each case's target is held to the Mac's drawing of it, the mistyped 2D
/// texture to the Mac's case with nothing bound (a Mac's Metal draws no mistyped case). iOS 17's QuartzCore has no such
/// state, so the scene draws nothing there.
struct CAGenericStateScene: GPUScene {
    let id = "gpu.ca.generic_texture3d"
    let name = "Core Animation's generic state with its colour cubes bound, unbound and mistyped"

    static let line = PipelineLine(
        "R ok=1 label=com.apple.coreanimation.PBGRAXm_A2Xghfc samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 "
            + "c1=115:15:0:1:0:0:1:0:0 | v fixed_vert_lph_gen fc_base_color:53:00 fc_generic:53:01 "
            + "fc_vertex_layout:49:00 | f fixed_frag_lph_cpf fc_attachment_count:49:02 fc_framebuffer_fetch:53:01 "
            + "fc_generic:53:01 fc_has_asg_77_weights:53:01 fc_has_attachment_0:53:00 fc_has_attachment_1:53:00 "
            + "fc_has_attachment_2:53:00 fc_has_color_cube0:53:01 fc_has_color_cube1:53:01 fc_has_color_params:53:01 "
            + "fc_has_color_trc0:53:01 fc_has_color_trc1:53:01 fc_has_gamma_luts:53:01 fc_has_image_0A:53:01 "
            + "fc_has_image_0B:53:01 fc_has_image_0C:53:01 fc_has_image_1A:53:01 fc_has_image_1B:53:01 "
            + "fc_has_image_1C:53:01 fc_has_lod_bias:53:01 fc_has_lut_tex:53:01 fc_has_noise_scale:53:01 "
            + "fc_has_noise_tex:53:01 fc_has_ycbcr_matrix:53:01 fc_vertex_layout:49:00 | ")!
    static let size = 256
    static let warmups = 20
    static let mistyped = "leftover_2d"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let kind = RawFacade.kind(of: gpu.device)
        let pipeline: MTLRenderPipelineState
        do {
            let library = try gpu.device.makeLibrary(URL: CAState.library)
            pipeline = try CAState.pipeline(Self.line, library: library, device: gpu.device)
        } catch {
            result.notes.append("no generic state from this QuartzCore (\(GPU.describe(error)))")
            return result
        }
        let target = try gpu.texture(.bgra8Unorm, Self.size, Self.size)
        let scratch = try gpu.texture(.rgba16Float, Self.size, Self.size, usage: [.renderTarget], storage: .memoryless)
        let leftover = try gpu.texture(.bgra8Unorm, 1, 16, usage: [.shaderRead])
        let cube = try volume(gpu), other = try volume(gpu), lut = try volume(gpu)
        let cases: [(String, [MTLTexture?])] = [
            ("nothing", [nil, nil, nil]), ("leftover_2d", [leftover, nil, nil]), ("one_cube", [cube, nil, nil]),
            ("same_cube", [cube, cube, cube]), ("distinct_cubes", [cube, other, lut]),
        ].filter { kind != "apple" || $0.0 != Self.mistyped }
        let raw = kind == "raw"
        let zeros = [UInt8](repeating: 0, count: 256)
        for (label, cubes) in cases {
            let draw = { (commands: MTLCommandBuffer) in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                pass.colorAttachments[1].texture = scratch
                pass.colorAttachments[1].loadAction = .clear
                pass.colorAttachments[1].storeAction = .dontCare
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                CAState.bind(
                    encoder, vertices: CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: SIMD4(1, 1, 1, 1)),
                    source: nil)
                for slot in 4..<8 { encoder.setVertexBytes(zeros, length: zeros.count, index: slot) }
                for slot in 4..<16 { encoder.setFragmentBytes(zeros, length: zeros.count, index: slot) }
                encoder.setFragmentTextures(cubes, range: 12..<15)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            var timing = try await gpu.run(tolerateError: true, draw)
            var runs = 1
            while raw && timing.path.hasPrefix("m2v:state") && runs < Self.warmups {
                try await Task.sleep(nanoseconds: 50_000_000)
                timing = try await gpu.run(tolerateError: true, draw)
                runs += 1
            }
            result.metrics.set("path_" + label, timing.path)
            result.metrics.set("wait_ms_" + label, timing.wallMilliseconds, digits: 2)
            if !timing.path.hasPrefix("error:") {
                let pixels = try await gpu.read(target, bytesPerPixel: 4)
                result.picture(
                    label, bytes: pixels, metalFormat: target.pixelFormat.rawValue, width: Self.size,
                    height: Self.size, golden: label == Self.mistyped ? "nothing" : label, cleared: [0, 0, 0, 1])
            }
            guard raw else { continue }
            result.expect(!timing.path.hasPrefix("error:"), "\(label): \(timing.path)")
            result.expect(timing.path == "raw", "\(label) drawn on \(timing.path)")
        }
        return result
    }

    private func volume(_ gpu: GPU) throws -> MTLTexture {
        let descriptor = MTLTextureDescriptor()
        descriptor.textureType = .type3D
        descriptor.pixelFormat = .bgra8Unorm
        (descriptor.width, descriptor.height, descriptor.depth) = (4, 4, 4)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .private
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else {
            throw TestFailure("makeTexture(3D 4x4x4) returned nil")
        }
        return texture
    }
}
