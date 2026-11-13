import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.compressed_targets' shadow cases (drivers/TegraGM20BMetal/raw/runtime/raw_comp.h copyEngineReadsCompressed):
/// a pitch target over a buffer, rendered through a block-linear image the copy engine fills before the pass and stores
/// after it, which is compressed memory (C32 for BGRA8, C64 for RGBA16Float) once the facade has made some. Two ways to
/// get one: the target beside a memoryless depth attachment (`mixed`), and rows not a multiple of 128 bytes
/// (`unaligned`). Each round: the pattern blitted in, a pass that loads it and draws the left half, the target blitted out
/// and read by the CPU (`load`, `cpu`); then a pass that clears it (to the boot ZBC table's opaque black or to an exact
/// colour on alternate rounds) and draws the left half, blitted out (`clear`); then the pattern again under a
/// framebuffer-fetching draw that writes back what it reads with alpha 1 (`fetch`: the pass binds the colour as a storage
/// image, so its shadow must stay plain memory and the draw keep its storage form). Every byte the copy engine reads back
/// must be the expected one; `cpu` (the buffer's CPU view) is reported, not judged here. ce_copies: fills and stores
/// through compressed memory, which must move; storage_fallback_compressed must not; shadow_paths must be raw.
enum CompressedShadowCases {
    static let width = 320, height = 180, rounds = 24

    struct Format {
        let name: String
        let pixel: MTLPixelFormat
        let bytes: Int
    }

    static let formats = [
        Format(name: "bgra8", pixel: .bgra8Unorm, bytes: 4), Format(name: "f16", pixel: .rgba16Float, bytes: 8),
    ]

    static let quad = SIMD4<Float>(1, 0, 0, 1)

