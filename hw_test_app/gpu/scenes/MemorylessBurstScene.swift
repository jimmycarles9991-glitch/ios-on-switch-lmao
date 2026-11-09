import Foundation
import HWTestCore
import Metal

/// A pass's memoryless attachment is backed by a view of memory its attachment slot's passes share, taken in size
/// classes, as compressed memory is: a pass that fits the largest the slot holds takes a view of it, one that does not
/// takes a larger class, and the smaller memory goes once its last use completes. Boot 15's keyboard drew Core
/// Animation's passes at a new size every frame; backboardd's chunks grew from 356 to 953 MB in 35 s and stayed at 912
/// MB. This draws one pass a frame into a memoryless RGBA16Float attachment that grows 8 x 5 pixels a frame for
/// `frames` frames, then shrinks back through sizes 4 pixels off those, then enough small passes for the backings to
/// expire. backings_grow_kb and backings_shrink_kb are the backings' memory held after each half: the shrinking half
/// must take none. pool_before_kb, pool_peak_kb and pool_after_kb are the raw pool's chunks then, backings_after_kb the
/// backings' memory still held and grows_burst the allocations of the burst that wired a chunk themselves (a client has
/// no grower thread: the render server's line says grow=). On the raw facade nothing may be held after, and the pool
/// must come back to within two chunks of where it started.
struct MemorylessBurstScene: GPUScene {
    let id = "gpu.raw.memoryless_burst"
    let name = "Memoryless attachments at a new size every frame: what the raw pool holds after"

    static let frames = 48
    static let settle = 680  // a backing's 600 command buffers past its last use, then the trim at the next 64th
    static let warmups = 20
    static let slack = 8 << 20  // two of a client's largest chunks

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            result.notes.append("the raw facade's pool only")
            return result
        }
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba16Float)
        let draw = { (scratch: MTLTexture) in
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = scratch
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .dontCare
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0), color: SIMD4(1, 0.5, 0, 1))
                encoder.endEncoding()
            }
        }
        // the state, made at its first use where the pack lacks it, before anything is counted
        let first = try gpu.texture(.rgba16Float, 16, 16, usage: [.renderTarget], storage: .memoryless)
        for _ in 0..<Self.warmups {
            guard try await draw(first).path.hasPrefix("m2v:state") else { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        let before = RawFacade.memory(of: gpu.device)
        let sizes =
            (0..<Self.frames).map { (320 + 8 * $0, 180 + 5 * $0) }
            + (0..<Self.frames).map { (316 + 8 * (Self.frames - 1 - $0), 176 + 5 * (Self.frames - 1 - $0)) }
        var grown: [String: Int] = [:]
        for (frame, (width, height)) in sizes.enumerated() {
            if frame == Self.frames { grown = RawFacade.memory(of: gpu.device) }
            let scratch = try gpu.texture(.rgba16Float, width, height, usage: [.renderTarget], storage: .memoryless)
            let timing = try await draw(scratch)
            result.expect(timing.path == "raw", "frame \(frame) drawn on \(timing.path)")
        }
        let peak = RawFacade.memory(of: gpu.device)
        if let atGrown = grown["backings"], let atShrunk = peak["backings"] {
            result.metrics.set("backings_grow_kb", atGrown >> 10)
            result.metrics.set("backings_shrink_kb", atShrunk >> 10)
            result.expect(
                atShrunk <= atGrown, "the shrinking frames made \((atShrunk - atGrown) >> 10) KiB of backings")
        }
        let small = try gpu.texture(.bgra8Unorm, 16, 16)
        for _ in 0..<Self.settle {
            _ = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = small
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
        }
        let after = RawFacade.memory(of: gpu.device)
        guard let pool = before["pool"], let held = after["pool"], let backings = after["backings"] else {
            result.notes.append("the raw facade answered no memory (switchRawMemory)")
            return result
        }
        result.metrics.set("pool_before_kb", pool >> 10)
        result.metrics.set("pool_peak_kb", (peak["pool"] ?? 0) >> 10)
        result.metrics.set("pool_after_kb", held >> 10)
        result.metrics.set("backings_after_kb", backings >> 10)
        result.metrics.set("grows_burst", (peak["grows"] ?? 0) - (before["grows"] ?? 0))
        result.expect(backings == 0, "\(backings >> 10) KiB of backings still held \(Self.settle) command buffers on")
        result.expect(
            held - pool <= Self.slack, "the pool holds \((held - pool) >> 10) KiB more than before the burst")
        return result
    }
}
