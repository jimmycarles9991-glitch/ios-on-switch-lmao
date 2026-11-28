import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// More RGB10A8_2P_XR10 (550) storage images in one pass than a draw binds: 25, then 40, 64x64 textures over 'b3a8'
/// pixel buffers, usage ShaderRead|ShaderWrite, written by one pass into a BGRA8 colour that draws a full quad per 16
/// of them, its fragment function writing (0.5, 0.25, -0.25, 0.75) at the pixel into all 16 bound (the last draw binds
/// its first texture again in the slots it has none for). As on an M4 Max (m2v-textures5): the pass completes, and
/// every texel of every texture holds the word 0x27f80100 (bits 30-31 clear) and the alpha byte 191. status: command
/// buffers that failed; path: their paths; textures_written: textures every texel of which holds both, for each count
/// ("25/40"); words_off and alpha_off: texels that do not, over both.
struct XrImagesManyTest: HWTest {
    let id = "gpu.gm20b.xr_images_many"
    let name = "25 and 40 RGB10A8_2P_XR10 storage images written in one pass, 16 a draw, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 64, slots = 16, counts = [25, 40]
    static let word: UInt32 = 0x27f8_0100, alpha: UInt8 = 191

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            var paths: [String] = [], written: [String] = []
            var wordsOff = 0, alphaOff = 0
            for count in Self.counts {
                let (path, texels) = try await Self.written(gpu, count)
                paths.append(path)
                guard let texels else { continue }
                written.append(String(texels.filter { $0.allSatisfy { $0 == Self.texel } }.count))
                for t in texels.joined() {
                    wordsOff += UInt32(t & 0xffff_ffff) == Self.word ? 0 : 1
                    alphaOff += UInt8(t >> 32) == Self.alpha ? 0 : 1
                }
            }
            let failed = paths.filter { $0.hasPrefix("error:") }
            metrics.set("status", failed.count)
            metrics.set("path", paths.joined(separator: "/"))
            metrics.set("textures_written", written.joined(separator: "/"))
            metrics.set("words_off", wordsOff)
            metrics.set("alpha_off", alphaOff)
            checks.append(("both passes complete", failed.isEmpty))
            checks.append(("every texture holds the write", written == Self.counts.map(String.init)))
        } catch {
            checks.append(("\(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static let texel = UInt64(alpha) << 32 | UInt64(word)

    static func image(_ gpu: GPU) throws -> (CVPixelBuffer, MTLTexture) {
        var made: CVPixelBuffer?
        let attributes: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        guard
            CVPixelBufferCreate(
                nil, side, side, kCVPixelFormatType_30RGBLE_8A_BiPlanar, attributes as CFDictionary, &made
            )
            == kCVReturnSuccess, let buffer = made,
            let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { throw TestFailure("no b3a8 pixel buffer") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: XRTwoPlaneTest.format, width: side, height: side, mipmapped: false
        )
        descriptor.usage = [.shaderRead, .shaderWrite]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no 550 storage texture over the b3a8 surface")
        }
        return (buffer, texture)
    }

    /// The pass's path, and each texture's texels (the alpha byte above the word) when it completed
    static func written(_ gpu: GPU, _ count: Int) async throws -> (String, [[UInt64]]?) {
        let images = try (0 ..< count).map { _ in try image(gpu) }
        let colour = try gpu.texture(.bgra8Unorm, side, side)
        let state = try gpu.render(fragment: "xr_write_sixteen", format: .bgra8Unorm)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            for first in stride(from: 0, to: count, by: slots) {
                for slot in 0 ..< slots {
                    let i = first + slot < count ? first + slot : first
                    encoder.setFragmentTexture(images[i].1, index: slot)
                }
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        guard !timing.path.hasPrefix("error:") else { return (timing.path, nil) }
        return (timing.path, images.map { XRTwoPlaneTest.planes($0.0, width: side, height: side) })
    }
}
