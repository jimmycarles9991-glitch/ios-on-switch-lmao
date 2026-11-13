import Foundation
import HWTestCore
import Metal

/// COMPILER.md E3, METALMAP d23, LIMITS G3: GM20B's float conversions against Metal's. m2v rounds every unblended
/// colour write in the shader, so the ROP's own conversion is reachable only through a blend that keeps it (max over
/// zero, as gpu.sm53_probe's unorm8 sweep does); the unblended store is a keyed build's probe.
public enum X1ConversionProbes {
    struct Target {
        let name: String
        let format: MTLPixelFormat
        let bytesPerPixel: Int
        /// The stored code of the first channel.
        let stored: ([UInt8], Int) -> Int
        let want: (Float16) -> Int
        let tie: (Float16) -> Bool
        /// Codes off the reference that still count as its: the sRGB formula's, which an M-series Mac's own encode
        /// misses by one at 1,643 of the halves (X1ProbeTests).
        var tolerance = 0
        /// GM20B's own store rule for the format (X1Reference.store), where one is known
        var chip: ((Float16) -> Int)?
    }

    static func u16(_ bytes: [UInt8], _ at: Int) -> Int { Int(bytes[at]) | Int(bytes[at + 1]) << 8 }

    static let targets: [Target] = [
        Target(
            name: "unorm8", format: .bgra8Unorm, bytesPerPixel: 4, stored: { Int($0[$1 * 4]) },
            want: { X1Reference.unorm(Double($0), top: 255) }, tie: { X1Reference.tie(Double($0), top: 255) },
            chip: { X1Reference.store(Double($0), bits: 8) }),
        Target(
            name: "unorm16", format: .rgba16Unorm, bytesPerPixel: 8, stored: { u16($0, $1 * 8) },
            want: { X1Reference.unorm(Double($0), top: 65535) }, tie: { X1Reference.tie(Double($0), top: 65535) },
            chip: { X1Reference.store(Double($0), bits: 16) }),
        Target(
            name: "srgb8", format: .bgra8Unorm_srgb, bytesPerPixel: 4, stored: { Int($0[$1 * 4]) },
            want: { X1Reference.srgb8(Double($0)) }, tie: { _ in false }, tolerance: 1),
        Target(
            name: "rgb10a2", format: .rgb10a2Unorm, bytesPerPixel: 4, stored: { u16($0, $1 * 4) & 0x3ff },
            want: { X1Reference.unorm(Double($0), top: 1023) }, tie: { X1Reference.tie(Double($0), top: 1023) },
            chip: { X1Reference.store(Double($0), bits: 10) }),
        Target(
            name: "half", format: .rgba16Float, bytesPerPixel: 8, stored: { u16($0, $1 * 8) },
            want: { $0.isNaN || $0 <= 0 ? 0 : Int($0.bitPattern) }, tie: { _ in false }),
    ]

    public static func rop(_ gpu: GPU) async throws -> ProbeFinding {
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        let pipeline = { (format: MTLPixelFormat) in
            try gpu.render(fragment: "x1_half_bits", format: format) { descriptor in
                let attachment = descriptor.colorAttachments[0]!
                attachment.isBlendingEnabled = true
                attachment.rgbBlendOperation = .max
                attachment.alphaBlendOperation = .max
            }
        }
        for target in targets {
            let texture = try gpu.texture(target.format, 256, 256, usage: [.renderTarget, .shaderRead])
            let state = try pipeline(target.format)
            paths.append(
                try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = texture
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
                    pass.colorAttachments[0].storeAction = .store
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    encoder.endEncoding()
                }.path)
            let bytes = try await gpu.read(texture, bytesPerPixel: target.bytesPerPixel)
            var count = 0
            var ties = 0
            var byOne = 0
            var first: String?
            var chipMisses = 0
            for i in 0..<65536 {
                let h = Float16(bitPattern: UInt16(i))
                let (have, want) = (target.stored(bytes, i), target.want(h))
                if let chip = target.chip, have != chip(h) { chipMisses += 1 }
                guard have != want else { continue }
                if target.tie(h) { ties += 1 }
                if abs(have - want) == 1 { byOne += 1 }
                guard abs(have - want) > target.tolerance else { continue }
                count += 1
                if first == nil { first = "\(String(i, radix: 16)):\(have),want_\(want)" }
            }
            wrong[target.name + "_wrong"] = count
            metrics.set(target.name + "_ties", ties)
            metrics.set(target.name + "_by_one", byOne)
            if target.chip != nil { metrics.set(target.name + "_chip_misses", chipMisses) }
            if let first { metrics.set(target.name + "_first", first) }
        }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }

    /// Each unorm code sampled as half, against the half nearest code / top (ties to even); a toward-zero count beside
    /// it tells which rounding the lowered sample does. TEXS.F16's own rounding needs the compiler to emit it (keyed).
    public static func sampler(_ gpu: GPU) async throws -> ProbeFinding {
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        let kernel = try gpu.compute("x1_sample_half")
        for (name, format, top, width, height) in [
            ("r8", MTLPixelFormat.r8Unorm, 255.0, 256, 1), ("r16", MTLPixelFormat.r16Unorm, 65535.0, 256, 256),
        ] {
            let count = width * height
            let texture = try gpu.texture(format, width, height, usage: [.shaderRead])
            let bytes: [UInt8] =
                top == 255
                ? (0..<count).map { UInt8($0) } : (0..<count).flatMap { [UInt8($0 & 0xff), UInt8($0 >> 8)] }
            try await gpu.upload(bytes, to: texture, bytesPerRow: width * (top == 255 ? 1 : 2))
            let out = try gpu.buffer(count * 8)
            paths.append(
                try await gpu.run { commands in
                    let encoder = commands.makeComputeCommandEncoder()!
                    encoder.setComputePipelineState(kernel)
                    encoder.setTexture(texture, index: 0)
                    encoder.setBuffer(out, offset: 0, index: 0)
                    encoder.dispatchThreads(
                        MTLSize(width: width, height: height, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 16, height: 1, depth: 1))
                    encoder.endEncoding()
                }.path)
            let got = out.contents().bindMemory(to: UInt16.self, capacity: count * 4)
            var nearest = 0
            var towardZero = 0
            var first: String?
            for code in 0..<count {
                let have = got[code * 4]
                let exact = Double(code) / top
                let rne = Float16(exact).bitPattern
                var rz = Float16(exact)
                if Double(rz) > exact { rz = rz.nextDown }
                if have != rne {
                    nearest += 1
                    if first == nil { first = "\(code):\(String(have, radix: 16)),want_\(String(rne, radix: 16))" }
                }
                if have != rz.bitPattern { towardZero += 1 }
            }
            wrong[name + "_wrong"] = nearest
            metrics.set(name + "_toward_zero_misses", towardZero)
            if let first { metrics.set(name + "_first", first) }
        }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }
}
