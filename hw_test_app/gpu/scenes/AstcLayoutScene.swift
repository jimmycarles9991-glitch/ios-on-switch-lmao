import Darwin
import Foundation
import HWTestCore
import Metal

/// One 128x128 ASTC 4x4 image (every block a void-extent block of its own colour, AstcBlockGrid) put into a texture
/// every way an app or Core Animation can, each sampled texel by texel by a compute kernel and named by the block it
/// came from:
/// - texture: a shared texture filled by replaceRegion;
/// - blit: a private texture filled by a blit from a buffer;
/// - nocopy, nocopy2: Core Animation's way (MetalContext::update_texture): the device's layout object lays the blocks
///   out in memory and newTextureWithBytesNoCopy wraps it, at every 512-byte base inside a 16 KiB page, twice, the
///   second time over new pages while the first round's textures still hold theirs;
/// - replace: a no-copy texture over zeroed memory, filled after by replaceRegion;
/// - rewrite: a no-copy texture over zeroed memory, filled after by the layout object;
/// - blitin: a no-copy texture over zeroed memory, filled after by a blit from a buffer; blitin_back: its caller's
///   memory read back through getBytes;
/// - alias: as nocopy, over memory a no-copy buffer already imported and the GPU already read;
/// - ce: the copy engine's readback of the nocopy textures' blocks.
/// On the raw facade a base off the layout's alignment (4096 bytes here: bits 0, 8, 16 and 24 of a _bad mask are the
/// aligned bases) is a pool image the copy engine fills from the caller's memory, and the aligned ones are that memory.
/// texture= and blit= are wrong blocks; <label>_bad= says which of the 32 bases (bit n: base n x 512) had any, as 8 hex
/// digits, <label>_wrong= their wrong blocks summed. map=, map2= and map3= are the first three different address bit
/// mappings the wrong cases showed (BlockLinearMap.bitMap: displayed bit gK from written bits wN), each after the first
/// case that showed it; maps= how many different ones the first three wrong bases of each path showed.
/// On the raw facade a no-copy texture sampled off raw is counted in <label>_off_raw= (m2v samples nothing there), not
/// as wrong. Only the raw facade takes the no-copy paths: the m2v device refuses no-copy textures, and a Mac's AGX
/// device answers the selector but throws inside it.
struct AstcLayoutScene: GPUScene {
    let id = "gpu.astc.layouts"
    let name = "One ASTC 4x4 image through every way in and every no-copy base, read block by block"

    func run(_ gpu: GPU) async throws -> SceneResult {
        let run = try AstcLayoutRun(gpu)
        await run.uploaded("texture", storage: .shared)
        await run.uploaded("blit", storage: .private)
        if run.onRaw { await run.noCopyPaths() }
        return run.finish()
    }
}

final class AstcLayoutRun {
    static let grid = AstcBlockGrid(across: 32, down: 32)
    static let bases = Array(stride(from: 0, to: 16384, by: 512))

    let gpu: GPU
    let onRaw: Bool
    let sampler: MTLComputePipelineState
    var result = SceneResult()
    var maps: [(label: String, map: String)] = []
    var textures: [MTLTexture] = []
    var buffers: [MTLBuffer] = []

    init(_ gpu: GPU) throws {
        self.gpu = gpu
        onRaw = RawFacade.kind(of: gpu.device) == "raw"
        sampler = try gpu.compute("sample_texture")
    }

    func finish() -> SceneResult {
        textures.removeAll()
        buffers.removeAll()
        var distinct: [(label: String, map: String)] = []
        for case (let label, let map) in maps where !distinct.contains(where: { $0.map == map }) {
            distinct.append((label, map))
        }
        result.metrics.set("maps", distinct.count)
        for (index, shown) in distinct.prefix(3).enumerated() {
            result.metrics.set(index == 0 ? "map" : "map\(index + 1)", "\(shown.label):\(shown.map)")
        }
        return result
    }

    static func descriptor(_ storage: MTLStorageMode) -> MTLTextureDescriptor {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .astc_4x4_ldr, width: grid.texelWidth, height: grid.texelHeight, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = storage
        return descriptor
    }

    static var region: MTLRegion { MTLRegionMake2D(0, 0, grid.texelWidth, grid.texelHeight) }

    func uploaded(_ label: String, storage: MTLStorageMode) async {
        guard let texture = gpu.device.makeTexture(descriptor: Self.descriptor(storage)) else {
            result.metrics.set(label, "nil")
            result.expect(!onRaw, "\(label): makeTexture(astc_4x4_ldr) returned nil")
            return
        }
        do {
            if storage == .private {
                try await gpu.upload(Self.grid.bytes, to: texture, bytesPerRow: Self.grid.rowBytes)
            } else {
                Self.grid.bytes.withUnsafeBytes {
                    texture.replace(
                        region: Self.region, mipmapLevel: 0, withBytes: $0.baseAddress!,
                        bytesPerRow: Self.grid.rowBytes)
                }
            }
            let reading = try await sample(texture, noCopy: false)
            result.metrics.set(label, reading.wrong)
            if reading.wrong > 0 { maps.append((label, reading.map)) }
            result.expect(reading.wrong == 0, "\(label): \(reading.wrong) wrong blocks")
        } catch {
            result.metrics.set(label, "error")
            result.expect(false, "\(label): \(error)")
        }
    }

