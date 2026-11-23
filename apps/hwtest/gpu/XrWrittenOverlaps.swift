import Foundation
import HWTestCore
import IOSurface
import Metal

/// Written RGB10A8_2P_XR10 (550) textures whose word rows run where a plane's do not: into the alpha plane after them,
/// or past the surface's last page; each surface's byte i SurfaceReads.pattern(i) first
public enum XrWrittenOverlaps {
    typealias P = SurfaceReads.Plane

    static let cleared = SIMD4<Float>(0.5, 0.25, -0.25, 0.75)
    static let clearedWord: UInt32 = 0x27f8_0100
    static let clearedAlpha: UInt8 = 0xbf

    public struct IntoAlpha: Sendable {
        public var made = false
        public var status = "-"
        /// Texels a kernel read off the planes' bytes; bytes of the clear off an M4 Max's but the tie byte; bytes past
        /// both planes changed; the tie byte after the clear
        public var readOff = -1
        public var clearOff = -1
        public var tailChanged = -1
        public var tie = -1
    }

    /// The byte both a word and an alpha byte of one texel column store to (word row 5's byte 42, alpha texel (10, 5)),
    /// which an M4 Max's clear leaves as the OR of the two (0xf8 | 0xbf), three runs alike
    public static let tieByte = 682

    /// gpu.gm20b.xr_words_into_alpha: a 550 32x8, ShaderRead, ShaderWrite and RenderTarget, over plane 0 of a 'b3a8' 32x4
    /// (words rows 128 at 0, alpha rows 32 at 512, 1024 bytes): its word rows 4 to 7 are plane 1's bytes and the bytes
    /// after it. Read by a kernel, then cleared to (0.5, 0.25, -0.25, 0.75) by a pass
    public static func intoAlpha(_ gpu: GPU) async throws -> IntoAlpha {
        guard
            let surface = SurfaceReads.surface(
                "b3a8", 32, 4, planes: [P(at: 0, 32, 4, bpe: 4, rows: 128), P(at: 512, 32, 4, bpe: 1, rows: 32)],
                allocation: 1024
            )
        else { throw TestFailure("no 'b3a8' 32x4") }
        var result = IntoAlpha()
        guard
            let texture = SurfaceReads.texture(
                gpu, 550, 32, 8, over: surface, usage: [.shaderRead, .shaderWrite, .renderTarget]
            )
        else { return result }
        result.made = true
        let start = SurfaceReads.bytes(surface)
        let got = try await SurfaceReads.read(gpu, texture)
        result.readOff = SurfaceReads.off(got) { i in
            let (x, y) = (i % 32, i / 32)
            let alpha = Float(start[512 + y * 32 + x]) / 255
            return SurfaceReads.xr(SurfaceReads.word(start, y * 128 + 4 * x), alpha: alpha)
        }
        result.status = try await clear(gpu, texture)
        guard !result.status.hasPrefix("error:") else { return result }
        let after = SurfaceReads.bytes(surface)
        let want = intoAlphaCleared()
        result.clearOff = (0 ..< 1024).filter { $0 != tieByte && after[$0] != want[$0] }.count
        result.tie = Int(after[tieByte])
        result.tailChanged = (1024 ..< after.count).filter { after[$0] != start[$0] }.count
        return result
    }

    /// An M4 Max's bytes after the clear, three runs alike: words everywhere but alpha rows 4 to 7 (bytes 640 to 767),
    /// which hold alpha but row 5's first 10 bytes, which hold the words' bytes, and its 11th, the tie byte
    public static func intoAlphaCleared() -> [UInt8] {
        (0 ..< 1024).map { o in
            let word = UInt8(truncatingIfNeeded: clearedWord >> (8 * UInt32(o % 4)))
            guard (640 ..< 768).contains(o) else { return word }
            if o == tieByte { return word | clearedAlpha }
            let (row, column) = ((o - 512) / 32, (o - 512) % 32)
            return row == 5 && column < 10 ? word : clearedAlpha
        }
    }

    static func clear(_ gpu: GPU, _ texture: MTLTexture) async throws -> String {
        try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(cleared.x), green: Double(cleared.y), blue: Double(cleared.z), alpha: Double(cleared.w)
            )
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }.path
    }

    public struct AlonePastLastPage: Sendable {
        public var made = false
        public var status = "-"
        /// Texels a kernel read off the words inside the page (0 past it, alpha 0); after a kernel's store of (0.5,
        /// 0.25, -0.25, 0.75), plane bytes inside the page not its word and bytes before the plane changed
        public var readOff = -1
        public var storeOff = -1
    }

    public static let alonePage = 16384, aloneOffset = 16384 - 256

    /// gpu.gm20b.xr_alone_past_last_page: a 550 32x8, ShaderRead and ShaderWrite, over a 'w30r' of one 16 KiB page whose
    /// one plane (32x4 two-byte elements of rows 64, which IOSurfaceCreate makes where it makes no four-byte one)
    /// starts 256 bytes before its end: the texture's rows overlap (128 bytes of words a row) and rows 3 to 7 run past
    /// the page. Read by a kernel, then written by one
    public static func alonePastLastPage(_ gpu: GPU) async throws -> AlonePastLastPage {
        guard
            let surface = SurfaceReads.surface(
                "w30r", 32, 8, planes: [P(at: aloneOffset, 32, 4, bpe: 2, rows: 64)], allocation: alonePage
            )
        else { throw TestFailure("no 'w30r' of a plane 256 bytes before its page's end") }
        var result = AlonePastLastPage()
        guard let texture = SurfaceReads.texture(gpu, 550, 32, 8, over: surface, usage: [.shaderRead, .shaderWrite])
        else { return result }
        result.made = true
        let start = SurfaceReads.bytes(surface)
        let got = try await SurfaceReads.read(gpu, texture)
        result.readOff = SurfaceReads.off(got) { i in
            let at = aloneOffset + (i / 32) * 64 + 4 * (i % 32)
            return SurfaceReads.xr(at + 4 <= alonePage ? SurfaceReads.word(start, at) : 0, alpha: 0)
        }
        result.status = try await SurfaceReads.fill(gpu, texture, cleared)
        guard !result.status.hasPrefix("error:") else { return result }
        let after = SurfaceReads.bytes(surface)
        let before = (0 ..< aloneOffset).filter { after[$0] != start[$0] }.count
        let words = stride(from: aloneOffset, to: alonePage, by: 4).filter {
            SurfaceReads.word(after, $0) != clearedWord
        }
        result.storeOff = before + words.count
        return result
    }
}
