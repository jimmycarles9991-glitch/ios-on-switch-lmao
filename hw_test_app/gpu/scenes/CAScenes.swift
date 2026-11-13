import CoreVideo
import Foundation
import HWTestCore
import IOSurface
import Metal

/// The process's first GPU work (the suite runs it before any other Metal test): Core Animation's untextured fill
/// into a 720x1280 BGRA8 IOSurface with Core Animation's framebuffer row of 3072 bytes, which the kernel makes
/// block-linear, cleared first; then read back three ways (blit, shader, CPU lock). Three rounds, each on a new
/// surface, so a round after a channel the kernel killed and the facade reopened runs too: reopens= counts those,
/// cb_errors= the command buffers that failed. The fill's own pixels have no CPU model: the first round's are held to
/// the Mac's (fill), and the other rounds must draw the same bytes.
struct FirstTiledSurfaceScene: GPUScene {
    let id = "gpu.ca.first_tiled_surface"
    let name = "First command buffer: CA fill into a block-linear 720x1280 surface"

    static let (width, height, rowBytes) = (720, 1280, 3072)
    static let fill = UploadRect(x: 100, y: 200, width: 520, height: 700)
    static let clear: [Float] = [0.2, 0.4, 0.6, 1]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let start = RawFacade.counters(of: gpu.device)
        if !start.isEmpty { result.metrics.set("first", ["raw", "m2v", "split"].allSatisfy { (start[$0] ?? 0) == 0 }) }
        var pipeline: MTLRenderPipelineState?
        do {
            let library = try gpu.device.makeLibrary(URL: CAState.library)
            pipeline = try CAState.pipeline(CACorpus.state("PBGRAXm_A2Xhf")!, library: library, device: gpu.device)
        } catch {
            result.notes.append("clear only: no Core Animation fill state here (\(GPU.describe(error)))")
        }
        var hashes: Set<UInt64> = []
        for round in 0..<3 {
            let target = try SurfaceTarget.make(
                gpu, .bgra8Unorm, Self.width, Self.height, rowBytes: Self.rowBytes, usage: [.renderTarget, .shaderRead])
            let colour1 =
                pipeline == nil
                ? nil
                : try gpu.texture(
                    .bgra8Unorm, Self.width, Self.height, usage: [.renderTarget], storage: .memoryless)
            let timing = try await gpu.run(tolerateError: true) { commands in
                let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(target.texture, colour1))!
                if let pipeline {
                    encoder.setRenderPipelineState(pipeline)
                    let (x0, y0) = (
                        Float(Self.fill.x) / Float(Self.width) * 2 - 1, 1 - Float(Self.fill.y) / Float(Self.height) * 2
                    )
                    let x1 = Float(Self.fill.x + Self.fill.width) / Float(Self.width) * 2 - 1
                    let y1 = 1 - Float(Self.fill.y + Self.fill.height) / Float(Self.height) * 2
                    CAState.bind(
                        encoder,
                        vertices: CAState.rectangle(x0: x0, y0: y1, x1: x1, y1: y0, color: SIMD4(1, 0.5, 0, 1)),
                        source: nil)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            result.metrics.set("path\(round)", timing.path)
            result.metrics.set("gpu_ms\(round)", timing.gpuMilliseconds, digits: 3)
            result.expect(!timing.path.hasPrefix("error:"), "round \(round): \(timing.path)")
            let blit = try await gpu.read(target.texture, bytesPerPixel: 4)
            let got = TexelFormat.bgra8Unorm.raster(blit, width: Self.width, height: Self.height)
            let drawn = pipeline == nil ? nil : Self.fill
            result.compare("round \(round) blit: clear outside the fill", got, Self.model(drawn), tolerance: 1.01 / 255)
            result.compare(
                "round \(round) CPU lock = blit", bytes: try target.cpuRead(), expected: blit, format: .bgra8Unorm,
                width: Self.width, height: Self.height, tolerance: 1.01 / 255)
            result.compare(
                "round \(round) shader = blit", try await gpu.computeRaster(target.texture, .bgra8Unorm), got,
                tolerance: 1.01 / 255)
            if let drawn {
                let region = TexelImage(.bgra8Unorm, Self.width, Self.height, got).read(drawn)
                if hashes.isEmpty {
                    result.picture(
                        "fill", bytes: region, metalFormat: MTLPixelFormat.bgra8Unorm.rawValue, width: drawn.width,
                        height: drawn.height, cleared: Self.clear)
                }
                hashes.insert(Self.fnv(region))
                result.metrics.set("fill_px\(round)", got.shown(got.pixel(drawn.x + 1, drawn.y + 1)))
            }
        }
        let end = RawFacade.counters(of: gpu.device)
        if !end.isEmpty { result.metrics.set("reopens", (end["reopens"] ?? 0) - (start["reopens"] ?? 0)) }
        result.metrics.set("cb_errors", gpu.commandBufferErrors.count)
        result.expect(hashes.count <= 1, "the fill drew \(hashes.count) different pictures in three rounds")
        return result
    }

    static func pass(_ target: MTLTexture, _ colour1: MTLTexture?) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(
            red: Double(clear[0]), green: Double(clear[1]), blue: Double(clear[2]), alpha: Double(clear[3]))
        pass.colorAttachments[0].storeAction = .store
        if let colour1 {
            pass.colorAttachments[1].texture = colour1
            pass.colorAttachments[1].loadAction = .clear
            pass.colorAttachments[1].storeAction = .dontCare
        }
        return pass
    }

    /// The clear colour, any value inside the fill.
    static func model(_ fill: UploadRect?) -> Raster {
        let texel = TexelFormat.bgra8Unorm.decode(TexelFormat.bgra8Unorm.encode(clear)[...])
        return Raster(width: width, height: height, channels: 4) { x, y, c in
            if let fill, x >= fill.x, x < fill.x + fill.width, y >= fill.y, y < fill.y + fill.height { return .nan }
            return texel[c]
        }
    }

    static func fnv(_ bytes: [UInt8]) -> UInt64 {
        bytes.reduce(0xcbf2_9ce4_8422_2325) { ($0 ^ UInt64($1)) &* 0x100_0000_01b3 }
    }
}
