import CoreVideo
import Foundation
import HWTestCore
import IOSurface
import Metal

/// Core Animation's backdrop blur chain, state by state as the corpus has it: the downsamples and narrow blurs into a
/// 'b3a8' (format 550) surface and the luma boost into it and into BGR10_XR (554), each beside a memoryless 550
/// colour 1, sampling a BGRA8 picture. The downsamples' and blurs' uniforms are zero (as the raw pack's probe binds them):
/// what counts there is how long each pass takes on the GPU (t_<state>= in ms, the median of three), which path took it,
/// and whether it completed. The luma boost fetches its target's colour through a LUT (CAUniforms): the BGR10_XR one
/// draws over a picture uploaded first, and its result (lub_xr) is held to the Mac's.
struct CABlurChainScene: GPUScene {
    let id = "gpu.ca.blur_chain"
    let name = "CA backdrop blur chain on b3a8 and BGR10_XR: timing and path"

    static let size = 256
    static let b3a8 = MTLPixelFormat(rawValue: 550)!
    static let chain = [
        "Pb3a8Xm_Tds4A2Xhf_Isrc", "Pb3a8Xm_Tds8A2Xhf_Isrc", "Pb3a8Xm_Tn11A2Xhf_Isrc", "Pb3a8Xm_Tn15A2Xhf_Isrc",
        "Pb3a8Xm_Tn19A2Xhf_Isrc", "Pb3a8Xm_Tn23A2Xhf_Isrc", "Pb3a8Xm_Tn27A2Xhf_Isrc", "Pb3a8Xm_TlubA2Xhf_Isrc",
        "Pw30rXm_TlubA2Xhf_Isrc",
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            result.notes.append("no QuartzCore library here (\(GPU.describe(error)))")
            return result
        }
        let size = Self.size
        let source = try gpu.texture(.bgra8Unorm, size, size, usage: [.shaderRead])
        try await gpu.blitUpload(
            [UploadRect(x: 0, y: 0, width: size, height: size)],
            [Patterns.bytes(Patterns.busy(width: size, height: size, channels: 4), .bgra8Unorm)], into: source, texel: 4
        )
        let xr = try gpu.texture(.bgr10_xr, size, size)
        let lutPicture = CAUniforms.lumaLut()
        let lut = try gpu.texture(.rgba16Float, lutPicture.width, 1, usage: [.shaderRead])
        try await gpu.upload(Patterns.bytes(lutPicture, .rgba16Float), to: lut, bytesPerRow: lutPicture.width * 8)
        let before = Patterns.bytes(Patterns.busy(width: size, height: size, channels: 3, seed: 5), .bgr10XR)
        let deep = try? Self.b3a8Texture(gpu)
        let colour1 = try? gpu.texture(Self.b3a8, size, size, usage: [.renderTarget], storage: .memoryless)
        if deep == nil { result.notes.append("no b3a8 surface texture") }
        if colour1 == nil { result.notes.append("no memoryless 550 texture") }
        for label in Self.chain {
            let short = Self.short(label)
            guard let line = CACorpus.state(label) else { continue }
            guard let target = line.colors.first?.format == 554 ? xr : deep else { continue }
            let pipeline: MTLRenderPipelineState
            do {
                pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
            } catch {
                result.notes.append("\(short): no state (\(GPU.describe(error)))")
                continue
            }
            var times: [Double] = []
            var paths: Set<String> = []
            let boost = label.contains("_Tlub") && target === xr
            for _ in 0..<3 {
                if boost { try await gpu.upload(before, to: xr, bytesPerRow: size * 4) }
                let timing = try await gpu.run(tolerateError: true) { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = boost ? .load : .clear
                    pass.colorAttachments[0].storeAction = .store
                    pass.colorAttachments[1].texture = colour1
                    pass.colorAttachments[1].loadAction = .clear
                    pass.colorAttachments[1].storeAction = .dontCare
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(pipeline)
                    CAState.bind(
                        encoder, vertices: CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: SIMD4(1, 1, 1, 1)),
                        source: source)
                    if label.contains("_Tlub") { CAUniforms.bindLumaBoost(encoder, lut: lut) }
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    encoder.endEncoding()
                }
                times.append(timing.gpuMilliseconds)
                paths.insert(timing.path)
            }
            result.expect(
                !paths.contains { $0.hasPrefix("error:") }, "\(short): \(paths.sorted().joined(separator: ","))")
            result.metrics.set("t_\(short)", Stats.percentile(times, 0.5), digits: 3)
            if target === xr {
                let bytes = try await gpu.read(xr, bytesPerPixel: 4)
                result.picture(
                    short, bytes: bytes, metalFormat: xr.pixelFormat.rawValue, width: size, height: size,
                    before: boost ? before : nil, cleared: boost ? nil : [0, 0, 0, 1])
            }
        }
        return result
    }

    /// "Pb3a8Xm_Tn11A2Xhf_Isrc" -> "n11", "Pw30rXm_TlubA2Xhf_Isrc" -> "lub_xr".
    static func short(_ label: String) -> String {
        let texture = label.split(separator: "_").dropFirst().first.map { String($0.dropFirst().prefix { $0 != "A" }) }
        return (texture ?? label) + (label.hasPrefix("Pw30r") ? "_xr" : "")
    }

    static func b3a8Texture(_ gpu: GPU) throws -> MTLTexture {
        var buffer: CVPixelBuffer?
        let attributes: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        guard
            CVPixelBufferCreate(
                nil, size, size, kCVPixelFormatType_30RGBLE_8A_BiPlanar, attributes as CFDictionary, &buffer)
                == kCVReturnSuccess, let buffer, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { throw TestFailure("no b3a8 pixel buffer") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: b3a8, width: size, height: size, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no 550 texture over a b3a8 surface")
        }
        return texture
    }
}
