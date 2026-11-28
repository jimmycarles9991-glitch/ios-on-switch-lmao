import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// generateMipmapsForTexture: of formats whose levels the driver filters as words (pack/fmt.metal word states),
/// against an M4 Max's levels of the same chain (WordMipGoldens.swift, made by this file's own code there): 16x16 2D
/// chains of the extended-range formats, 101x63 2D and 45x37x7 3D chains whose steps weight texels and planes off 1/2.
/// A texel is judged by its largest difference in codes from Apple's (RGB9E5: in mantissa steps of Apple's exponent).
public struct WordMipFormat {
    public let key: String
    public let format: MTLPixelFormat
    public let bytes: Int
    public let volume: Bool
    /// Each channel's (bit offset, bit width) in the texel's little-endian word
    public let fields: [(shift: Int, width: Int)]
    public var size: (width: Int, height: Int, depth: Int) = (16, 16, 1)

    public init(
        key: String, format: MTLPixelFormat, bytes: Int, volume: Bool, fields: [(shift: Int, width: Int)],
        size: (width: Int, height: Int, depth: Int) = (16, 16, 1)
    ) {
        self.key = key
        self.format = format
        self.bytes = bytes
        self.volume = volume
        self.fields = fields
        self.size = size
    }

    public static let bgra10: [(shift: Int, width: Int)] = [(38, 10), (22, 10), (6, 10), (54, 10)]
    public static let bgr10: [(shift: Int, width: Int)] = [(20, 10), (10, 10), (0, 10)]

    public static let a1bgr5: [(shift: Int, width: Int)] = [(11, 5), (6, 5), (1, 5), (0, 1)]
    public static let abgr4: [(shift: Int, width: Int)] = [(12, 4), (8, 4), (4, 4), (0, 4)]
    public static let rgb9e5: [(shift: Int, width: Int)] = [(0, 9), (9, 9), (18, 9)]

    public static func plane(
        _ key: String, _ format: MTLPixelFormat, _ bytes: Int, _ fields: [(shift: Int, width: Int)]
    )
        -> WordMipFormat
    {
        WordMipFormat(key: key, format: format, bytes: bytes, volume: false, fields: fields, size: (16, 16, 1))
    }

    /// 45x37x7, whose steps weight texels and planes off 1/2, as a power-of-two chain never does
    public static func volume(
        _ key: String, _ format: MTLPixelFormat, _ bytes: Int, _ fields: [(shift: Int, width: Int)]
    )
        -> WordMipFormat
    {
        WordMipFormat(key: key, format: format, bytes: bytes, volume: true, fields: fields, size: (45, 37, 7))
    }

    public static func odd(
        _ key: String, _ format: MTLPixelFormat, _ bytes: Int, _ fields: [(shift: Int, width: Int)]
    )
        -> WordMipFormat
    {
        WordMipFormat(key: key, format: format, bytes: bytes, volume: false, fields: fields, size: (101, 63, 1))
    }

    public static let all: [WordMipFormat] = [
        plane("f552", .bgra10_xr, 8, bgra10), plane("f553", .bgra10_xr_srgb, 8, bgra10),
        plane("f554", .bgr10_xr, 4, bgr10), plane("f555", .bgr10_xr_srgb, 4, bgr10),
        odd("rgb9e5", .rgb9e5Float, 4, rgb9e5), odd("r8_srgb", .r8Unorm_srgb, 1, [(0, 8)]),
        odd("a1bgr5", .a1bgr5Unorm, 2, a1bgr5), odd("abgr4", .abgr4Unorm, 2, abgr4),
        volume("f552_3d", .bgra10_xr, 8, bgra10), volume("f553_3d", .bgra10_xr_srgb, 8, bgra10),
        volume("f554_3d", .bgr10_xr, 4, bgr10), volume("f555_3d", .bgr10_xr_srgb, 4, bgr10),
        volume("r8_srgb_3d", .r8Unorm_srgb, 1, [(0, 8)]), volume("a1bgr5_3d", .a1bgr5Unorm, 2, a1bgr5),
        volume("abgr4_3d", .abgr4Unorm, 2, abgr4), volume("rgb9e5_3d", .rgb9e5Float, 4, rgb9e5),
    ]

