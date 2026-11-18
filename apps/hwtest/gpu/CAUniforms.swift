import Foundation
import HWTestCore
import Metal
import simd

/// Uniforms that make two Core Animation states draw something a skipped draw would not leave, bound where each function
/// reads them (QuartzCore 24B119's reflection on a Mac): the glass's are a real frame's, the luma boost's are chosen.
public enum CAUniforms {
    /// luma_boost_lph: it fetches the target's own colour and maps it through the LUT at fragment index 3, scaled by
    /// u = {half scale, half inv_edr, half edr} at fragment buffer 1, so its target must hold a picture before it draws.
    public static func bindLumaBoost(_ encoder: MTLRenderCommandEncoder, lut: MTLTexture) {
        var texmat = matrix_identity_float4x4
        var u = UniformBlock(8)
        u.half(0, 1.5)
        u.half(2, 0.5)
        u.half(4, 2)
        encoder.setVertexBytes(&texmat, length: MemoryLayout<simd_float4x4>.size, index: 3)
        encoder.setFragmentBytes(u.bytes, length: u.bytes.count, index: 1)
        encoder.setFragmentTexture(lut, index: 3)
    }

    /// A 256x1 RGBA16Float LUT that moves every channel differently: squared, linear, inverted.
    public static func lumaLut() -> Raster {
        Raster(width: 256, height: 1, channels: 4) { x, _, c in
            let v = Float(x) / 255
            return [v * v, v, 1 - v, 1][c]
        }
    }

    /// glass_background_all_lph: the picture under the glass at fragment index 3, its signed distance field at 4 (in
    /// pixels, negative inside: a field positive inside draws one flat colour), the 240-byte parameter block at fragment
    /// buffer 1, its 28-byte extension at 9, the EDR scale at 6 and the texture matrix (a float4x4) at vertex buffer 3.
    public static func bindGlass(_ encoder: MTLRenderCommandEncoder, source: MTLTexture, sdf: MTLTexture) {
        var texmat = matrix_identity_float4x4
        var edr = Float16(1)
        let u = glassBlock(), ext = glassExtension()
        encoder.setVertexBytes(&texmat, length: MemoryLayout<simd_float4x4>.size, index: 3)
        encoder.setFragmentBytes(u.bytes, length: u.bytes.count, index: 1)
        encoder.setFragmentBytes(ext.bytes, length: ext.bytes.count, index: 9)
        encoder.setFragmentBytes(&edr, length: MemoryLayout<Float16>.size, index: 6)
        encoder.setFragmentTexture(source, index: 3)
        encoder.setFragmentTexture(sdf, index: 4)
    }

    /// The 240-byte glass block and the 28-byte extension of a real frame's glass: macOS 27's QuartzCore drawing an
    /// NSGlassEffectView over a 1280x720 picture (glass_background_sdf_all's u after its 48-byte sdf part, and its u_ext),
    /// captured on a Mac (setFragmentBuffer at indices 1 and 9). Little-endian words.
    static let glassWords = [
        "abaa2a3a 00000000 00000000 000000bb 000070c2 cdcc4c3d 00000000 0000807f 000080bf 00000000 00000040 00000000",
        "0000fc42 2108023c 00000000 0000807f 00000000 000000c1 00000000 0000807f fc3cd7b1 b0a41f2d f0aa783c b8a41f2d",
        "f3aad5b1 203d1f2d f03a27b7 c4a90030 40b06d38 c7a90030 41b027b7 a43b0030 9a396783 67030000 3e009a39 01810000",
        "60826002 9a390000 00000000 9a99993e 663a0000 66b20000 00000000 00000000 003c0000 0000003c 00bc003c 00000000",
        "00000000 003ce270 000000c8 00440045 0000003c 003c0000 003800b8 000000b8 00b26642 0000333b 00380000 003c0000",
    ]
    static let glassExtensionWords = [
        "cdcc4c3d 00000000 0000803f 74a1903f 6666263f 00010101 01000000"
    ]

    static func glassBlock() -> UniformBlock { UniformBlock(hex: glassWords.joined(separator: " ")) }

    static func glassExtension() -> UniformBlock { UniformBlock(hex: glassExtensionWords.joined(separator: " ")) }
}

/// A uniform buffer's bytes, written at the offsets a function's reflection names.
public struct UniformBlock {
    public var bytes: [UInt8]

    public init(_ length: Int) { bytes = [UInt8](repeating: 0, count: length) }

    /// Bytes written as hex, whitespace between them ignored.
    public init(hex: String) {
        let digits = Array(hex.filter { !$0.isWhitespace })
        bytes = stride(from: 0, to: digits.count, by: 2).map { UInt8(String(digits[$0...$0 + 1]), radix: 16)! }
    }

    public mutating func floats(_ offset: Int, _ values: [Float]) {
        for (i, value) in values.enumerated() { put(offset + 4 * i, value.bitPattern) }
    }

    public mutating func halves(_ offset: Int, _ values: [Float]) {
        for (i, value) in values.enumerated() { half(offset + 2 * i, value) }
    }

    public mutating func half(_ offset: Int, _ value: Float) { put(offset, Float16(value).bitPattern) }

    mutating func put<T: FixedWidthInteger>(_ offset: Int, _ value: T) {
        withUnsafeBytes(of: value.littleEndian) { bytes.replaceSubrange(offset..<(offset + $0.count), with: $0) }
    }
}
