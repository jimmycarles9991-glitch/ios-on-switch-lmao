import Foundation
import HWTestCore
import Metal

/// QuartzCore 27's Liquid Glass background (glass_background_all over the variable blur's vertex function, into BGRA8
/// beside an RGBA16Float and an R32Float attachment) and the RGBA16Float downsample its blur starts with, made from
/// QuartzCore's own library exactly as these corpus lines say. They are caspec27_test.py's GLASS and DOWNSAMPLE
/// (drivers/TegraGM20BMetal/raw/pack/ca), which caspec.state_line makes from qc-24B119.json. 27's raw pack holds every
/// glass variant, so on the facade each pass must take the raw path: an m2v path fails. Their uniforms are zero, as
/// the pack's probe binds them, except the glass's own parameter block (CAUniforms, so its draw shows), and the pictures
/// have no CPU model: each pass's is held to the Mac's (ds4_rgba16f, glass),
/// t_<pass>= is the GPU time in ms (the median of three). iOS 27's app runs it (Scenes.glass).
struct CAGlassScene: GPUScene {
    let id = "gpu.ca.glass"
    let name = "CA 27 Liquid Glass background and RGBA16Float downsample: path and pixels"

    static let size = 256
    static let passes: [(short: String, line: String)] = [
        (
            "ds4_rgba16f",
            "R ok=1 label=com.apple.coreanimation.PRGhAXm_Tds4A2Xhf_Isrc samples=1 topo=0 ds=0/0"
                + " c0=115:15:0:1:0:0:1:0:0 c1=115:0:0:1:0:0:1:0:0"
                + " a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1"
                + " | v downsample_blur_vert_lph  | f downsample_4_frag_lph  | "
        ),
        (
            "glass",
            "R ok=1 label=com.apple.coreanimation.PBGRABsovXmw_TgbhA3Xhf_Isrc_Isrc samples=1 topo=0 ds=0/0"
                + " c0=80:15:1:1:5:0:1:5:0 c1=115:0:0:1:0:0:1:0:0 c2=55:0:0:1:0:0:1:0:0"
                + " a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1"
                + " | v variable_blur_vert_lph  | f glass_background_all_lph  | "
        ),
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            result.expect(false, "no QuartzCore library (\(GPU.describe(error)))")
            return result
        }
        let size = Self.size
        let source = try gpu.texture(.bgra8Unorm, size, size, usage: [.shaderRead])
        try await gpu.upload(
            Patterns.bytes(Patterns.busy(width: size, height: size, channels: 4), .bgra8Unorm), to: source,
            bytesPerRow: size * 4)
        let sdf = try gpu.texture(.rgba16Float, size, size, usage: [.shaderRead])
        let field = Raster(width: size, height: size, channels: 4) { x, y, _ in
            (hypotf(Float(x - size / 2), Float(y - size / 2)) - Float(size) / 3) / Float(size)
        }
        try await gpu.upload(Patterns.bytes(field, .rgba16Float), to: sdf, bytesPerRow: size * 8)
        let onFacade = RawFacade.kind(of: gpu.device) == "raw"
        for (short, text) in Self.passes {
            guard let line = PipelineLine(text) else {
                result.expect(false, "\(short): the corpus line does not parse")
                continue
            }
            let pipeline: MTLRenderPipelineState
            do {
                pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
            } catch {
                result.expect(false, "\(short): no state (\(GPU.describe(error)))")
                continue
            }
            let attachments = try line.colors.map { color -> MTLTexture in
                let format = MTLPixelFormat(rawValue: color.format) ?? .invalid
                return color.index == 0
                    ? try gpu.texture(format, size, size, usage: [.renderTarget, .shaderRead])
                    : try gpu.texture(format, size, size, usage: [.renderTarget], storage: .memoryless)
            }
            var times: [Double] = []
            var paths: Set<String> = []
            for _ in 0..<3 {
                let timing = try await gpu.run(tolerateError: true) { commands in
                    let pass = MTLRenderPassDescriptor()
                    for (index, texture) in attachments.enumerated() {
                        pass.colorAttachments[index].texture = texture
                        pass.colorAttachments[index].loadAction = .clear
                        pass.colorAttachments[index].clearColor = MTLClearColor(
                            red: 0.1, green: 0.2, blue: 0.3, alpha: 1)
                        pass.colorAttachments[index].storeAction = index == 0 ? .store : .dontCare
                    }
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(pipeline)
                    CAState.bind(
                        encoder, vertices: CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: SIMD4(1, 1, 1, 1)),
                        source: source)
                    if short == "glass" { CAUniforms.bindGlass(encoder, source: source, sdf: sdf) }
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    encoder.endEncoding()
                }
                times.append(timing.gpuMilliseconds)
                paths.insert(timing.path)
            }
            let shown = paths.sorted().joined(separator: ",")
            result.expect(!paths.contains { $0.hasPrefix("error:") }, "\(short): \(shown)")
            if onFacade { result.expect(paths == ["raw"], "\(short) drew on \(shown), not the raw path") }
            result.metrics.set("t_\(short)", Stats.percentile(times, 0.5), digits: 3)
            let format = line.colors[0].format
            let bytesPerPixel = format == MTLPixelFormat.rgba16Float.rawValue ? 8 : 4
            let bytes = try await gpu.read(attachments[0], bytesPerPixel: bytesPerPixel)
            result.picture(
                short, bytes: bytes, metalFormat: format, width: size, height: size, cleared: [0.1, 0.2, 0.3, 1])
        }
        return result
    }
}
