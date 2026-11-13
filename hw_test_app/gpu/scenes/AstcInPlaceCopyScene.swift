import Foundation
import HWTestCore
import Metal

/// The copy engine in and out of a no-copy ASTC 4x4 texture that is its caller's memory (gpu.astc.layouts' image at a
/// 16 KiB page's first byte, on the layout's alignment, so in place):
/// - readback: its blocks copied out to a buffer;
/// - blitin: a blit from a buffer into it over zeroed memory, then its readback; blitin_back: its caller's memory
///   through getBytes;
/// - layer1: the readback of the second layer of a two-layer array, only that layer written.
/// Each must read the blocks written, on raw, with no channel killed: boot 14's gpu.astc.layouts hung the copy engine
/// in a context save on every such copy (a CTXSW_TIMEOUT and about 500 ms of the compositor's each).
struct AstcInPlaceCopyScene: GPUScene {
    let id = "gpu.astc.inplace_copy"
    let name = "The copy engine in and out of a no-copy ASTC texture over its caller's memory"

    func run(_ gpu: GPU) async throws -> SceneResult {
        let run = try AstcLayoutRun(gpu)
        guard run.onRaw else {
            run.result.notes.append("off the raw facade: only raw takes the no-copy paths")
            return run.finish()
        }
        guard let layout = TextureLayoutSPI(gpu.device, AstcLayoutRun.descriptor(.shared)) else {
            run.result.expect(false, "newTextureLayoutWithDescriptor returned nil")
            return run.finish()
        }
        await run.inPlace("readback") {
            try await run.copyEngine(try run.wrapped(layout, "nocopy"))
        }
        var blitted: MTLTexture?
        await run.inPlace("blitin") {
            let texture = try await run.wrapped(layout, "blitin")
            blitted = texture
            return try await run.copyEngine(texture)
        }
        if let blitted { await run.inPlace("blitin_back") { run.readBack(blitted) } }
        await run.inPlace("layer1") { try await run.secondLayer() }
        return run.finish()
    }
}

extension AstcLayoutRun {
    /// One check of the scene: `read`'s wrong blocks, or the error that stopped it.
    func inPlace(_ label: String, _ read: () async throws -> AstcLayoutReading) async {
        do {
            let reading = try await read()
            result.metrics.set("inplace_\(label)", reading.wrong)
            if reading.wrong > 0 { maps.append((label, reading.map)) }
            result.expect(reading.wrong == 0, "\(label): \(reading.wrong) wrong blocks")
        } catch {
            result.metrics.set("inplace_\(label)", "error")
            result.expect(false, "\(label): \(error)")
        }
    }

    /// gpu.astc.layouts' no-copy texture of `label` at base 0, in place.
    func wrapped(_ layout: TextureLayoutSPI, _ label: String) async throws -> MTLTexture {
        guard let texture = try await noCopy(layout, label, 0) else {
            throw TestFailure("newTextureWithBytesNoCopy returned nil")
        }
        return texture
    }

    /// The copy engine's readback of layer 1 of a two-layer no-copy array whose layer 0 is zeros.
    func secondLayer() async throws -> AstcLayoutReading {
        let descriptor = Self.descriptor(.shared)
        descriptor.textureType = .type2DArray
        descriptor.arrayLength = 2
        guard let layout = TextureLayoutSPI(gpu.device, descriptor) else {
            throw TestFailure("no layout of a two-layer array")
        }
        let memory = try PageAlignedMemory(bytes: (layout.size + 16383) / 16384 * 16384)
        layout.copy(Self.grid.bytes, rowBytes: Self.grid.rowBytes, region: Self.region, slice: 1, to: memory.base)
        guard
            let texture = TextureLayoutSPI.noCopyTexture(
                gpu.device, memory.base, length: memory.bytes, descriptor, deallocator: memory.deallocator)
        else {
            memory.release()
            throw TestFailure("newTextureWithBytesNoCopy of the array returned nil")
        }
        textures.append(texture)
        return try await copyEngine(texture, slice: 1)
    }
}
