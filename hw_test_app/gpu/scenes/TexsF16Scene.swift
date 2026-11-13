import Foundation
import HWTestCore
import Metal

/// Decides raw_chip_facts.h AssumeTexsF16: TEXS.F16 against round-to-nearest-even of TEXS's F32 result, over every
/// unorm8 code and every half, point sampled and bilinear between neighbours. It also holds bound TEXS's F32 form to
/// TEX.B's (a compute kernel's textures stay bindless), since bound textures had not run on GM20B before. Every target is
/// a float format the raw path renders: the F16 form's bits arrive as whole floats, the proof word as a float's bits.
struct TexsF16Scene: GPUScene {
    let id = "gpu.raw.texs_f16"
    let name = "TEXS.F16 against round-to-nearest-even of its F32 form; bound TEXS against TEX.B"

    /// (name, bilinear, offset in texels from the pixel's centre). metal2vulkan fetches a nearest pixel-coordinate sample
    /// as a texel load, so "point" decides TLDS.F16 and the bilinear ones TEXS.F16 ("centre": each texel exactly).
    static let samplings: [(String, UInt32, SIMD2<Float>)] = [
        ("point", 0, SIMD2(0, 0)), ("centre", 1, SIMD2(0, 0)), ("quarter", 1, SIMD2(0.25, 0)),
        ("half", 1, SIMD2(0.5, 0.5)),
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        try await probe(gpu, "unorm8", .rgba8Unorm, 256, 1, unorm8Texels(), &result)
        try await probe(gpu, "rgba16f", .rgba16Float, 256, 256, halfTexels(), &result)
        return result
    }

    /// Every code in each channel, in a different order per channel
    private func unorm8Texels() -> [UInt8] {
        var out = [UInt8]()
        for k in 0..<256 {
            out += [UInt8(k), UInt8(255 - k), UInt8((k * 37) & 255), UInt8((k * 101) & 255)]
        }
        return out
    }

    /// Every half in R, its negation in G, and two other orders in B and A
    private func halfTexels() -> [UInt8] {
        var out = [UInt8]()
        out.reserveCapacity(65536 * 8)
        for v in 0..<65536 {
            let swapped: Int = ((v << 8) | (v >> 8)) & 0xFFFF
            let words: [UInt16] = [UInt16(v), UInt16(v) ^ 0x8000, UInt16((v &* 40503) & 0xFFFF), UInt16(swapped)]
            for w in words {
                out.append(UInt8(w & 0xFF))
                out.append(UInt8(w >> 8))
            }
        }
        return out
    }

