import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUDecode
import HWTestKit
import Metal

/// A view of a decoded texture holds its parent, which holds the kept blocks (TegraGM20BMetalDecodedFamily.mm): a 203
/// texture's 185 view, the texture's last reference let go, takes an upload of the rotated blocks and reads its own
/// decode of them (`decode_view_outlives_off` 0, against a 185 texture holding those blocks). Then a 203 texture with
/// views of other forms (two 185 views, and on 24B119 a 221 one), each let go while a command buffer uploading new
/// blocks into the texture is in flight: none fails (`decode_view_release_failed` 0) and the texture reads its decode
/// of the last blocks. Once all of it is gone and one more command buffer completed, the device holds no more texture
/// headers than before (`decode_memory_left` at most 0, of switchMetalMemory headers_left, which is the process's: below
/// 0 is earlier tests' retired backings going meanwhile) and no more texture memory, the textures' and views' images and
/// the kept blocks (`decode_texture_bytes_left` at most 0, of switchMetalMemory textures; the pool and write-combined
/// bytes reported). Whether a view goes before its upload was encoded is the worker's timing; either order must hold.
/// Red: a view whose image or blocks went with the texture, a decode into a view already gone, or headers, images or
/// kept blocks a family keeps.
struct DecodeViewLifetimeTest: HWTest {
    let id = "gpu.decode.view_lifetime"
    let name = "Views of decoded textures outlive their texture, and their family's memory goes with the last of them"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.memory(of: gpu.device)
        let set = DecodeVectors.set(203)
        guard !set.blocks.isEmpty else { return .fail("no 203 vectors") }
        let outlives = try await Self.outliving(gpu, set)
        let forms = context.ios >= 27 ? [185, 185, 221] : [185, 185]
        let released = try await Self.releasedInFlight(gpu, set, forms)
        let settled = try gpu.buffer(16)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: settled, range: 0 ..< 16, value: 0)
            blit.endEncoding()
        }
        let after = GM20BFacade.memory(of: gpu.device)
        var metrics = Metrics()
        metrics.set("decode_view_outlives_off", outlives)
        metrics.set("decode_view_release_failed", gpu.commandBufferErrors.count)
        metrics.set("decode_view_release_off", released)
        metrics.set("decode_memory_left", (before["headers_left"] ?? 0) - (after["headers_left"] ?? 0))
        metrics.set("decode_pool_delta_kib", ((after["pool"] ?? 0) - (before["pool"] ?? 0)) >> 10)
        metrics.set("decode_wc_delta_kib", ((after["wc"] ?? 0) - (before["wc"] ?? 0)) >> 10)
        let left = (before["headers_left"] ?? 0) - (after["headers_left"] ?? 0)
        var checks = [
            ("\(outlives) texels of a view whose texture went off its decode", outlives == 0),
            ("\(released) texels of the texture off its last blocks", released == 0),
            ("\(gpu.commandBufferErrors.count) command buffers failed", gpu.commandBufferErrors.isEmpty),
            ("\(left) texture headers still held", left <= 0),
        ]
        if let textures = after["textures"] {
            let bytesLeft = textures - (before["textures"] ?? 0)
            metrics.set("decode_texture_bytes_left", bytesLeft)
            checks.append(("\(bytesLeft) bytes of images and kept blocks still held", bytesLeft <= 0))
        } else if !after.isEmpty {
            checks.append((GM20BFacade.missing("textures", on: gpu.device), false))
        }
        return .judged(checks, metrics, proof: gpu.proof).onRawPath(gpu)
    }

    /// The 185 view's texels off a 185 texture of the rotated blocks, after its texture went and they were uploaded
    /// through it
    static func outliving(_ gpu: GPU, _ set: DecodeVectors.Astc) async throws -> Int {
        var texture = try await DecodeScene.astcRow(gpu, set)
        guard let view = texture?.makeTextureView(pixelFormat: MTLPixelFormat(rawValue: 185)!) else {
            throw TestFailure("no 203 texture or 185 view")
        }
        texture = nil
        try await DecodeScene.upload(
            gpu, set.bytes(rotated: 1), rowBytes: set.blocks.count * 16, into: view, width: set.width,
            height: set.blockH
        )
        let at = set.points.map(\.at)
        let read = try await DecodeScene.read(gpu, view, at)
        let want = try await DecodeViewsTest.reference(gpu, set, 185, rotated: 1, at)
        return DecodeScene.differing(read, want)
    }

    /// The texture's texels off its decode of the last blocks, after each view of `forms` went while an upload into the
    /// texture was in flight
    static func releasedInFlight(_ gpu: GPU, _ set: DecodeVectors.Astc, _ forms: [Int]) async throws -> Int {
        guard let texture = try await DecodeScene.astcRow(gpu, set) else { throw TestFailure("no 203 texture") }
        var views: [MTLTexture?] = forms.map {
            texture.makeTextureView(pixelFormat: MTLPixelFormat(rawValue: UInt($0))!)
        }
        guard views.allSatisfy({ $0 != nil }) else { throw TestFailure("a view of \(forms) not made") }
        var buffers: [MTLCommandBuffer] = []
        for index in views.indices {
            let staging = try gpu.buffer(set.bytes(rotated: index + 1))
            buffers.append(
                try gpu.commit { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.copy(
                        from: staging, sourceOffset: 0, sourceBytesPerRow: set.blocks.count * 16,
                        sourceBytesPerImage: 0,
                        sourceSize: MTLSize(width: set.width, height: set.blockH, depth: 1), to: texture,
                        destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin()
                    )
                    blit.endEncoding()
                }
            )
            views[index] = nil
        }
        // a failed one is counted by the caller (commandBufferErrors), with the texels it left
        try? gpu.finish(buffers)
        let at = set.points.map(\.at)
        let read = try await DecodeScene.read(gpu, texture, at)
        let want = try await DecodeViewsTest.reference(gpu, set, 203, rotated: views.count, at)
        return DecodeScene.differing(read, want)
    }
}

