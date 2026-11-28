import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

extension PlainRows {
    /// A store scene's plane bytes after its work: the facade path and the bytes from the plane's start to the end of
    /// the allocation's last page; nil bytes where no texture is made
    public struct Stored: Sendable {
        public var path = "-"
        public var bytes: [UInt8]?
    }

    /// One pass of `s`'s codes over its whole frame into `texture`: `load` its colour's load action, `scissor` its
    /// scissor; the facade path
    static func pass(
        _ gpu: GPU, _ texture: MTLTexture, _ s: Shape, load: MTLLoadAction, scissor: MTLScissorRect? = nil
    ) async throws -> String {
        let unorm = s.format == MTLPixelFormat.r8Unorm.rawValue
        let pipeline = try gpu.render(
            fragment: unorm ? "plain_rows_code_unorm" : "plain_rows_code_words", format: texture.pixelFormat
        )
        var shape = SIMD4<UInt32>(UInt32(s.bpp), UInt32(s.width), UInt32(s.height), UInt32(s.turn))
        return try await gpu.run(tolerateError: true) { commands in
            let descriptor = MTLRenderPassDescriptor()
            descriptor.colorAttachments[0].texture = texture
            descriptor.colorAttachments[0].loadAction = load
            descriptor.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
            if let scissor { encoder.setScissorRect(scissor) }
            var corners = GPU.fullScreen
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBytes(&shape, length: 16, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }.facadePath
    }

    /// `s` over a fresh surface of 0xee, its codes stored by a pass (`scissor` given: a loaded pass scissored so) or,
    /// with `blit`, by a copy from a buffer of them
    public static func stored(
        _ gpu: GPU, _ s: Shape, scissor: MTLScissorRect? = nil, blit: Bool = false
    ) async throws -> Stored {
        guard let surface = surface(s, fill: { _ in 0xee }) else { throw TestFailure("no '420v' of rows \(s.pitch)") }
        guard let texture = texture(gpu, s, over: surface) else { return Stored() }
        var out = Stored()
        if blit {
            let codes = (0 ..< s.width * s.height).flatMap { code(s.bpp, $0 % s.width, $0 / s.width) }
            let source = try gpu.buffer(codes)
            out.path = try await RowsUnder16.blitPath(gpu) {
                $0.copy(
                    from: source, sourceOffset: 0, sourceBytesPerRow: s.width * s.bpp, sourceBytesPerImage: codes.count,
                    sourceSize: MTLSize(width: s.width, height: s.height, depth: 1), to: texture, destinationSlice: 0,
                    destinationLevel: 0, destinationOrigin: MTLOrigin()
                )
            }
        } else {
            out.path = try await pass(gpu, texture, s, load: scissor == nil ? .dontCare : .load, scissor: scissor)
        }
        out.bytes = mappedBytes(surface)
        return out
    }

    /// For each `s.bpp`-byte element from the plane's start to its span's end, the row whose code it holds: -1 none of
    /// its rows' (left as it was, or torn), -2 one no row reaches
    public static func holders(_ s: Shape, _ bytes: [UInt8]) -> [Int] {
        Swift.stride(from: 0, to: s.span, by: s.bpp).map { a in
            let rows = s.rows(reaching: a)
            guard !rows.isEmpty else { return -2 }
            return rows.last { Array(bytes[a ..< (a + s.bpp)]) == code(s.bpp, (a - s.stride * $0) / s.bpp, $0) } ?? -1
        }
    }

    /// Where the store of plane texel (x, y) of an unturned pass lands, later higher: 64x32 tiles from the last tile
    /// row up, each row from its last tile back; in a tile the right 32-texel half later, then the 2x2 quad's row, its
    /// column, the even row of a quad after the odd (overlap_order.h overlapOrder, an M4 Max's order)
    public static func overlapOrder(_ x: Int, _ y: Int) -> Int {
        let inTile = (x % 64 / 32) << 9 | (y % 32 / 2) << 5 | (x % 32 / 2) << 1 | (y % 2 == 0 ? 1 : 0)
        return (0x3ff - y / 32) << 18 | (0xff - x / 64) << 10 | inTile
    }

    /// Element `a`'s bytes after an unturned pass of codes over the whole frame, all of it drawn or (`scissor`) loaded
    /// and scissored: of its rows whose texel lies in a 32x32 tile the scissor reaches, the latest by overlapOrder
    /// stores its code inside the scissor and its loaded bytes (the surface's 0xee) outside; none: the element keeps
    /// them
    public static func passed(_ s: Shape, _ a: Int, scissor: MTLScissorRect?) -> [UInt8] {
        let kept = [UInt8](repeating: 0xee, count: s.bpp)
        let reached = s.rows(reaching: a).filter { r in
            guard let c = scissor else { return true }
            let x = (a - s.stride * r) / s.bpp
            return (c.x / 32 ... (c.x + c.width - 1) / 32).contains(x / 32)
                && (c.y / 32 ... (c.y + c.height - 1) / 32).contains(r / 32)
        }
        let order = { (r: Int) in overlapOrder((a - s.stride * r) / s.bpp, r) }
        guard let r = reached.max(by: { order($0) < order($1) }) else { return kept }
        let x = (a - s.stride * r) / s.bpp
        if let c = scissor, !(c.x ..< (c.x + c.width)).contains(x) || !(c.y ..< (c.y + c.height)).contains(r) {
            return kept
        }
        return code(s.bpp, x, r)
    }

    /// Elements of `stored`'s span off `want(a)` (nil: not judged), the bytes past the span that changed, the first off
    public static func judged(
        _ s: Shape, _ stored: Stored, _ want: (Int) -> [UInt8]?
    ) -> Reading {
        var r = Reading()
        guard let bytes = stored.bytes else { return r }
        r.made = true
        r.path = stored.path
        r.off = 0
        for a in Swift.stride(from: 0, to: s.span, by: s.bpp) {
            guard let w = want(a) else { continue }
            r.judged += 1
            if Array(bytes[a ..< (a + s.bpp)]) != w {
                if r.off == 0 { r.first = "element_\(a / s.bpp)_got_\(bytes[a])_want_\(w[0])" }
                r.off += 1
            }
        }
        let past = bytes[s.span...].filter { $0 != 0xee }.count
        if past > 0 && r.off == 0 { r.first = "\(past)_bytes_past_the_span" }
        r.off += past
        return r
    }

    public struct Under16: Sendable {
        public var made = false
        public var readPath = "-", passPath = "-"
        /// Texels a kernel read off row y's at y MiB, getBytes texels off the plane's rows (y * 8), plane bytes the
        /// pass left off rows 16 apart (in the first 16 KiB and at each mebibyte row)
        public var readOff = -1, getBytesOff = -1, storeOff = -1
        public var first = "-"
    }

    /// R8Unorm 8x3 over a '420v' plane 0 of rows 8 in 2.5 MiB: over every byte seeded, a kernel reads row y at y MiB +
    /// x and getBytes row y at y * 8 + x; over 0xee, a pass of codes stores row y at y * 16 + x and nothing else
    public static func rowsUnder16(_ gpu: GPU) async throws -> Under16 {
        let s = Shape(MTLPixelFormat.r8Unorm.rawValue, 8, 3, bpp: 1, pitch: 8)
        let mebibyte = RowsUnder16.mebibyte, allocation = 0x28_0000
        var u = Under16()
        guard let seededSurface = surface(s, allocation: allocation, fill: seeded) else {
            throw TestFailure("no '420v' of rows 8 in 2.5 MiB")
        }
        guard let reader = texture(gpu, s, over: seededSurface) else { return u }
        u.made = true
        let (path, got) = try await read(gpu, reader)
        u.readPath = path
        var r = Reading()
        judged(&r, got) { SIMD4(Float(seeded($0 / 8 * mebibyte + $0 % 8)) / 255, 0, 0, 1) }
        (u.readOff, u.first) = (r.off, r.first)
        var rows = [UInt8](repeating: 0, count: 24)
        reader.getBytes(&rows, bytesPerRow: 8, from: MTLRegionMake2D(0, 0, 8, 3), mipmapLevel: 0)
        u.getBytesOff = rows.indices.filter { rows[$0] != seeded($0) }.count
        guard let blank = surface(s, allocation: allocation, fill: { _ in 0xee }),
              let target = texture(gpu, s, over: blank)
        else { return u }
        u.passPath = try await pass(gpu, target, s, load: .dontCare)
        let bytes = mappedBytes(blank)
        let judgedBytes = Array(0 ..< 16384) + (1 ... 2).flatMap { m in (m * mebibyte) ..< (m * mebibyte + 16) }
        u.storeOff = judgedBytes.filter { a in
            let (y, x) = (a / 16, a % 16)
            return bytes[a] != (y < 3 && x < 8 ? code(1, x, y)[0] : 0xee)
        }.count
        return u
    }
}
