import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// generateMipmapsForTexture: of mipmapped 3D textures on the driver: every level drawn a plane a pass by the pack's
/// volume mip state (pack/mip.metal mip_3d_*), each texel Metal's trilinear step at its centre with weights in 1/256 steps
/// (an M4 Max's taps, probe zweight), against the same step computed here from the level above as read back. RGBA8Unorm
/// within one code, RGBA16Float within three units in the last place, R32Float within four, at an even and an odd size;
/// every command buffer on the driver.
struct MipmapVolumeTest: HWTest {
    let id = "gpu.gm20b.mipmap_3d"
    let name = "Mipmaps of 3D textures and texture views"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Arm {
        let name: String
        let format: MTLPixelFormat
        let channels: Int
        let bytes: Int // per channel
        let tolerance: Double
    }

    static let arms = [
        Arm(name: "rgba8", format: .rgba8Unorm, channels: 4, bytes: 1, tolerance: 1),
        Arm(name: "rgba16f", format: .rgba16Float, channels: 4, bytes: 2, tolerance: 3),
        Arm(name: "r32f", format: .r32Float, channels: 1, bytes: 4, tolerance: 4),
    ]

    static func value(_ raw: [UInt8], _ i: Int, _ arm: Arm) -> Double {
        switch arm.bytes {
        case 1: return Double(raw[i]) / 255
        case 2: return Double(Float(Float16(bitPattern: UInt16(raw[2 * i]) | UInt16(raw[2 * i + 1]) << 8)))
        default:
            return Double(Float(bitPattern: (0 ..< 4).reduce(UInt32(0)) { $0 | UInt32(raw[4 * i + $1]) << (8 * $1) }))
        }
    }

    /// The step's taps on one axis of `s` texels halved to `d`, at texel `x`: the two texels and the second's weight
    static func taps(_ x: Int, _ s: Int, _ d: Int) -> (Int, Int, Float) {
        let at = (2 * x + 1) * s - d
        return (min(at / (2 * d), s - 1), min(at / (2 * d) + 1, s - 1), Float((at % (2 * d) * 256 + d) / (2 * d)) / 256)
    }

    static func lerp(_ a: Float, _ b: Float, _ w: Float) -> Float { (a * (1 - w)).addingProduct(b, w) }