    private func probe(
        _ gpu: GPU, _ label: String, _ format: MTLPixelFormat, _ width: Int, _ height: Int, _ texels: [UInt8],
        _ result: inout SceneResult
    ) async throws {
        let texelBytes = texels.count / (width * height)
        let source = try gpu.texture(format, width, height, usage: [.shaderRead], storage: .shared)
        source.replace(
            region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: texels,
            bytesPerRow: width * texelBytes)
        let bindless = try gpu.compute("texs_f16_bindless")
        for (sampling, linear, offset) in Self.samplings {
            let pipeline = try gpu.render(
                fragment: linear == 1 ? "texs_f16_linear" : "texs_f16_point", format: .rgba32Float
            ) {
                $0.colorAttachments[1].pixelFormat = .rgba32Float
                $0.colorAttachments[2].pixelFormat = .r32Float
            }
            let f32 = try gpu.texture(.rgba32Float, width, height, usage: [.renderTarget])
            let f16 = try gpu.texture(.rgba32Float, width, height, usage: [.renderTarget])
            let proof = try gpu.texture(.r32Float, width, height, usage: [.renderTarget])
            let reference = try gpu.buffer(width * height * 16)
            var at = offset, filter = linear
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                for (i, target) in [f32, f16, proof].enumerated() {
                    pass.colorAttachments[i].texture = target
                    pass.colorAttachments[i].loadAction = .clear
                    pass.colorAttachments[i].storeAction = .store
                }
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var corners: [SIMD4<Float>] = [
                    SIMD4(-1, -1, 0, 1), SIMD4(1, -1, 0, 1), SIMD4(-1, 1, 0, 1), SIMD4(1, 1, 0, 1),
                ]
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentTexture(source, index: 0)
                encoder.setFragmentTexture(source, index: 1)
                encoder.setFragmentBytes(&at, length: MemoryLayout<SIMD2<Float>>.stride, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
                let compute = commands.makeComputeCommandEncoder()!
                compute.setComputePipelineState(bindless)
                compute.setTexture(source, index: 0)
                compute.setBuffer(reference, offset: 0, index: 0)
                compute.setBytes(&at, length: MemoryLayout<SIMD2<Float>>.stride, index: 1)
                compute.setBytes(&filter, length: 4, index: 2)
                compute.dispatchThreads(
                    MTLSize(width: width, height: height, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: min(width, 16), height: min(height, 16), depth: 1))
                compute.endEncoding()
            }
            let name = "\(label)_\(sampling)"
            result.metrics.set("\(name)_path", timing.path)
            let floats = try await gpu.read(f32, bytesPerPixel: 16)
            let halves = try await gpu.read(f16, bytesPerPixel: 16)
            let stamp = try await gpu.read(proof, bytesPerPixel: 4)
            let proofWord = stamp.withUnsafeBytes { $0.load(as: UInt32.self) }
            let bindlessFloats = UnsafeBufferPointer(
                start: reference.contents().bindMemory(to: UInt32.self, capacity: width * height * 4),
                count: width * height * 4)
            var f16Wrong = 0, boundWrong = 0, nanPayloadOnly = 0
            var firstWrong: String?
            floats.withUnsafeBytes { f in
                halves.withUnsafeBytes { h in
                    for i in 0..<(width * height * 4) {
                        let bits = f.load(fromByteOffset: 4 * i, as: UInt32.self)
                        if bits != bindlessFloats[i] { boundWrong += 1 }
                        guard let half = UInt16(exactly: h.load(fromByteOffset: 4 * i, as: Float.self)) else {
                            f16Wrong += 1
                            continue
                        }
                        let want = Float16(Float(bitPattern: bits)).bitPattern
                        if half == want { continue }
                        if Float16(bitPattern: half).isNaN && Float(bitPattern: bits).isNaN {
                            nanPayloadOnly += 1
                            continue
                        }
                        f16Wrong += 1
                        if firstWrong == nil { firstWrong = "\(hex(bits))->\(hex(UInt32(half)))!=\(hex(UInt32(want)))" }
                    }
                }
            }
            result.metrics.set("\(name)_proof", hex(proofWord))
            result.metrics.set("\(name)_f16_wrong", f16Wrong)
            result.metrics.set("\(name)_f16_nan_payload", nanPayloadOnly)
            result.metrics.set("\(name)_bound_wrong", boundWrong)
            if let firstWrong { result.notes.append("\(name) first: \(firstWrong)") }
            let stamped = proofWord >> 16 == 0x5346 && proofWord & 0xFFFF > 0
            // an unstamped marker is a signalling NaN's bits, which a store may quiet
            let marker = proofWord & ~0x0040_0000 == 0x7FA5_5347
            result.expect(stamped || marker, "\(name): the proof output was not written")
            if !RawFacade.counters(of: gpu.device).isEmpty {
                result.expect(stamped, "\(name): NAK did not fold the F16 fetch")
                result.expect(timing.path == "raw", "\(name) left raw: \(timing.path)")
            }
            result.expect(boundWrong == 0, "\(name): bound TEXS differs from TEX.B on \(boundWrong) components")
        }
    }

    private func hex(_ v: UInt32) -> String { String(format: "0x%08x", v) }
}