    /// The boot ZBC table's opaque black, and a colour the format holds exactly
    static func clears(_ f: Format) -> [MTLClearColor] {
        [
            MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1),
            f.bytes == 4
                ? MTLClearColor(red: 51.0 / 255, green: 102.0 / 255, blue: 204.0 / 255, alpha: 1)
                : MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1),
        ]
    }

    static func texel(_ c: SIMD4<Double>, _ f: Format) -> [UInt8] {
        if f.bytes == 4 { return [c.z, c.y, c.x, c.w].map { UInt8(($0 * 255).rounded()) } }
        return [c.x, c.y, c.z, c.w].flatMap { v -> [UInt8] in
            let bits = Float16(Float(v)).bitPattern
            return [UInt8(bits & 0xff), UInt8(bits >> 8)]
        }
    }

    static func pattern(_ f: Format, round: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: width * height * f.bytes)
        for i in 0..<width * height {
            var v = UInt32(truncatingIfNeeded: (i &* 2_654_435_761) ^ (round &* 40_503))
            v ^= v >> 15
            for b in 0..<f.bytes { bytes[i * f.bytes + b] = UInt8(truncatingIfNeeded: v >> UInt32(8 * (b % 4))) }
            if f.bytes == 8 {
                for c in 0..<4 {
                    let half = UInt16(truncatingIfNeeded: v >> UInt32(c * 3)) % 0x3c00
                    bytes[i * 8 + c * 2] = UInt8(half & 0xff)
                    bytes[i * 8 + c * 2 + 1] = UInt8(half >> 8)
                }
            }
        }
        return bytes
    }

    /// `under` with the left half the quad's colour
    static func expected(_ under: [UInt8], _ f: Format) -> [UInt8] {
        var out = under
        let q = texel(SIMD4<Double>(quad), f)
        for y in 0..<height {
            for x in 0..<width / 2 {
                out.replaceSubrange((y * width + x) * f.bytes..<(y * width + x + 1) * f.bytes, with: q)
            }
        }
        return out
    }

    /// `under` with every alpha 1
    static func opaque(_ under: [UInt8], _ f: Format) -> [UInt8] {
        var out = under
        let one: [UInt8] = f.bytes == 4 ? [255] : [0x00, 0x3c]
        for t in 0..<width * height {
            out.replaceSubrange((t + 1) * f.bytes - one.count..<(t + 1) * f.bytes, with: one)
        }
        return out
    }

    static func run(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        let start = RawFacade.compression(of: gpu.device)
        var paths: Set<String> = []
        for f in formats {
            for mixed in [true, false] {
                let label = "shadow_\(mixed ? "mixed" : "unaligned")_\(f.name)"
                let rowBytes = width * f.bytes + (mixed ? 0 : 64)
                let memory = try gpu.buffer(rowBytes * height)
                let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                    pixelFormat: f.pixel, width: width, height: height, mipmapped: false)
                descriptor.usage = [.renderTarget, .shaderRead]
                descriptor.storageMode = .shared
                guard let target = memory.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: rowBytes) else {
                    wrong.append("\(label)_texture")
                    continue
                }
                let depth =
                    mixed
                    ? try gpu.texture(.depth32Float, width, height, usage: [.renderTarget], storage: .memoryless) : nil
                let zeta: MTLPixelFormat = mixed ? .depth32Float : .invalid
                let state = try gpu.render(fragment: "exact_fragment", format: f.pixel, depth: zeta)
                let fetch = try gpu.render(
                    vertex: "fetch_depth_vertex", fragment: "fetch_add_fragment", format: f.pixel, depth: zeta)
                let half = { (encoder: MTLRenderCommandEncoder) in
                    encoder.setRenderPipelineState(state)
                    GPU.draw(
                        encoder, corners: GPU.pixelQuad(0, 0, width / 2, height, width: width, height: height),
                        color: quad)
                }
                var bad = ["load": 0, "cpu": 0, "clear": 0, "fetch": 0]
                var engagedAt = -1
                for round in 0..<rounds {
                    let before = RawFacade.compression(of: gpu.device)["ce_copies"] ?? 0
                    let under = pattern(f, round: round)
                    try await gpu.upload(under, to: target, bytesPerRow: width * f.bytes)
                    paths.insert(try await pass(gpu, target, depth, load: nil, draw: half))
                    let loaded = expected(under, f)
                    if try await gpu.read(target, bytesPerPixel: f.bytes) != loaded { bad["load", default: 0] += 1 }
                    let cpu = (0..<height).flatMap { y in
                        Array(UnsafeRawBufferPointer(start: memory.contents() + y * rowBytes, count: width * f.bytes))
                    }
                    if cpu != loaded { bad["cpu", default: 0] += 1 }
                    let clear = clears(f)[round % 2]
                    paths.insert(try await pass(gpu, target, depth, load: clear, draw: half))
                    let cleared = expected(
                        Array(
                            repeating: texel(SIMD4(clear.red, clear.green, clear.blue, clear.alpha), f),
                            count: width * height
                        )
                        .flatMap { $0 }, f)
                    if try await gpu.read(target, bytesPerPixel: f.bytes) != cleared { bad["clear", default: 0] += 1 }
                    try await gpu.upload(under, to: target, bytesPerRow: width * f.bytes)
                    paths.insert(
                        try await pass(gpu, target, depth, load: nil) { encoder in
                            encoder.setRenderPipelineState(fetch)
                            var z: Float = 0.5
                            var nothing = SIMD4<Float>(0, 0, 0, 1)
                            encoder.setVertexBytes(&z, length: 4, index: 0)
                            encoder.setFragmentBytes(&nothing, length: 16, index: 0)
                            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                        })
                    if try await gpu.read(target, bytesPerPixel: f.bytes) != opaque(under, f) {
                        bad["fetch", default: 0] += 1
                    }
                    if engagedAt < 0 && (RawFacade.compression(of: gpu.device)["ce_copies"] ?? 0) > before {
                        engagedAt = round
                    }
                    try await Task.sleep(nanoseconds: 10_000_000)
                }
                for (name, count) in bad {
                    metrics.set("\(label)_\(name)_wrong", count)
                    if count > 0 && name != "cpu" { wrong.append("\(label)_\(name)") }
                }
                metrics.set("\(label)_compressed_from_round", engagedAt)
            }
        }
        let end = RawFacade.compression(of: gpu.device)
        let delta = { (key: String) in (end[key] ?? 0) - (start[key] ?? 0) }
        metrics.set("shadow_paths", paths.sorted().joined(separator: ","))
        metrics.set("ce_copies", delta("ce_copies"))
        metrics.set("storage_fallback_compressed", delta("storage_fallback_compressed"))
        if paths != ["raw"] { wrong.append("shadow_paths") }
        if delta("ce_copies") == 0 { wrong.append("no_compressed_shadow") }
        if delta("storage_fallback_compressed") != 0 { wrong.append("storage_fallback_compressed") }
    }

    /// A pass over `target` that loads it (`load` nil) or clears it to `load`, then `draw`s: its path
    static func pass(
        _ gpu: GPU, _ target: MTLTexture, _ depth: MTLTexture?, load: MTLClearColor?,
        draw: (MTLRenderCommandEncoder) -> Void
    ) async throws -> String {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = load == nil ? .load : .clear
            if let load { pass.colorAttachments[0].clearColor = load }
            pass.colorAttachments[0].storeAction = .store
            if let depth {
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.storeAction = .dontCare
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            draw(encoder)
            encoder.endEncoding()
        }.path
    }
}