    /// Units between `want` and `got`: codes for 8-bit unorm, units in the last place for the float formats
    static func units(_ want: Float, _ got: Double, _ arm: Arm) -> Double {
        switch arm.bytes {
        case 1: return abs(Double((want * 255).rounded()) - (got * 255).rounded())
        case 2: return Double(abs(Int(Float16(want).bitPattern) - Int(Float16(Float(got)).bitPattern)))
        default: return Double(abs(Int64(want.bitPattern) - Int64(Float(got).bitPattern)))
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var failures: [String] = []
        for arm in Self.arms {
            for (w, h, d) in [(8, 8, 8), (9, 6, 5)] {
                let label = "\(arm.name)_\(w)x\(h)x\(d)"
                let levels = 1 + Int(log2(Double(max(w, h, d))))
                let desc = MTLTextureDescriptor()
                desc.textureType = .type3D
                desc.pixelFormat = arm.format
                desc.width = w
                desc.height = h
                desc.depth = d
                desc.mipmapLevelCount = levels
                desc.usage = [.shaderRead]
                desc.storageMode = .shared
                guard let t = gpu.device.makeTexture(descriptor: desc) else {
                    failures.append("\(label): no texture")
                    continue
                }
                let texel = arm.channels * arm.bytes
                var bytes = [UInt8](repeating: 0, count: w * h * d * texel)
                var seed: UInt32 = 0x2545_f491
                for i in bytes.indices {
                    seed = seed &* 1_664_525 &+ 1_013_904_223
                    bytes[i] = UInt8(truncatingIfNeeded: seed >> 24)
                }
                if arm.bytes == 2 { for i in stride(from: 1, to: bytes.count, by: 2) { bytes[i] &= 0x5b } }
                if arm.bytes == 4 {
                    for i in stride(from: 3, to: bytes.count, by: 4) { bytes[i] = 0x40 | (bytes[i] & 0x81) }
                }
                t.replace(
                    region: MTLRegionMake3D(0, 0, 0, w, h, d), mipmapLevel: 0, slice: 0, withBytes: bytes,
                    bytesPerRow: w * texel, bytesPerImage: w * h * texel
                )
                let path = try await gpu.run { commands in
                    let b = commands.makeBlitCommandEncoder()!
                    b.generateMipmaps(for: t)
                    b.endEncoding()
                }.path
                var wrong = 0, largest = 0.0
                var above = bytes
                for l in 1 ..< levels {
                    let s = (max(1, w >> (l - 1)), max(1, h >> (l - 1)), max(1, d >> (l - 1)))
                    let n = (max(1, w >> l), max(1, h >> l), max(1, d >> l))
                    var level = [UInt8](repeating: 0, count: n.0 * n.1 * n.2 * texel)
                    t.getBytes(
                        &level, bytesPerRow: n.0 * texel, bytesPerImage: n.0 * n.1 * texel,
                        from: MTLRegionMake3D(0, 0, 0, n.0, n.1, n.2), mipmapLevel: l, slice: 0
                    )
                    for z in 0 ..< n.2 {
                        for y in 0 ..< n.1 {
                            for x in 0 ..< n.0 {
                                let tx = Self.taps(x, s.0, n.0), ty = Self.taps(y, s.1, n.1),
                                    tz = Self.taps(z, s.2, n.2)
                                for c in 0 ..< arm.channels {
                                    let read = { (xx: Int, yy: Int, zz: Int) -> Float in
                                        Float(Self.value(above, ((zz * s.1 + yy) * s.0 + xx) * arm.channels + c, arm))
                                    }
                                    let plane = { (zz: Int) -> Float in
                                        Self.lerp(
                                            Self.lerp(read(tx.0, ty.0, zz), read(tx.1, ty.0, zz), tx.2),
                                            Self.lerp(read(tx.0, ty.1, zz), read(tx.1, ty.1, zz), tx.2), ty.2
                                        )
                                    }
                                    let want = Self.lerp(plane(tz.0), plane(tz.1), tz.2)
                                    let got = Self.value(level, ((z * n.1 + y) * n.0 + x) * arm.channels + c, arm)
                                    let off = Self.units(want, got, arm)
                                    largest = max(largest, off)
                                    wrong += off > arm.tolerance ? 1 : 0
                                }
                            }
                        }
                    }
                    above = level
                }
                metrics.set("\(label)_path", path)
                metrics.set("\(label)_wrong", wrong)
                metrics.set("\(label)_max", largest)
                if path != "gm20b" { failures.append("\(label) went off the driver (\(path))") }
                if wrong > 0 {
                    failures.append("\(label): \(wrong) channels past \(arm.tolerance) (largest \(largest))")
                }
            }
        }
        let desc = MTLTextureDescriptor()
        desc.textureType = .type3D
        desc.pixelFormat = .rgba8Unorm
        desc.width = 9
        desc.height = 6
        desc.depth = 5
        desc.mipmapLevelCount = 4
        desc.usage = [.shaderRead, .pixelFormatView]
        desc.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: desc) else { throw TestFailure("no volume view parent") }
        var initial: [[UInt8]] = []
        for level in 0 ..< 4 {
            let w = max(1, 9 >> level), h = max(1, 6 >> level), d = max(1, 5 >> level)
            var bytes = [UInt8](repeating: 255, count: w * h * d * 4)
            for z in 0 ..< d {
                for y in 0 ..< h {
                    for x in 0 ..< w {
                        let p = ((z * h + y) * w + x) * 4
                        bytes[p] = UInt8(40 + level * 40 + z * 32)
                        bytes[p + 1] = UInt8(16 + level * 24 + y * 4)
                        bytes[p + 2] = UInt8(8 + x * 8)
                    }
                }
            }
            texture.replace(
                region: MTLRegionMake3D(0, 0, 0, w, h, d), mipmapLevel: level, slice: 0, withBytes: bytes,
                bytesPerRow: w * 4, bytesPerImage: w * h * 4
            )
            initial.append(bytes)
        }
        guard
            let view = texture.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .type3D, levels: 1 ..< 3, slices: 0 ..< 1
            )
        else { throw TestFailure("no volume mip view") }
        let viewPath = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.generateMipmaps(for: view)
            blit.endEncoding()
        }.path
        var viewWrong = 0
        for level in 0 ..< 4 {
            let w = max(1, 9 >> level), h = max(1, 6 >> level), d = max(1, 5 >> level)
            var bytes = [UInt8](repeating: 0, count: w * h * d * 4)
            texture.getBytes(
                &bytes, bytesPerRow: w * 4, bytesPerImage: w * h * 4,
                from: MTLRegionMake3D(0, 0, 0, w, h, d), mipmapLevel: level, slice: 0
            )
            let expected = level == 2 ? [UInt8](arrayLiteral: 96, 44, 12, 255, 96, 44, 28, 255) : initial[level]
            for i in expected.indices where bytes[i] != expected[i] { viewWrong += 1 }
        }
        metrics.set("volume_view_path", viewPath)
        metrics.set("volume_view_wrong", viewWrong)
        if viewPath != "gm20b" { failures.append("volume view went off the driver (\(viewPath))") }
        if viewWrong > 0 { failures.append("volume view: \(viewWrong) changed or incorrect bytes") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