    public func word(_ bytes: ArraySlice<UInt8>) -> UInt64 {
        bytes.enumerated().reduce(UInt64(0)) { $0 | UInt64($1.element) << (8 * $1.offset) }
    }

    public func codes(_ word: UInt64) -> [Int] {
        fields.map { Int(word >> UInt64($0.shift) & (1 << UInt64($0.width) - 1)) }
    }

    /// The largest difference in codes between a stored texel and Apple's; RGB9E5's in mantissa steps of Apple's
    /// exponent
    public func apart(_ stored: UInt64, _ apple: UInt64) -> Int {
        guard format == .rgb9e5Float else { return zip(codes(stored), codes(apple)).map { abs($0 - $1) }.max() ?? 0 }
        let value = { (w: UInt64, k: Int) in Double(w >> UInt64(9 * k) & 511) * pow(2, Double(w >> 27 & 31) - 24) }
        let step = pow(2, Double(apple >> 27 & 31) - 24)
        return (0 ..< 3).map { Int((abs(value(stored, $0) - value(apple, $0)) / step).rounded()) }.max() ?? 0
    }
}

public enum WordMipmapCases {
    public static func splitmix(_ s: inout UInt64) -> UInt64 {
        s = s &+ 0x9e37_79b9_7f4a_7c15
        var z = s
        z = (z ^ (z >> 30)) &* 0xbf58_476d_1ce4_e5b9
        z = (z ^ (z >> 27)) &* 0x94d0_49bb_1331_11eb
        return z ^ (z >> 31)
    }

    /// Level 0's texels: random codes in every field (RGB9E5: random mantissas under every exponent, 0 to 31)
    public static func level0(_ f: WordMipFormat, count: Int) -> [UInt8] {
        var state = UInt64(f.format.rawValue)
        return (0 ..< count).flatMap { _ -> [UInt8] in
            var word = splitmix(&state)
            if f.format == .rgb9e5Float { word = word & 0x7ff_ffff | (word >> 40 & 31) << 27 }
            if f.bytes == 8 { word &= 0xffc0_ffc0_ffc0_ffc0 }
            return (0 ..< f.bytes).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) }
        }
    }

    /// The chain's levels 1 and down as stored, planes and rows packed, and the mipmap command buffer's path
    public static func run(_ f: WordMipFormat, _ gpu: GPU) async throws -> (levels: [UInt8], path: String) {
        let (w, h, depth) = f.size
        let d = MTLTextureDescriptor()
        d.textureType = f.volume ? .type3D : .type2D
        d.pixelFormat = f.format
        (d.width, d.height, d.depth) = (w, h, depth)
        d.mipmapLevelCount = Int(log2(Double(max(w, h, depth)))) + 1
        d.usage = [.shaderRead, .renderTarget]
        d.storageMode = .private
        guard let texture = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no \(f.key) chain") }
        let staging = try gpu.buffer(level0(f, count: w * h * depth))
        let path = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: w * f.bytes, sourceBytesPerImage: w * h * f.bytes,
                sourceSize: MTLSize(width: w, height: h, depth: depth), to: texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            blit.generateMipmaps(for: texture)
            blit.endEncoding()
        }.path
        var levels: [UInt8] = []
        for level in 1 ..< d.mipmapLevelCount {
            for plane in 0 ..< (f.volume ? max(depth >> level, 1) : 1) {
                levels += try await gpu.read(texture, bytesPerPixel: f.bytes, level: level, plane: plane)
            }
        }
        return (levels, path)
    }

    /// The largest difference of `levels` from Apple's, and the texels that differ
    public static func judged(_ f: WordMipFormat, _ levels: [UInt8], apple: [UInt8]) -> (worst: Int, off: Int) {
        guard levels.count == apple.count else { return (Int.max, levels.count / f.bytes) }
        let apart = stride(from: 0, to: levels.count, by: f.bytes).map {
            f.apart(f.word(levels[$0 ..< $0 + f.bytes]), f.word(apple[$0 ..< $0 + f.bytes]))
        }
        return (apart.max() ?? 0, apart.filter { $0 > 0 }.count)
    }
}
