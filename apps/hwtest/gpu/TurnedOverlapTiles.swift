import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Turned targets over planes whose rows overlap or run into the next plane, each plane declared as wide as its rows
/// hold (IOSurfaceCreate makes no plane wider than its bytesPerRow), every byte 0xee first. An M4 Max stores such a
/// target by 32x32 tiles of its FRAME; inside one tile the later frame row (or the word over the alpha) keeps a byte
/// several rows reach, while between tiles the order is a race, so only bytes whose writers lie in one frame tile are
/// judged. Expected bytes come from the same pass into a control surface of rows that do not overlap.
public enum TurnedOverlapTiles {
    public struct Reading: Sendable {
        public var made = false
        public var path = "-"
        /// Judged bytes off Apple's, and of those the ones whose other candidate holds a different value (the bytes
        /// that tell the two orders apart)
        public var off = -1
        public var telling = 0
        /// The plane's declared width against the turned frame's extent across it
        public var planeWidth = 0, frameExtent = 0
    }

    struct Plane {
        let offset, width, height, bpe, pitch: Int
    }

    static func surface(
        _ fourcc: String, _ width: Int, _ height: Int, _ planes: [Plane], allocation: Int
    ) -> IOSurfaceRef? {
        let info = planes.map { p -> [CFString: Any] in
            [
                kIOSurfacePlaneWidth: p.width, kIOSurfacePlaneHeight: p.height, kIOSurfacePlaneBytesPerElement: p.bpe,
                kIOSurfacePlaneBytesPerRow: p.pitch, kIOSurfacePlaneOffset: p.offset,
                kIOSurfacePlaneSize: p.pitch * p.height,
            ]
        }
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: width, kIOSurfaceHeight: height, kIOSurfaceAllocSize: allocation,
            kIOSurfacePixelFormat: fourcc.utf8.reduce(UInt32(0)) { $0 << 8 | UInt32($1) }, kIOSurfacePlaneInfo: info,
        ]
        guard let s = IOSurfaceCreate(properties as CFDictionary) else { return nil }
        RowsUnder16.edit(s) { memset($0, 0xee, allocation) }
        return s
    }

    /// A `format` frame `w` x `h` turned by `rotation` over plane 0: cleared, then drawn whole by turned_order_colours
    /// unless `clearOnly`; nil where none is made
    static func pass(_ gpu: GPU, _ s: IOSurfaceRef, format: UInt, _ w: Int, _ h: Int, rotation: Int, clearOnly: Bool)
        async throws -> String?
    {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: format)!, width: w, height: h, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else {
            throw TestFailure("MTLTextureDescriptor answers no setRotation:")
        }
        descriptor.setValue(NSNumber(value: rotation), forKey: "rotation")
        descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: s, plane: 0) else { return nil }
        let clear = MTLClearColor(red: 0.5, green: 0.25, blue: -0.25, alpha: 0.75)
        let fragment = clearOnly ? nil : "turned_order_colours"
        return try await RowsUnder16.pass(gpu, texture, clear: clear, fragment: fragment)
    }

    /// `odd`'s bytes at `judged` against `want(i)`, and how many of them `other(i)` would miss
    static func judge(_ odd: [UInt8], _ judged: [Int], want: (Int) -> UInt8, other: (Int) -> UInt8) -> (Int, Int) {
        (judged.filter { odd[$0] != want($0) }.count, judged.filter { want($0) != other($0) }.count)
    }

    /// Spec scene B: a 500 frame 96x128 turned by 3 over a '420v' of luma 128x96 and chroma 64x48 pairs, both rows 16
    /// apart (chroma at 16384): chroma bytes 16400-16415 hold chroma row 0's bytes 16-31 (not row 1's 0-15) and bytes
    /// 17232-17247 row 46's bytes 112-127 (not row 47's 96-111), each as the control of rows 128 stores those rows
    public static func chromaRows(_ gpu: GPU) async throws -> Reading {
        let odd = surface(
            "420v", 128, 96, [Plane(offset: 0, width: 16, height: 96, bpe: 1, pitch: 16),
                              Plane(offset: 16384, width: 8, height: 48, bpe: 2, pitch: 16)], allocation: 32768
        )
        let control = surface(
            "420v", 128, 96, [Plane(offset: 0, width: 128, height: 96, bpe: 1, pitch: 128),
                              Plane(offset: 16384, width: 64, height: 48, bpe: 2, pitch: 128)], allocation: 32768
        )
        guard let odd, let control else { throw TestFailure("no '420v' 128x96 of rows 16 or 128") }
        var r = Reading(planeWidth: IOSurfaceGetWidthOfPlane(odd, 0), frameExtent: 128)
        guard try await pass(gpu, control, format: 500, 96, 128, rotation: 3, clearOnly: false) != nil else {
            throw TestFailure("no turned 500 over the control")
        }
        guard let path = try await pass(gpu, odd, format: 500, 96, 128, rotation: 3, clearOnly: false) else { return r }
        let (o, c) = (SurfaceReads.bytes(odd), SurfaceReads.bytes(control))
        let row = { (k: Int, b: Int) in c[16384 + 128 * k + b] }
        let judged = Array(16400 ..< 16416) + Array(17232 ..< 17248)
        (r.off, r.telling) = judge(
            o, judged, want: { $0 < 16416 ? row(0, $0 - 16400 + 16) : row(46, $0 - 17232 + 112) },
            other: { $0 < 16416 ? row(1, $0 - 16400) : row(47, $0 - 17232 + 96) }
        )
        (r.made, r.path) = (true, path)
        return r
    }

    /// Spec scene C: a 550 frame 72x96 turned by 1 over a 'b3a8' of words 96x72 rows 96 apart at 0 (a row 384 bytes:
    /// they overlap and run into the alpha plane at 6912, rows 96): bytes 96-127 hold word row 1's words 0-7 (not row
    /// 0's words 24-31), as the control of word rows 384 stores those words
    public static func wordRows(_ gpu: GPU) async throws -> Reading {
        let odd = surface(
            "b3a8", 96, 72, [Plane(offset: 0, width: 24, height: 72, bpe: 4, pitch: 96),
                             Plane(offset: 6912, width: 96, height: 72, bpe: 1, pitch: 96)], allocation: 16384
        )
        let control = surface(
            "b3a8", 96, 72, [Plane(offset: 0, width: 96, height: 72, bpe: 4, pitch: 384),
                             Plane(offset: 27648, width: 96, height: 72, bpe: 1, pitch: 96)], allocation: 49152
        )
        guard let odd, let control else { throw TestFailure("no 'b3a8' 96x72 of word rows 96 or 384") }
        var r = Reading(planeWidth: IOSurfaceGetWidthOfPlane(odd, 0), frameExtent: 96)
        guard try await pass(gpu, control, format: 550, 72, 96, rotation: 1, clearOnly: false) != nil else {
            throw TestFailure("no turned 550 over the control")
        }
        guard let path = try await pass(gpu, odd, format: 550, 72, 96, rotation: 1, clearOnly: false) else { return r }
        let (o, c) = (SurfaceReads.bytes(odd), SurfaceReads.bytes(control))
        (r.off, r.telling) = judge(o, Array(96 ..< 128), want: { c[384 + $0 - 96] }, other: { c[$0] })
        (r.made, r.path) = (true, path)
        return r
    }

    /// Spec 5: a 550 frame 8x96 turned by 1 over a 'b3a8' of words 96x8 rows 192 apart at 0 (rows 384 bytes long run
    /// into the alpha plane at 1536, rows 96), only cleared to (0.5, 0.25, -0.25, 0.75): bytes 1568-1597 and 1696-1706
    /// hold word row 7's bytes of 0x27f80100, not the alpha byte 0xbf
    public static func clearOrder(_ gpu: GPU) async throws -> Reading {
        let odd = surface(
            "b3a8", 96, 8, [Plane(offset: 0, width: 48, height: 8, bpe: 4, pitch: 192),
                            Plane(offset: 1536, width: 96, height: 8, bpe: 1, pitch: 96)], allocation: 16384
        )
        guard let odd else { throw TestFailure("no 'b3a8' 96x8 of word rows 192") }
        var r = Reading(planeWidth: IOSurfaceGetWidthOfPlane(odd, 0), frameExtent: 96)
        guard let path = try await pass(gpu, odd, format: 550, 8, 96, rotation: 1, clearOnly: true) else { return r }
        let o = SurfaceReads.bytes(odd)
        let word = { (i: Int) in UInt8(truncatingIfNeeded: UInt32(0x27f8_0100) >> (8 * ((i - 1344) % 4))) }
        (r.off, r.telling) = judge(o, Array(1568 ..< 1598) + Array(1696 ..< 1707), want: word, other: { _ in 0xbf })
        (r.made, r.path) = (true, path)
        return r
    }
}
