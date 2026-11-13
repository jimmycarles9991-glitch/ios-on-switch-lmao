import Foundation
import HWTestCore
import Metal

/// What a pass draws is what the next work in its command buffer reads: Core Animation's untextured fill draws 64
/// overlapping rectangles, each its own colour, over a 720x1280 BGRA8 IOSurface (the kernel makes it block-linear, as
/// the display's framebuffers are), and the same command buffer copies the surface into a texture with the blit
/// engine; then the surface's own blit readback and its CPU lock must both equal the copy. Twenty rounds, each with
/// other colours, so a copy that read what an earlier round left (draws still binned in the tiled cache, writes not
/// yet in memory) differs from the surface. stale= counts the rounds whose copy differed; round 0's surface is held to
/// the Mac's (round0).
struct CADrawThenCopyScene: GPUScene {
    let id = "gpu.ca.draw_then_copy"
    let name = "CA fill draws then a blit copy in one command buffer: the copy reads every draw"

    static let (width, height, rounds, draws) = (720, 1280, 20, 64)
    /// Cleared to a colour the fill draws none of (QuartzCore 27's fill comes out opaque black with zero uniforms), so a
    /// draw that never ran leaves a picture unlike the Mac's
    static let sentinel = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
    static let sentinelTexel: [Float] = [0.2, 0.4, 0.6, 1]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let pipeline: MTLRenderPipelineState
        do {
            let library = try gpu.device.makeLibrary(URL: CAState.library)
            pipeline = try CAState.pipeline(CACorpus.state("PBGRAXm_A2Xhf")!, library: library, device: gpu.device)
        } catch {
            result.notes.append("no Core Animation fill state here (\(GPU.describe(error)))")
            return result
        }
        let target = try SurfaceTarget.make(
            gpu, .bgra8Unorm, Self.width, Self.height, rowBytes: Self.width * 4, usage: [.renderTarget, .shaderRead])
        let copy = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.shaderRead])
        // Core Animation's fill state declares a second colour attachment
        let colour1 = try gpu.texture(
            .bgra8Unorm, Self.width, Self.height, usage: [.renderTarget], storage: .memoryless)
        var stale = 0
        for round in 0..<Self.rounds {
            let timing = try await gpu.run(tolerateError: true) { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target.texture
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = Self.sentinel
                pass.colorAttachments[0].storeAction = .store
                pass.colorAttachments[1].texture = colour1
                pass.colorAttachments[1].loadAction = .clear
                pass.colorAttachments[1].storeAction = .dontCare
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                for draw in 0..<Self.draws {
                    let (x0, y0) = (Float(draw % 8) * 0.25 - 1, Float(draw / 8) * 0.25 - 1)
                    CAState.bind(
                        encoder,
                        vertices: CAState.rectangle(
                            x0: x0, y0: y0, x1: x0 + 0.5, y1: y0 + 0.5, color: Self.colour(round, draw)),
                        source: nil)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
                let blit = commands.makeBlitCommandEncoder()!
                let size = MTLSize(width: Self.width, height: Self.height, depth: 1)
                blit.copy(
                    from: target.texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(), sourceSize: size,
                    to: copy, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
                blit.endEncoding()
            }
            result.expect(!timing.path.hasPrefix("error:"), "round \(round): \(timing.path)")
            let copied = try await gpu.read(copy, bytesPerPixel: 4)
            let drawn = try await gpu.read(target.texture, bytesPerPixel: 4)
            if copied != drawn { stale += 1 }
            if round == 0 || copied != drawn {
                result.compare(
                    "round \(round): the copy against the surface", bytes: copied, expected: drawn,
                    format: .bgra8Unorm, width: Self.width, height: Self.height, tolerance: 0)
                result.compare(
                    "round \(round): the CPU lock against the surface", bytes: try target.cpuRead(), expected: drawn,
                    format: .bgra8Unorm, width: Self.width, height: Self.height, tolerance: 0)
            }
            if round == 0 {
                result.picture(
                    "round0", bytes: drawn, metalFormat: MTLPixelFormat.bgra8Unorm.rawValue, width: Self.width,
                    height: Self.height, cleared: Self.sentinelTexel)
            }
            result.metrics.set("path\(round)", timing.path)
        }
        result.metrics.set("stale", stale)
        return result
    }

    static func colour(_ round: Int, _ draw: Int) -> SIMD4<Float16> {
        let a = Float16(Float((round * 37 + draw * 11) % 64) / 63)
        let b = Float16(Float((round * 13 + draw * 29) % 64) / 63)
        return SIMD4(a, b, 1 - a, 1)
    }
}
