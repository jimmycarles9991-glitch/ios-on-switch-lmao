import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Which axis of QuartzCore 27's glass background a device gets wrong (ca.sweep's Tgbh/Tgbg states, ui.glass's
/// column_spread): each glass state (Tgbh glass_background_all, Tgbg glass_background_cr, both over the variable blur's
/// vertex function, as caspec.state_line makes them) drawn over a source that varies along x alone and one that varies
/// along y alone, everything else as gpu.ca.glass draws it. Each picture is held to the Mac's: `golden_<state>_<axis>`
/// is that axis's verdict. `<state>_<axis>_along`, `_across`: the picture's mean spread (max - min of a line's
/// luminance, in 255ths) along the source's axis and across it, which say what the device kept of that axis.
struct CAGlassAxesScene: GPUScene {
    let id = "gpu.ca.glass_axes"
    let framework = true
    let name = "CA 27 glass background over one-axis sources: which axis is the Mac's"

    static let size = 256
    static let states: [(short: String, line: String)] = [
        ("tgbh", CAGlassScene.passes[1].line),
        (
            "tgbg",
            "R ok=1 label=com.apple.coreanimation.PBGRABsovXmw_TgbgA3Xhf_Isrc_Isrc samples=1 topo=0 ds=0/0"
                + " c0=80:15:1:1:5:0:1:5:0 c1=115:0:0:1:0:0:1:0:0 c2=55:0:0:1:0:0:1:0:0"
                + " a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1"
                + " | v variable_blur_vert_lph  | f glass_background_cr_lph  | "
        ),
    ]

    /// A source of four channels that rise along `x` (or along y) alone, each channel at its own slope
    static func ramp(alongX: Bool) -> Raster {
        Raster(width: size, height: size, channels: 4) { x, y, c in
            let t = Float(alongX ? x : y) / Float(size - 1)
            return c == 3 ? 1 : min(1, t * Float(c + 1) / 3 + Float(c) / 8)
        }
    }

    /// The mean over lines of a BGRA8 picture's luminance spread, lines along x (rows) or along y (columns)
    static func spread(_ bytes: [UInt8], alongX: Bool) -> Int {
        let luma = { (x: Int, y: Int) -> Int in
            let at = 4 * (y * size + x)
            return (Int(bytes[at + 2]) * 2 + Int(bytes[at + 1]) * 5 + Int(bytes[at])) / 8
        }
        var total = 0
        for line in 0 ..< size {
            let values = (0 ..< size).map { alongX ? luma($0, line) : luma(line, $0) }
            total += (values.max() ?? 0) - (values.min() ?? 0)
        }
        return total / size
    }

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
        let sdf = try gpu.texture(.rgba16Float, size, size, usage: [.shaderRead])
        let field = Raster(width: size, height: size, channels: 4) { x, y, _ in
            (hypotf(Float(x - size / 2), Float(y - size / 2)) - Float(size) / 3) / Float(size)
        }
        try await gpu.upload(Patterns.bytes(field, .rgba16Float), to: sdf, bytesPerRow: size * 8)
        let onFacade = GM20BFacade.kind(of: gpu.device) == "gm20b"
        for (short, text) in Self.states {
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
            for alongX in [true, false] {
                let axis = alongX ? "x" : "y"
                let source = try gpu.texture(.bgra8Unorm, size, size, usage: [.shaderRead])
                try await gpu.upload(
                    Patterns.bytes(Self.ramp(alongX: alongX), .bgra8Unorm), to: source, bytesPerRow: size * 4
                )
                let attachments = try line.colors.map { color -> MTLTexture in
                    let format = MTLPixelFormat(rawValue: color.format) ?? .invalid
                    return color.index == 0
                        ? try gpu.texture(format, size, size, usage: [.renderTarget, .shaderRead])
                        : try gpu.texture(format, size, size, usage: [.renderTarget], storage: .memoryless)
                }
                let timing = try await gpu.run(tolerateError: true) { commands in
                    let pass = MTLRenderPassDescriptor()
                    for (index, texture) in attachments.enumerated() {
                        pass.colorAttachments[index].texture = texture
                        pass.colorAttachments[index].loadAction = .clear
                        pass.colorAttachments[index].clearColor = MTLClearColor(
                            red: 0.1,
                            green: 0.2,
                            blue: 0.3,
                            alpha: 1
                        )
                        pass.colorAttachments[index].storeAction = index == 0 ? .store : .dontCare
                    }
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(pipeline)
                    CAState.bind(
                        encoder, vertices: CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: SIMD4(1, 1, 1, 1)),
                        source: source
                    )
                    CAUniforms.bindGlass(encoder, source: source, sdf: sdf)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    encoder.endEncoding()
                }
                let label = "\(short)_\(axis)"
                result.expect(!timing.path.hasPrefix("error:"), "\(label): \(timing.path)")
                if onFacade { result.expect(timing.path == "gm20b", "\(label) drew on \(timing.path), not the driver") }
                let bytes = try await gpu.read(attachments[0], bytesPerPixel: 4)
                result.metrics.set("\(label)_along", Self.spread(bytes, alongX: alongX))
                result.metrics.set("\(label)_across", Self.spread(bytes, alongX: !alongX))
                result.picture(
                    label, bytes: bytes, metalFormat: line.colors[0].format, width: size, height: size,
                    cleared: [0.1, 0.2, 0.3, 1]
                )
            }
        }
        return result
    }
}
