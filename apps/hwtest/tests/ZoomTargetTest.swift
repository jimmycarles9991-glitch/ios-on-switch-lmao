import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Core Animation's zoom window target of an app opening and closing (boot 21: 1792x1472 BGRA8, cleared and redrawn
/// every frame, beside a memoryless RGBA16Float second colour), drawn with Core Animation's own fill state
/// (ZoomTargetScene): a target made plain for want of compressed memory moves there once some is made (the compression
/// thread's event, CompressionSettled), its texels with it (private_comp.h compressedFromPlain), at the next
/// submission; a closed target's memory serves the next one at its creation; the
/// memoryless second colour's clear covers only the tiles of the draws that see it (clear_box.h). Redrawn: the
/// first target is sampled by a kernel before its move, and after it a pass loads it and draws a solid rectangle, and
/// the kernel samples it again: the texture unit reads the redrawn texels through the header the move rewrote in place,
/// where a header left naming the plain memory reads the old ones (redrawn_samples_off 0, the M4 Max's codes). Fields:
/// first_at_creation and first_after (comptags of the first target), moved_bytes_differ (against the same draw into a
/// target held plain), next_at_creation, compressed_moves, redrawn_samples_off, clears_narrowed and clear_pixels_left
/// (this test's), paths.
struct ZoomTargetTest: HWTest {
    let id = "gpu.gm20b.zoom_target"
    let name =
        "Zoom window target: moved onto compressed memory once made, reused at creation, memoryless clear narrowed"
    var subsystem: Subsystem { .metal }
    static let width = ZoomTargetScene.width, height = ZoomTargetScene.height
    static let plainSelector = NSSelectorFromString("switchMetalProbePlainPrivate:")
    static let factsSelector = NSSelectorFromString("switchMetalProbeFacts:")

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.plainSelector) else {
            return .skip(GM20BFacade.missing("switchMetalProbePlainPrivate:", on: gpu.device))
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
            CACorpus.colorFormat(line, 1), Self.width, Self.height, usage: [.renderTarget], storage: .memoryless
        )
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: [String] = []
        var first: MTLTexture? = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let firstAtCreation = comptags(facade, first!)
        paths.append(try await ZoomTargetScene.draw(gpu, pipeline, first!, scratch))
        var redrawnOff = -1
        do {
            paths.append(try await ZoomTargetScene.sampled(gpu, first!).1)
        } catch {
            wrong.append("early_sample")
        }
        try await CompressionSettled.settle(gpu.device)
        let bytes = try await gpu.read(first!, bytesPerPixel: 4)
        let firstAfter = comptags(facade, first!)
        do {
            paths.append(try await ZoomTargetScene.redraw(gpu, first!))
            let (redrawn, sampledPath) = try await ZoomTargetScene.sampled(gpu, first!)
            paths.append(sampledPath)
            redrawnOff = zip(redrawn, ZoomTargetScene.redrawnCodes).filter { $0 != $1 }.count
        } catch {
            wrong.append("redrawn_failed")
        }
        _ = facade.perform(Self.plainSelector, with: NSNumber(value: true))
        let reference = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        paths.append(try await ZoomTargetScene.draw(gpu, pipeline, reference, scratch))
        let want = try await gpu.read(reference, bytesPerPixel: 4)
        _ = facade.perform(Self.plainSelector, with: NSNumber(value: false))
        let differing = zip(bytes, want).filter { $0 != $1 }.count
        first = nil
        let next = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let nextAtCreation = comptags(facade, next)
        paths.append(try await ZoomTargetScene.draw(gpu, pipeline, next, scratch))
        let after = GM20BFacade.counters(of: gpu.device)
        let delta = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        metrics.set("first_at_creation", firstAtCreation)
        metrics.set("first_after", firstAfter)
        metrics.set("moved_bytes_differ", differing)
        metrics.set("next_at_creation", nextAtCreation)
        metrics.set("redrawn_samples_off", redrawnOff)
        for key in ["private_compressed_moves", "clears_narrowed", "clear_pixels_left"] {
            guard after[key] != nil else {
                wrong.append("no_counter_" + key)
                continue
            }
            metrics.set(key == "private_compressed_moves" ? "compressed_moves" : key, delta(key))
        }
        metrics.set("paths", paths.joined(separator: ","))
        if paths.contains(where: { $0 != "gm20b" }) { wrong.append("paths") }
        if firstAfter != 1 { wrong.append(firstAtCreation == 1 ? "first_not_compressed" : "not_moved") }
        if differing != 0 { wrong.append("moved_bytes") }
        if redrawnOff > 0 { wrong.append("redrawn_samples") }
        if nextAtCreation != 1 { wrong.append("not_reused") }
        if delta("clears_narrowed") < 3 { wrong.append("clear_not_narrowed") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    func comptags(_ facade: NSObject, _ texture: MTLTexture) -> Int {
        let line = facade.perform(Self.factsSelector, with: texture)?.takeUnretainedValue() as? String ?? ""
        return line.contains("comptags=1") ? 1 : 0
    }
}
