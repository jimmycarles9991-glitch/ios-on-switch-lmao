import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// YCBCR8_420_2P (500) 8x8 targets over '420v' planes of rows under 16 bytes in 8 MiB + 16 KiB (luma at 0, chroma at
/// 4096; gpu.gm20b.ycbcr_rows_under_16_stores), luma and chroma rows 8 and 8 (b), 8 and 16 (p0), 16 and 8 (p1). An M4 Max
/// reads such a plane's rows a mebibyte apart and stores them 16 bytes apart from its offset (rows of 16 and more at
/// their own rows), so a pass leaves the first 16 KiB as the same pass leaves a control target of rows 16 and 16 whose
/// rows hold what the odd one reads; a blit from a buffer lands there too, one into a buffer reads the mebibyte rows,
/// and getBytes and replaceRegion use the stated rows.
public enum YcbcrRowsUnder16Stores {
    typealias P = SurfaceReads.Plane
    static let mebibyte = RowsUnder16.mebibyte
    static let fill: UInt8 = 0xa5
    public static let clear = MTLClearColor(red: 0.8, green: 0.4, blue: 0.2, alpha: 1)

    public static let shapes: [(key: String, luma: Int, chroma: Int)] = [("b", 8, 8), ("p0", 8, 16), ("p1", 16, 8)]

    public struct Reading: Sendable {
        /// Bytes off the control's after a clear pass and after a whole-target draw, by shape
        public var clearOff: [String: Int] = [:]
        public var drawOff: [String: Int] = [:]
        /// Bytes off after a loaded pass whose one draw discards every fragment (the read rows stored at the store
        /// rows), and after the same pass without its draw (nothing stored)
        public var loadMovedOff = -1
        public var loadNoDrawOff = -1
        /// Bytes off for a blit from a buffer, a blit into a buffer, getBytes and replaceRegion
        public var blitOff: [String: Int] = [:]
        public var made = true
        public var failures: [String] = []
    }

    public static func target(_ gpu: GPU, luma: Int = 8, chroma: Int = 8) -> (IOSurfaceRef, MTLTexture)? {
        let planes = [P(at: 0, 8, 8, bpe: 1, rows: luma), P(at: 4096, 4, 4, bpe: 2, rows: chroma)]
        let usage: MTLTextureUsage = [.shaderRead, .shaderWrite, .renderTarget]
        guard let surface = RowsUnder16.surface("420v", 8, 8, planes: planes, fill: fill),
              let texture = SurfaceReads.texture(gpu, 500, 8, 8, over: surface, usage: usage)
        else { return nil }
        return (surface, texture)
    }

    static func head(_ surface: IOSurfaceRef) -> [UInt8] { RowsUnder16.bytes(surface, at: Array(0 ..< 16384)) }

    public static func observe(_ gpu: GPU) async throws -> Reading {
        var r = Reading()
        for draw in [false, true] {
            let fragment = draw ? "rows16_ycbcr_colours" : nil
            guard let (control, controlTarget) = target(gpu, luma: 16, chroma: 16) else {
                r.made = false
                return r
            }
            let path = try await RowsUnder16.pass(gpu, controlTarget, clear: clear, fragment: fragment)
            if path.hasPrefix("error") { r.failures.append("control \(draw ? "draw" : "clear") \(path)") }
            let want = head(control)
            for shape in shapes {
                guard let (surface, texture) = target(gpu, luma: shape.luma, chroma: shape.chroma) else {
                    r.made = false
                    return r
                }
                let path = try await RowsUnder16.pass(gpu, texture, clear: clear, fragment: fragment)
                if path.hasPrefix("error") { r.failures.append("\(shape.key) \(draw ? "draw" : "clear") \(path)") }
                let off = RowsUnder16.off(surface, want, fill: fill)
                if draw { r.drawOff[shape.key] = off } else { r.clearOff[shape.key] = off }
            }
        }
        for discarding in [true, false] {
            guard let (surface, texture) = target(gpu), let (control, controlTarget) = target(gpu, luma: 16, chroma: 16)
            else { throw TestFailure("no 500 for the loaded pass") }
            let rows = readRows(surface)
            RowsUnder16.edit(control) { base in
                for (at, byte) in rows {
                    base[(at % mebibyte >= 4096 ? 4096 : 0) + 16 * (at / mebibyte) + at % 8] = byte
                }
            }
            let path = try await loaded(gpu, texture, discarding: discarding)
            if path.hasPrefix("error") { r.failures.append("load \(discarding ? "discarding" : "no draw") \(path)") }
            var want = [UInt8](repeating: fill, count: 16384)
            for (at, byte) in rows where at < 16384 { want[at] = byte }
            if discarding {
                let controlPath = try await loaded(gpu, controlTarget, discarding: true)
                if controlPath.hasPrefix("error") { r.failures.append("control load \(controlPath)") }
                want = head(control)
            }
            let off = RowsUnder16.off(surface, want, fill: fill, except: rows.filter { $0.key >= 16384 })
            if discarding { r.loadMovedOff = off } else { r.loadNoDrawOff = off }
        }
        r.blitOff = try await blits(gpu)
        return r
    }