/// What a decode on upload costs, once an upload and never in a frame (decoded.h): replaceRegion of a 2048x2048 166
/// level (2 MiB of PVRTC blocks, any words), of a 2048x2048 222 level (4 MiB, 24B119), of a 1024x512 209 level, and of
/// that 209 level again with a 191 and (24B119) a 227 view alive, every live form decoded in the upload's own job (the
/// ASTC levels the driver's vectors' blocks, tiled: every block mode they exercise, none an error block alone); each
/// followed by a command buffer that samples the texture once, waited for. Reported: the time from the call to that
/// command buffer's completion (`decode_ms_pvrtc_2048`, `decode_ms_hdr_2048`, `decode_ms_8x4_1024`,
/// `decode_ms_8x4_1024_three_forms`; no threshold: none was measured) and the pool's chunk growths meanwhile
/// (`decode_pool_grows`). Judged: every texture made, no command buffer failed, and no command ring outgrown by the
/// uploads (`decode_rings_outgrown` 0, switchMetalCounters rings_outgrown: an upload's decode is one job's few draws).
struct DecodeUploadCostTest: HWTest {
    let id = "gpu.decode.upload_cost"
    let name = "Decode on upload: large uploads' time to a sampling command buffer's completion"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let hdr = context.ios >= 27
        let before = GM20BFacade.memory(of: gpu.device)
        let ringsBefore = GM20BFacade.counters(of: gpu.device)["rings_outgrown"]
        var metrics = Metrics()
        var unmade: [String] = []
        func timed(
            _ key: String, _ format: Int, _ width: Int, _ height: Int, blockW: Int, bytes: Int, forms: [Int] = []
        ) async throws {
            let pixelFormat = MTLPixelFormat(rawValue: UInt(format))!
            guard let texture = DecodeScene.texture(gpu, pixelFormat, width, height, storage: .shared) else {
                unmade.append(key)
                return
            }
            let views = forms.compactMap { texture.makeTextureView(pixelFormat: MTLPixelFormat(rawValue: UInt($0))!) }
            if views.count != forms.count { unmade.append(key + "_views") }
            let pvrtc = format >= 160 && format <= 167
            let vectors = pvrtc ? [] : DecodeVectors.set(format).bytes()
            guard pvrtc || !vectors.isEmpty else {
                unmade.append(key + "_vectors")
                return
            }
            let blocks =
                pvrtc
                    ? DecodeVectors.bytes(DecodeReadbackTest.words(bytes / 4, seed: UInt32(format)))
                    : (0 ..< bytes).map { vectors[$0 % vectors.count] }
            let rowBytes = pvrtc ? 0 : width / blockW * 16
            let start = CFAbsoluteTimeGetCurrent()
            blocks.withUnsafeBytes {
                texture.replace(
                    region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: $0.baseAddress!,
                    bytesPerRow: rowBytes
                )
            }
            try await Self.sampleOnce(gpu, texture)
            metrics.set(key, (CFAbsoluteTimeGetCurrent() - start) * 1000, digits: 2)
            withExtendedLifetime(views) {}
        }
        try await timed("decode_ms_pvrtc_2048", 166, 2048, 2048, blockW: 4, bytes: 2048 * 2048 / 2)
        if hdr {
            try await timed("decode_ms_hdr_2048", 222, 2048, 2048, blockW: 4, bytes: 2048 * 2048)
        }
        try await timed("decode_ms_8x4_1024", 209, 1024, 512, blockW: 8, bytes: 1024 * 512 / 2)
        try await timed(
            "decode_ms_8x4_1024_three_forms", 209, 1024, 512, blockW: 8, bytes: 1024 * 512 / 2,
            forms: hdr ? [191, 227] : [191]
        )
        let after = GM20BFacade.memory(of: gpu.device)
        metrics.set("decode_pool_grows", (after["grows"] ?? 0) - (before["grows"] ?? 0))
        metrics.set("unmade", unmade.isEmpty ? "-" : unmade.joined(separator: ","))
        var checks = [
            ("textures or views not made: \(unmade)", unmade.isEmpty),
            ("\(gpu.commandBufferErrors.count) command buffers failed", gpu.commandBufferErrors.isEmpty),
        ]
        if let rings = GM20BFacade.counters(of: gpu.device)["rings_outgrown"], let ringsBefore {
            metrics.set("decode_rings_outgrown", rings - ringsBefore)
            checks.append(("\(rings - ringsBefore) command rings outgrown", rings == ringsBefore))
        } else if !before.isEmpty {
            checks.append((GM20BFacade.missing("rings_outgrown", on: gpu.device), false))
        }
        return .judged(checks, metrics, proof: gpu.proof).onRawPath(gpu)
    }

    static func sampleOnce(_ gpu: GPU, _ texture: MTLTexture) async throws {
        let state = try gpu.compute("decode_sample_once")
        let out = try gpu.buffer(16)
        try await gpu.run { commands in
            let e = commands.makeComputeCommandEncoder()!
            e.setComputePipelineState(state)
            e.setTexture(texture, index: 0)
            e.setBuffer(out, offset: 0, index: 0)
            e.dispatchThreadgroups(
                MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1)
            )
            e.endEncoding()
        }
    }
}
