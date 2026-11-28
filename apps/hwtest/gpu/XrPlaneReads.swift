import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// RGB10A8_2P_XR10 (550) over planes laid out as Metal lays them for an M4 Max, each
/// surface's byte i SurfaceReads.pattern(i) first, read by a kernel; the counts are texels or bytes not the M4 Max's.
public enum XrPlaneReads {
    typealias P = SurfaceReads.Plane

    /// gpu.gm20b.xr_next_plane: a three-plane 'b3a8' 32x8 (rows 128 at 0, 1024, 2048): over plane p the words of p with the
    /// alpha bytes of p + 1 (texels_off), over the last plane alpha 0 (last_alpha_off); a kernel's (0.5, 0.25, -0.25,
    /// 0.75) into the 550 over plane 1 writes plane 1's words 0x27f80100 and plane 2's first 32 bytes a row 0xbf, and
    /// nothing else (store_bytes_off)
    public static func nextPlane(_ gpu: GPU) async throws -> (texelsOff: Int, lastAlphaOff: Int, storeBytesOff: Int) {
        let planes = (0 ..< 3).map { P(at: 1024 * $0, 32, 8, bpe: 4, rows: 128) }
        guard let surface = SurfaceReads.surface("b3a8", 32, 8, planes: planes, allocation: 3072) else {
            throw TestFailure("no three-plane 'b3a8'")
        }
        let start = SurfaceReads.bytes(surface)
        var result = (texelsOff: 0, lastAlphaOff: 0, storeBytesOff: 0)
        for p in 0 ..< 3 {
            guard let texture = SurfaceReads.texture(gpu, 550, 32, 8, over: surface, plane: p) else {
                throw TestFailure("no 550 over plane \(p)")
            }
            let got = try await SurfaceReads.read(gpu, texture)
            let off = SurfaceReads.off(got) { i in
                let (x, y) = (i % 32, i / 32)
                let alpha = p < 2 ? Float(start[1024 * (p + 1) + y * 128 + x]) / 255 : 0
                return SurfaceReads.xr(SurfaceReads.word(start, 1024 * p + y * 128 + 4 * x), alpha: alpha)
            }
            if p < 2 { result.texelsOff += off } else { result.lastAlphaOff = off }
        }
        guard let written = SurfaceReads.texture(
            gpu,
            550,
            32,
            8,
            over: surface,
            plane: 1,
            usage: [.shaderRead, .shaderWrite]
        )
        else { throw TestFailure("no writable 550 over plane 1") }
        let path = try await SurfaceReads.fill(gpu, written, SIMD4(0.5, 0.25, -0.25, 0.75))
        guard !path.hasPrefix("error:") else { throw TestFailure("the store failed: \(path)") }
        var want = start
        for y in 0 ..< 8 {
            for x in 0 ..< 32 {
                withUnsafeBytes(of: UInt32(0x27f8_0100).littleEndian) {
                    for k in 0 ..< 4 { want[1024 + y * 128 + 4 * x + k] = $0[k] } }
                want[2048 + y * 128 + x] = 0xbf
            }
        }
        let got = SurfaceReads.bytes(surface)
        result.storeBytesOff = got.indices.filter { got[$0] != want[$0] }.count
        return result
    }

