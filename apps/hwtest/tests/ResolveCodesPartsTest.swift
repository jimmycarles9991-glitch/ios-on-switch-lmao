import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// The 2D engine's code resolve into part of a texture: a 64x64 4x RGBA8Snorm or BGR10_XR (554) target, each sample
/// drawn its own codes as gpu.gm20b.resolve_codes draws them (ResolveCodesTest.codes), MultisampleResolve into level 1 of
/// a 128x128 mipmapped texture (level), slice 1 of a two-slice array (slice), a 64x64 texture under renderTargetWidth
/// 40 and renderTargetHeight 24 (area), or a texture over a buffer whose rows are 64 * 4 + 16 bytes (shadow), every
/// byte of each 0x5a before. Each drawn texel is floor(sum / 4 + 0.5) in every channel (gpu.gm20b.resolve_codes' rule);
/// area resolves the render area rounded up to 32 pixels, [0, 64) x [0, 32), the clear's (0, 0, 0, 1) codes past the
/// drawn 40x24; every other level, slice, texel and row pad keeps 0x5a; tolerance 0. An M4 Max, this scene: 0 off and
/// nothing changed outside in every case of both formats. Per format and case <fmt>_<case>_off texels off, _max the
/// largest |off| in codes, _outside bytes changed outside the resolved part; every command buffer raw, failed_cbs=0.
struct ResolveCodesPartsTest: HWTest {
    let id = "gpu.gm20b.resolve_codes_parts"
    let name =
        "Signed normalized and BGR10_XR resolves into a level, a slice, part of a texture and a shadow target, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Read {
        var inside: [UInt8] = []
        var outside: [UInt8] = []
    }

    static let side = 64, samples = 4, fill: UInt8 = 0x5a
    static let area = (width: 40, height: 24)
    static let shadowRow = 64 * 4 + 16
    static let formats = [ResolveCodesTest.formats[2], ResolveCodesTest.formats[6]]
    static let cases = ["level", "slice", "area", "shadow"]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for f in Self.formats {
            for c in Self.cases {
                let key = "\(f.key)_\(c)"
                do {
                    let read = try await Self.resolved(gpu, f, c)
                    let judged = Self.judged(f, c, read.inside)
                    let outside = read.outside.filter { $0 != Self.fill }.count
                    metrics.set(key + "_off", judged.off)
                    metrics.set(key + "_max", judged.worst)
                    metrics.set(key + "_outside", outside)
                    checks.append(
                        ("\(key): the codes' mean inside, the prior bytes outside", judged.off + outside == 0)
                    )
                } catch {
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["paths", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func size(_ c: String) -> (width: Int, height: Int) {
        c == "area" ? (min(side, (area.width + 31) / 32 * 32), min(side, (area.height + 31) / 32 * 32)) : (side, side)
    }

    static func judged(_ f: ResolveCodesTest.Format, _ c: String, _ inside: [UInt8]) -> (off: Int, worst: Int) {
        let codes = ResolveCodesTest.codes(f, samples: samples)
        let (width, height) = size(c)
        var judged = (off: 0, worst: 0)
        for y in 0 ..< height {
            for x in 0 ..< width {
                let texel = y * side + x
                let drawn = c != "area" || (x < area.width && y < area.height)
                let offs = (0 ..< f.channels).map { channel -> Int in
                    let sum = (0 ..< samples).reduce(0) { $0 + codes[($1 * side * side + texel) * 4 + channel] }
                    let mean = Int((Double(sum) / Double(samples) + 0.5).rounded(.down))
                    let want = drawn ? mean : clearCode(f, channel)
                    return f.code(inside, texel: y * width + x, channel: channel) - want
                }
                judged.off += offs.allSatisfy { $0 == 0 } ? 0 : 1
                judged.worst = max(judged.worst, offs.map(abs).max() ?? 0)
            }
        }
        return judged
    }

    static func clearCode(_ f: ResolveCodesTest.Format, _ channel: Int) -> Int {
        let value = channel == 3 ? 1 : 0
        return f.snorm > 0 ? value * f.snorm : 384 + value * 510
    }

    static func target(_ gpu: GPU, _ f: ResolveCodesTest.Format, _ c: String) async throws -> (MTLTexture, MTLBuffer?) {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: f.format, width: side, height: side, mipmapped: false
        )
        d.usage = [.renderTarget, .shaderRead]
        switch c {
        case "level":
            (d.width, d.height, d.mipmapLevelCount) = (2 * side, 2 * side, 8)
        case "slice":
            (d.textureType, d.arrayLength) = (.type2DArray, 2)
        case "shadow":
            d.storageMode = .shared
            let buffer = try gpu.buffer([UInt8](repeating: fill, count: shadowRow * side))
            guard let texture = buffer.makeTexture(descriptor: d, offset: 0, bytesPerRow: shadowRow) else {
                throw TestFailure("no \(f.key) texture over a buffer")
            }
            return (texture, buffer)
        default: break
        }
        d.storageMode = .private
        guard let texture = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no \(f.key) \(c) target") }
        for level in 0 ..< d.mipmapLevelCount {
            for slice in 0 ..< d.arrayLength {
                let width = max(1, d.width >> level), height = max(1, d.height >> level)
                let bytes = [UInt8](repeating: fill, count: width * height * f.bytesPerTexel)
                try await gpu.upload(
                    bytes, to: texture, bytesPerRow: width * f.bytesPerTexel, level: level, slice: slice
                )
            }
        }
        return (texture, nil)
    }

    static func resolved(_ gpu: GPU, _ f: ResolveCodesTest.Format, _ c: String) async throws -> Read {
        let texels = side * side
        let codes = ResolveCodesTest.codes(f, samples: samples)
        let values = try gpu.buffer(
            (0 ..< samples * texels).map { at in
                SIMD4<Float>((0 ..< 4).map { $0 < f.channels ? f.value(codes[at * 4 + $0]) : 0 })
            }
        )
        let state = try gpu.render(fragment: "masked_texel_values", format: f.format, samples: samples)
        let drawn = try gpu.texture(f.format, side, side, usage: [.renderTarget], samples: samples)
        let (into, shadow) = try await target(gpu, f, c)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawn
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .multisampleResolve
            pass.colorAttachments[0].resolveTexture = into
            pass.colorAttachments[0].resolveLevel = c == "level" ? 1 : 0
            pass.colorAttachments[0].resolveSlice = c == "slice" ? 1 : 0
            if c == "area" { (pass.renderTargetWidth, pass.renderTargetHeight) = area }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            for sample in 0 ..< samples {
                var at = SIMD2<UInt32>(UInt32(side), UInt32(sample))
                encoder.setFragmentBuffer(values, offset: sample * texels * 16, index: 0)
                encoder.setFragmentBytes(&at, length: 8, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        let row = side * f.bytesPerTexel
        var read = Read()
        if let shadow {
            let bytes = [UInt8](UnsafeRawBufferPointer(start: shadow.contents(), count: shadowRow * side))
            for y in 0 ..< side {
                read.inside += bytes[y * shadowRow ..< y * shadowRow + row]
                read.outside += bytes[y * shadowRow + row ..< (y + 1) * shadowRow]
            }
            return read
        }
        for level in 0 ..< into.mipmapLevelCount {
            for slice in 0 ..< into.arrayLength {
                let bytes = try await gpu.read(into, bytesPerPixel: f.bytesPerTexel, level: level, slice: slice)
                guard level == (c == "level" ? 1 : 0) && slice == (c == "slice" ? 1 : 0) else {
                    read.outside += bytes
                    continue
                }
                let (width, height) = size(c)
                for y in 0 ..< side {
                    let line = bytes[y * row ..< (y + 1) * row]
                    if y >= height {
                        read.outside += line
                        continue
                    }
                    read.inside += line.prefix(width * f.bytesPerTexel)
                    read.outside += line.dropFirst(width * f.bytesPerTexel)
                }
            }
        }
        return read
    }
}
