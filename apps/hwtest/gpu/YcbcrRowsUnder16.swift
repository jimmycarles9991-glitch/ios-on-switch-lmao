import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// YCbCr textures over planes of rows under 16 bytes (gpu.gm20b.ycbcr_rows_under16): a 500 8x8 over a '420v' of rows 8
/// (luma byte i 16 + i, chroma 128 at 64, 96 bytes) and a 501 4x4 over a one-plane '2vuy' of rows 8 (byte i 16 + i, 32
/// bytes), each read by a kernel against the same format over a surface of rows 16 whose row 0 holds the scene's first
/// row (and chroma row) and every other row 0: an M4 Max reads row 0 at the plane and rows 1 on a MiB apart, past the
/// allocation, 0 where nothing is mapped
public enum YcbcrRowsUnder16 {
    typealias P = SurfaceReads.Plane

    /// Texels not the reference's; nil where no texture was made
    public static func off(_ gpu: GPU, packed: Bool) async throws -> Int? {
        let source: IOSurfaceRef?, reference: IOSurfaceRef?
        if packed {
            source = SurfaceReads
                .surface("2vuy", 4, 4, planes: [P(at: 0, 4, 4, bpe: 2, rows: 8)], allocation: 32) { UInt8(16 + $0) }
            reference = SurfaceReads.surface("2vuy", 4, 4, planes: [P(at: 0, 4, 4, bpe: 2, rows: 16)], allocation: 64) {
                $0 < 8 ? UInt8(16 + $0) : 0
            }
        } else {
            source = SurfaceReads.surface(
                "420v", 8, 8, planes: [P(at: 0, 8, 8, bpe: 1, rows: 8), P(at: 64, 4, 4, bpe: 2, rows: 8)],
                allocation: 96
            ) { $0 < 64 ? UInt8(16 + $0) : 128 }
            reference = SurfaceReads.surface(
                "420v", 8, 8, planes: [P(at: 0, 8, 8, bpe: 1, rows: 16), P(at: 128, 4, 4, bpe: 2, rows: 16)],
                allocation: 192
            ) { i in i < 8 ? UInt8(16 + i) : (128 ..< 136).contains(i) ? 128 : 0 }
        }
        let (format, size) = packed ? (UInt(501), 4) : (UInt(500), 8)
        guard let source, let reference else { throw TestFailure("no surface") }
        guard let texture = SurfaceReads.texture(gpu, format, size, size, over: source) else { return nil }
        guard let alike = SurfaceReads.texture(gpu, format, size, size, over: reference) else {
            throw TestFailure("no reference texture")
        }
        let got = try await SurfaceReads.read(gpu, texture), want = try await SurfaceReads.read(gpu, alike)
        return got.indices.filter { got[$0] != want[$0] }.count
    }
}
