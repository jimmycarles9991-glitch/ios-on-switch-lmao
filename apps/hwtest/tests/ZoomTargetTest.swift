import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Core Animation's zoom window target of an app opening and closing (boot 21: 1792x1472 BGRA8, cleared and redrawn
/// every frame, beside a memoryless RGBA16Float second colour), drawn with Core Animation's own fill state: a target made
/// plain for want of compressed memory moves there once some is made, its texels with it (raw_private_comp.h
/// compressedFromPlain); a closed target's memory serves the next one at its creation; the memoryless second colour's
/// clear covers only the tiles of the draws that see it (raw_clear_box.h). Fields: first_at_creation and first_after
/// (comptags of the first target), moved_bytes_differ (against the same draw into a target held plain), next_at_creation,
/// compressed_moves, clears_narrowed and clear_pixels_left (this test's), paths.
struct ZoomTargetTest: HWTest {
    let id = "gpu.raw.zoom_target"
    let name =
        "Zoom window target: moved onto compressed memory once made, reused at creation, memoryless clear narrowed"
    var subsystem: Subsystem { .metal }
    static let width = 1792, height = 1472
    static let plainSelector = NSSelectorFromString("switchRawProbePlainPrivate:")
    static let factsSelector = NSSelectorFromString("switchRawProbeFacts:")

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.plainSelector) else {
            return .skip("a release facade: no switch to hold compression off for the reference")
        }
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            return .skip("no QuartzCore library here (\(GPU.describe(error)))")
        }
        guard let line = CACorpus.state("PBGRAXm_A2Xhf") else { return .fail("CACorpus has no PBGRAXm_A2Xhf") }
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let scratch = try gpu.texture(
            CACorpus.colorFormat(line, 1), Self.width, Self.height, usage: [.renderTarget], storage: .memoryless)
        let before = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: [String] = []
        var first: MTLTexture? = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let firstAtCreation = comptags(facade, first!)
        paths.append(try await draw(gpu, pipeline, first!, scratch))
        try await Task.sleep(nanoseconds: 200_000_000)
        let bytes = try await gpu.read(first!, bytesPerPixel: 4)
        let firstAfter = comptags(facade, first!)
        _ = facade.perform(Self.plainSelector, with: NSNumber(value: true))
        let reference = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        paths.append(try await draw(gpu, pipeline, reference, scratch))
        let want = try await gpu.read(reference, bytesPerPixel: 4)
        _ = facade.perform(Self.plainSelector, with: NSNumber(value: false))
        let differing = zip(bytes, want).filter { $0 != $1 }.count
        first = nil
        let next = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let nextAtCreation = comptags(facade, next)
        paths.append(try await draw(gpu, pipeline, next, scratch))
        let after = RawFacade.counters(of: gpu.device)
        let delta = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        metrics.set("first_at_creation", firstAtCreation)
        metrics.set("first_after", firstAfter)
        metrics.set("moved_bytes_differ", differing)
        metrics.set("next_at_creation", nextAtCreation)
        for key in ["private_compressed_moves", "clears_narrowed", "clear_pixels_left"] {
            guard after[key] != nil else {
                wrong.append("no_counter_" + key)
                continue
            }
            metrics.set(key == "private_compressed_moves" ? "compressed_moves" : key, delta(key))
        }
        metrics.set("paths", paths.joined(separator: ","))
        if paths.contains(where: { $0 != "raw" }) { wrong.append("paths") }
        if firstAfter != 1 { wrong.append(firstAtCreation == 1 ? "first_not_compressed" : "not_moved") }
        if differing != 0 { wrong.append("moved_bytes") }
        if nextAtCreation != 1 { wrong.append("not_reused") }
        if delta("clears_narrowed") < 3 { wrong.append("clear_not_narrowed") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// One pass as Core Animation's over the zoom target: its colour cleared and stored, the memoryless second colour
    /// cleared and not stored, one rectangle under a 520x250 scissor
    func draw(
        _ gpu: GPU, _ pipeline: MTLRenderPipelineState, _ target: MTLTexture, _ scratch: MTLTexture
    ) async throws
        -> String
    {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[1].texture = scratch
            pass.colorAttachments[1].loadAction = .clear
            pass.colorAttachments[1].storeAction = .dontCare
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            CAState.bind(
                encoder,
                vertices: CAState.rectangle(
                    x0: -0.75, y0: -0.5, x1: 0.25, y1: 0.4, color: SIMD4<Float16>(0.75, 0.5, 0.25, 1)),
                source: nil)
            encoder.setScissorRect(MTLScissorRect(x: 250, y: 130, width: 520, height: 250))
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }.path
    }

    func comptags(_ facade: NSObject, _ texture: MTLTexture) -> Int {
        let line = facade.perform(Self.factsSelector, with: texture)?.takeUnretainedValue() as? String ?? ""
        return line.contains("comptags=1") ? 1 : 0
    }
}