    /// Luma read rows at y MiB and chroma read rows at 4096 + y MiB given distinct bytes; the bytes by offset
    public static func readRows(_ surface: IOSurfaceRef) -> [Int: UInt8] {
        var rows: [Int: UInt8] = [:]
        for y in 0 ..< 8 { for x in 0 ..< 8 { rows[y * mebibyte + x] = UInt8(truncatingIfNeeded: 20 + 8 * y + x) } }
        for y in 0 ..<
            4 { for b in 0 ..< 8 { rows[4096 + y * mebibyte + b] = UInt8(truncatingIfNeeded: 100 + 8 * y + b) } }
        RowsUnder16.edit(surface) { base in for (at, byte) in rows { base[at] = byte } }
        return rows
    }

    /// A pass that loads `texture` and stores it, with one draw that discards every fragment or none
    public static func loaded(_ gpu: GPU, _ texture: MTLTexture, discarding: Bool) async throws -> String {
        let fragment = discarding ? "rows16_discard" : nil
        return try await RowsUnder16.pass(gpu, texture, clear: clear, fragment: fragment, load: true)
    }

    static func blits(_ gpu: GPU) async throws -> [String: Int] {
        var off: [String: Int] = [:]
        let pattern = (0 ..< 64).map { UInt8(truncatingIfNeeded: 3 + 7 * $0) }
        let region = MTLRegionMake2D(0, 0, 8, 8)
        guard let (into, intoTexture) = target(gpu), let (from, fromTexture) = target(gpu),
              let (cpu, cpuTexture) = target(gpu), let (replaced, replacedTexture) = target(gpu)
        else { throw TestFailure("no 500 for the blits") }

        let source = try gpu.buffer(pattern)
        let up = try await RowsUnder16.blitPath(gpu) {
            $0.copy(
                from: source, sourceOffset: 0, sourceBytesPerRow: 8, sourceBytesPerImage: 64,
                sourceSize: MTLSize(width: 8, height: 8, depth: 1), to: intoTexture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
        }
        var landed = [UInt8](repeating: fill, count: 16384)
        for y in 0 ..< 8 { for x in 0 ..< 8 { landed[16 * y + x] = pattern[8 * y + x] } }
        off["from_buffer"] = up.hasPrefix("error") ? -1 : RowsUnder16.off(into, landed, fill: fill)

        let rows = readRows(from)
        let out = try gpu.buffer(64)
        let down = try await RowsUnder16.blitPath(gpu) {
            $0.copy(
                from: fromTexture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: 8, height: 8, depth: 1), to: out, destinationOffset: 0,
                destinationBytesPerRow: 8, destinationBytesPerImage: 64
            )
        }
        let got = [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: 64))
        let readOff = (0 ..< 64).filter { got[$0] != rows[($0 / 8) * mebibyte + $0 % 8] }.count
        off["to_buffer"] = down.hasPrefix("error") ? -1 : readOff

        RowsUnder16.edit(cpu) { base in for i in 0 ..< 64 { base[i] = pattern[i] } }
        var read = [UInt8](repeating: 0, count: 64)
        read.withUnsafeMutableBytes {
            cpuTexture.getBytes($0.baseAddress!, bytesPerRow: 8, from: region, mipmapLevel: 0)
        }
        off["get_bytes"] = zip(read, pattern).filter { $0 != $1 }.count

        pattern.withUnsafeBytes {
            replacedTexture.replace(region: region, mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 8)
        }
        var want = [UInt8](repeating: fill, count: 16384)
        for i in 0 ..< 64 { want[i] = pattern[i] }
        off["replace_region"] = RowsUnder16.off(replaced, want, fill: fill)
        return off
    }
}
