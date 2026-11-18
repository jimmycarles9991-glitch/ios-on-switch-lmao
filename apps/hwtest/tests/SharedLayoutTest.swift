import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Shared-storage textures in block-linear memory (drivers/TegraGM20BMetal/raw/runtime/RawMetalDevice.mm
/// newTextureWithDescriptor: every one-level 2D Shared texture is block-linear, as a Private one is): a 512x512 BGRA8 and a
/// 333x77 R8 texture written whole by replaceRegion, then an unaligned rectangle of each, read back by getBytes and
/// sampled by the texture unit (shared_read_fragment into a private target the copy engine reads), every byte against
/// the CPU's own. Reports the CPU milliseconds of the whole replaceRegion and getBytes, and the GPU milliseconds of 16
/// sampling passes; facts names the layout on a debug facade (block_linear=1).
struct SharedLayoutTest: HWTest {
    let id = "gpu.raw.shared_layout"
    let name = "Shared textures in block-linear memory: replaceRegion, getBytes and sampling exact, and their cost"
    let subsystem = Subsystem.metal

    static let factsSelector = NSSelectorFromString("switchRawProbeFacts:")

    struct Case {
        let name: String
        let format: MTLPixelFormat
        let width: Int, height: Int, bpp: Int
    }

    static let cases = [
        Case(name: "bgra8", format: .bgra8Unorm, width: 512, height: 512, bpp: 4),
        Case(name: "r8", format: .r8Unorm, width: 333, height: 77, bpp: 1),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        for c in Self.cases { try await check(gpu, c, &metrics, &wrong) }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    static func pattern(_ c: Case, seed: UInt32, width: Int, height: Int) -> [UInt8] {
        (0..<width * height * c.bpp).map { i in
            var h = UInt32(truncatingIfNeeded: i) &* 2_654_435_761 ^ seed &* 40503
            h ^= h >> 15
            return UInt8(truncatingIfNeeded: h)
        }
    }

    func check(_ gpu: GPU, _ c: Case, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        let texture = try gpu.texture(c.format, c.width, c.height, usage: [.shaderRead], storage: .shared)
        if let facade = gpu.device as? NSObject, facade.responds(to: Self.factsSelector) {
            let facts = facade.perform(Self.factsSelector, with: texture)?.takeUnretainedValue() as? String ?? "-"
            metrics.set("\(c.name)_facts", facts)
            if !facts.contains("block_linear=1") { wrong.append("\(c.name)_not_block_linear") }
        }
        var model = Self.pattern(c, seed: 1, width: c.width, height: c.height)
        let row = c.width * c.bpp
        var started = CFAbsoluteTimeGetCurrent()
        texture.replace(
            region: MTLRegionMake2D(0, 0, c.width, c.height), mipmapLevel: 0, withBytes: model, bytesPerRow: row)
        metrics.set("\(c.name)_replace_ms", (CFAbsoluteTimeGetCurrent() - started) * 1000, digits: 3)
        let (x, y, w, h) = (c.width / 5 + 3, c.height / 7 + 1, c.width / 2 + 1, c.height / 3 + 2)
        let patch = Self.pattern(c, seed: 2, width: w, height: h)
        texture.replace(region: MTLRegionMake2D(x, y, w, h), mipmapLevel: 0, withBytes: patch, bytesPerRow: w * c.bpp)
        for line in 0..<h {
            model.replaceSubrange(
                (y + line) * row + x * c.bpp..<(y + line) * row + (x + w) * c.bpp,
                with: patch[line * w * c.bpp..<(line + 1) * w * c.bpp])
        }
        var back = [UInt8](repeating: 0, count: model.count)
        started = CFAbsoluteTimeGetCurrent()
        texture.getBytes(&back, bytesPerRow: row, from: MTLRegionMake2D(0, 0, c.width, c.height), mipmapLevel: 0)
        metrics.set("\(c.name)_getbytes_ms", (CFAbsoluteTimeGetCurrent() - started) * 1000, digits: 3)
        let cpuWrong = zip(back, model).filter { $0 != $1 }.count
        metrics.set("\(c.name)_cpu_wrong", cpuWrong)
        if cpuWrong > 0 { wrong.append("\(c.name)_cpu") }

        let into = try gpu.texture(c.format, c.width, c.height, usage: [.renderTarget, .shaderRead])
        let state = try gpu.render(fragment: "shared_read_fragment", format: c.format)
        let sample: (MTLCommandBuffer) -> Void = { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = into
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let path = try await gpu.run(sample).path
        let sampled = try await gpu.read(into, bytesPerPixel: c.bpp)
        let sampledWrong = zip(sampled, model).filter { $0 != $1 }.count
        metrics.set("\(c.name)_sampled_wrong", sampledWrong)
        metrics.set("\(c.name)_path", path)
        if sampledWrong > 0 { wrong.append("\(c.name)_sampled") }
        if path != "raw" { wrong.append("\(c.name)_path") }
        let sixteen = try await gpu.run { commands in for _ in 0..<16 { sample(commands) } }
        metrics.set("\(c.name)_sample16_gpu_ms", sixteen.gpuMilliseconds, digits: 3)
    }
}