    func noCopyPaths() async {
        guard let layout = TextureLayoutSPI(gpu.device, Self.descriptor(.shared)) else {
            result.notes.append("no texture layout SPI")
            result.expect(!onRaw, "newTextureLayoutWithDescriptor returned nil")
            return
        }
        result.metrics.set("layout", "\(layout.className):\(layout.size):\(layout.alignment)")
        for label in ["nocopy", "nocopy2", "replace", "rewrite", "blitin", "alias"] {
            var tally = AstcLayoutTally(label)
            var copied = AstcLayoutTally(label == "blitin" ? "blitin_back" : "ce")
            for (index, base) in Self.bases.enumerated() {
                do {
                    guard let texture = try await noCopy(layout, label, base) else {
                        result.notes.append("\(label): no no-copy texture")
                        result.expect(!onRaw, "\(label)@\(base): newTextureWithBytesNoCopy returned nil")
                        break
                    }
                    tally.add(try await sample(texture, noCopy: true), index, base, &maps)
                    if label == "nocopy" { copied.add(try await copyEngine(texture), index, base, &maps) }
                    if label == "blitin" { copied.add(readBack(texture), index, base, &maps) }
                } catch {
                    tally.errors += 1
                    if tally.errors == 1 { result.notes.append("\(label)@\(base): \(error)") }
                }
            }
            tally.report(&result)
            if label == "nocopy" || label == "blitin" { copied.report(&result) }
        }
    }

    /// One no-copy texture at `base` bytes into fresh 16 KiB-aligned pages, filled the way `label` names.
    func noCopy(_ layout: TextureLayoutSPI, _ label: String, _ base: Int) async throws -> MTLTexture? {
        let length = max(layout.size, 16384)
        let memory = try PageAlignedMemory(bytes: (base + length + 16383) / 16384 * 16384)
        let at = memory.base + base
        var deallocator: TextureLayoutSPI.Deallocator? = memory.deallocator
        if label == "alias" {
            guard
                let buffer = gpu.device.makeBuffer(
                    bytesNoCopy: memory.base, length: memory.bytes, options: .storageModeShared,
                    deallocator: { pointer, _ in free(pointer) })
            else {
                memory.release()
                throw TestFailure("no-copy buffer creation failed")
            }
            buffers.append(buffer)
            deallocator = nil
            let scratch = try gpu.buffer(memory.bytes)
            try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(from: buffer, sourceOffset: 0, to: scratch, destinationOffset: 0, size: memory.bytes)
                blit.endEncoding()
            }
        }
        let filledAfter = ["replace", "rewrite", "blitin"].contains(label)
        if !filledAfter {
            layout.copy(Self.grid.bytes, rowBytes: Self.grid.rowBytes, region: Self.region, to: at)
        }
        guard
            let texture = TextureLayoutSPI.noCopyTexture(
                gpu.device, at, length: length, Self.descriptor(.shared), deallocator: deallocator)
        else {
            if deallocator != nil { memory.release() }
            return nil
        }
        textures.append(texture)
        switch label {
        case "replace":
            Self.grid.bytes.withUnsafeBytes {
                texture.replace(
                    region: Self.region, mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: Self.grid.rowBytes)
            }
        case "rewrite":
            layout.copy(Self.grid.bytes, rowBytes: Self.grid.rowBytes, region: Self.region, to: at)
        case "blitin":
            try await blitIn(texture)
        default:
            break
        }
        return texture
    }

    /// The image blitted into `texture` from a buffer, on raw: m2v blits nothing into a no-copy texture.
    func blitIn(_ texture: MTLTexture) async throws {
        let staging = try gpu.buffer(Self.grid.bytes)
        let timing = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: Self.grid.rowBytes, sourceBytesPerImage: 0,
                sourceSize: MTLSize(width: Self.grid.texelWidth, height: Self.grid.texelHeight, depth: 1), to: texture,
                destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding()
        }
        if timing.path != "raw" { throw TestFailure("the blit into it ran on \(timing.path)") }
    }
}

/// One label's no-copy textures over the 32 bases.
struct AstcLayoutTally {
    let label: String
    var bad: UInt32 = 0
    var wrong = 0
    var offRaw = 0
    var errors = 0
    var mapped = 0

    init(_ label: String) { self.label = label }

    mutating func add(
        _ reading: AstcLayoutReading, _ index: Int, _ base: Int, _ maps: inout [(label: String, map: String)]
    ) {
        guard reading.measured else {
            offRaw += 1
            return
        }
        guard reading.wrong > 0 else { return }
        bad |= 1 << index
        wrong += reading.wrong
        mapped += 1
        if mapped <= 3 { maps.append(("\(label)@\(base)", reading.map)) }
    }

    func report(_ result: inout SceneResult) {
        let mask = String(format: "%08x", bad)
        result.metrics.set(label + "_bad", mask)
        result.metrics.set(label + "_wrong", wrong)
        if offRaw > 0 { result.metrics.set(label + "_off_raw", offRaw) }
        if errors > 0 { result.metrics.set(label + "_errors", errors) }
        result.expect(bad == 0, "\(label): \(wrong) wrong blocks over bases \(mask)")
        result.expect(offRaw == 0, "\(label): \(offRaw) samples left raw")
        result.expect(errors == 0, "\(label): \(errors) bases failed")
    }
}