    /// gpu.gm20b.xr_planes_off16: a 550 32x8 over plane 0 of a 'b3a8' of rows 136 (plane 1 rows 40 at 1088) and over a
    /// 'w30r' of one plane of rows 136 (Metal asserts on a planeless one of rows off 16 bytes): the GPU reads and blits the words at rows rounded down to 16 bytes (128), the alpha bytes at
    /// rows of 32, while getBytes reads and replaceRegion writes the rows of 136
    public static func off16(_ gpu: GPU, planar: Bool) async throws -> (gpuOff: Int, blitOff: Int, cpuOff: Int) {
        let surface = planar
            ? SurfaceReads.surface(
                "b3a8", 32, 8, planes: [P(at: 0, 32, 8, bpe: 4, rows: 136), P(at: 1088, 32, 8, bpe: 1, rows: 40)],
                allocation: 1536
            )
            : SurfaceReads.surface("w30r", 32, 8, planes: [P(at: 0, 32, 8, bpe: 4, rows: 136)], allocation: 1536)
        guard let surface, let texture = SurfaceReads.texture(gpu, 550, 32, 8, over: surface) else {
            throw TestFailure("no 550 over a surface of rows 136")
        }
        let start = SurfaceReads.bytes(surface)
        let got = try await SurfaceReads.read(gpu, texture)
        let gpuOff = SurfaceReads.off(got) { i in
            let (x, y) = (i % 32, i / 32)
            return SurfaceReads.xr(
                SurfaceReads.word(start, y * 128 + 4 * x), alpha: planar ? Float(start[1088 + y * 32 + x]) / 255 : 0
            )
        }
        let out = try gpu.buffer(32 * 8 * 4)
        let blit = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeBlitCommandEncoder()!
            encoder.copy(
                from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: 32, height: 8, depth: 1), to: out, destinationOffset: 0,
                destinationBytesPerRow: 128, destinationBytesPerImage: 1024
            )
            encoder.endEncoding()
        }
        guard !blit.path.hasPrefix("error:") else { throw TestFailure("the blit failed: \(blit.facadePath)") }
        let blitted = out.contents().bindMemory(to: UInt32.self, capacity: 256)
        let blitOff = (0 ..< 256).filter { blitted[$0] != SurfaceReads.word(start, ($0 / 32) * 128 + 4 * ($0 % 32)) }
            .count
        let region = MTLRegionMake2D(0, 0, 32, 8)
        var words = [UInt32](repeating: 0, count: 256)
        words
            .withUnsafeMutableBytes { texture.getBytes($0.baseAddress!, bytesPerRow: 128, from: region, mipmapLevel: 0)
            }
        var cpuOff = (0 ..< 256).filter { words[$0] != SurfaceReads.word(start, ($0 / 32) * 136 + 4 * ($0 % 32)) }.count
        let replaced = (0 ..< 256).map { UInt32(0x2abc_0000 | $0) }
        replaced.withUnsafeBytes { texture.replace(
            region: region,
            mipmapLevel: 0,
            withBytes: $0.baseAddress!,
            bytesPerRow: 128
        ) }
        let after = SurfaceReads.bytes(surface)
        cpuOff += (0 ..< 256).filter { SurfaceReads.word(after, ($0 / 32) * 136 + 4 * ($0 % 32)) != replaced[$0] }.count
        return (gpuOff, blitOff, cpuOff)
    }

    /// gpu.gm20b.xr_alpha_sink: a 550 64x64 over a 'w30r' (rows 256, every byte 0xff): a kernel's (0.5, 0.25, -0.25, 0.75)
    /// stores every word 0x27f80100 and nothing past the rows; read back, the colour with alpha 0
    public static func alphaSink(_ gpu: GPU) async throws
    -> (status: String, wordsOff: Int, pastChanged: Int, readOff: Int, alpha: Float) {
        guard let surface = SurfaceReads.surface(
            "w30r",
            64,
            64,
            rows: 256,
            allocation: 256 * 64 + 4096,
            fill: { _ in 0xff }
        ),
              let texture = SurfaceReads.texture(gpu, 550, 64, 64, over: surface, usage: [.shaderRead, .shaderWrite])
        else { throw TestFailure("no writable 550 over a 'w30r'") }
        let path = try await SurfaceReads.fill(gpu, texture, SIMD4(0.5, 0.25, -0.25, 0.75))
        guard !path.hasPrefix("error:") else { return (path, -1, -1, -1, -1) }
        let bytes = SurfaceReads.bytes(surface)
        let wordsOff = (0 ..< 4096).filter { SurfaceReads.word(bytes, 4 * $0) != 0x27f8_0100 }.count
        let pastChanged = (16384 ..< bytes.count).filter { bytes[$0] != 0xff }.count
        let got = try await SurfaceReads.read(gpu, texture)
        let readOff = SurfaceReads.off(got) { _ in SurfaceReads.xr(0x27f8_0100, alpha: 0) }
        return ("completed", wordsOff, pastChanged, readOff, got[0].w)
    }
}
