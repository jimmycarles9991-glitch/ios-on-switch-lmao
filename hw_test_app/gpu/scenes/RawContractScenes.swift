import Foundation
import HWTestCore
import Metal

/// A 64x64 pattern whose neighbouring texels differ in every channel, uploaded to a private texture.
private func pattern(_ gpu: GPU, size: Int) async throws -> (MTLTexture, Raster) {
    var bytes = [UInt8](repeating: 0, count: size * size * 4)
    for y in 0..<size {
        for x in 0..<size {
            let at = (y * size + x) * 4
            bytes[at] = UInt8((x * 4) & 255)
            bytes[at + 1] = UInt8((y * 4) & 255)
            bytes[at + 2] = UInt8(((x + y) * 2) & 255)
            bytes[at + 3] = 255
        }
    }
    let texture = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead])
    try await gpu.upload(bytes, to: texture, bytesPerRow: size * 4)
    return (texture, Raster(unorm8: bytes, width: size, height: size, channels: 4))
}

/// Storage buffers bound off a multiple of 16: the vertex corners at 0x38e8 (iconservicesagent's RenderBox offset on boot
/// 13, which raw refused as enc.ubo) and the coordinates at 4 past one. Each texel is copied through the nearest
/// constexpr sampler, so the reference is the pattern itself.
struct StorageOffsetScene: GPUScene {
    let id = "gpu.raw.storage_offset"
    let name = "Storage buffers bound at offsets that are not multiples of 16"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let size = 64
        let (source, model) = try await pattern(gpu, size: size)
        let corners: [SIMD2<Float>] = [SIMD2(-1, -1), SIMD2(1, -1), SIMD2(-1, 1), SIMD2(1, 1)]
        let uvs: [Float] = [0, 1, 1, 1, 0, 0, 1, 0]
        let (cornerOffset, uvOffset) = (0x38e8, 0x2004)
        let cornerBuffer = try gpu.buffer(cornerOffset + 64)
        let uvBuffer = try gpu.buffer(uvOffset + 64)
        corners.withUnsafeBytes {
            cornerBuffer.contents().advanced(by: cornerOffset).copyMemory(from: $0.baseAddress!, byteCount: $0.count)
        }
        uvs.withUnsafeBytes {
            uvBuffer.contents().advanced(by: uvOffset).copyMemory(from: $0.baseAddress!, byteCount: $0.count)
        }
        let target = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(
            vertex: "device_textured_vertex", fragment: "nearest_fragment", format: .rgba8Unorm)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBuffer(cornerBuffer, offset: cornerOffset, index: 0)
            encoder.setVertexBuffer(uvBuffer, offset: uvOffset, index: 1)
            encoder.setFragmentTexture(source, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        result.metrics.set("path", timing.path)
        result.compare("copied", try await gpu.raster(target, .rgba8Unorm), model, tolerance: 1.01 / 255)
        return result
    }
}

/// A runtime sampler with normalizedCoordinates NO (RenderBox's and TVWidgetExtension's, which raw refused as call.sampler
/// on boot 13): nearest at texel centres in pixels copies the pattern, and linear half a texel on averages each texel
/// with its right neighbour (the last column clamps to the edge), as the normalized samplers' draws beside them do.
struct PixelSamplerScene: GPUScene {
    let id = "gpu.raw.pixel_sampler"
    let name = "Samplers with pixel coordinates beside normalized ones"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let size = 64
        let (source, model) = try await pattern(gpu, size: size)
        let pipeline = try gpu.render(vertex: "textured_vertex", fragment: "sampled_fragment", format: .rgba8Unorm)
        let averaged = Raster(width: size, height: size, channels: 4) { x, y, c in
            (model[x, y, c] + model[min(x + 1, size - 1), y, c]) / 2
        }
        let s = Float(size)
        let half = Float(0.5)
        let cases: [(String, Bool, MTLSamplerMinMagFilter, [SIMD2<Float>], Raster)] = [
            ("pixel nearest", false, .nearest, [SIMD2(0, s), SIMD2(s, s), SIMD2(0, 0), SIMD2(s, 0)], model),
            ("normalized nearest", true, .nearest, [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)], model),
            (
                "pixel linear", false, .linear,
                [SIMD2(half, s), SIMD2(s + half, s), SIMD2(half, 0), SIMD2(s + half, 0)], averaged
            ),
            (
                "normalized linear", true, .linear,
                [SIMD2(half / s, 1), SIMD2(1 + half / s, 1), SIMD2(half / s, 0), SIMD2(1 + half / s, 0)], averaged
            ),
        ]
        var paths: [String] = []
        for (label, normalized, filter, uvs, want) in cases {
            let descriptor = MTLSamplerDescriptor()
            descriptor.normalizedCoordinates = normalized
            descriptor.minFilter = filter
            descriptor.magFilter = filter
            descriptor.sAddressMode = .clampToEdge
            descriptor.tAddressMode = .clampToEdge
            guard let sampler = gpu.device.makeSamplerState(descriptor: descriptor) else {
                result.expect(false, "\(label): makeSamplerState returned nil")
                continue
            }
            let target = try gpu.texture(.rgba8Unorm, size, size)
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .dontCare
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var corners = GPU.fullScreen
                var uvs = uvs
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
                encoder.setFragmentTexture(source, index: 0)
                encoder.setFragmentSamplerState(sampler, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            paths.append(timing.path)
            result.compare(label, try await gpu.raster(target, .rgba8Unorm), want, tolerance: 1.51 / 255)
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        return result
    }
}

/// globalTraceObjectID, which MPS asks of every encoder for its kdebug traces (cameracaptured's compute encoders were
/// answered zero on boot 15): a command buffer, its blit encoder and its compute encoder each answer an ID of their
/// own, the same one when asked again, and the command buffer stays raw. m2v's encoders have no such method.
struct TraceIdScene: GPUScene {
    let id = "gpu.raw.trace_ids"
    let name = "Command buffers and encoders answer trace object IDs of their own"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let selector = NSSelectorFromString("globalTraceObjectID")
        func traceId(_ object: AnyObject) -> UInt64? {
            guard object.responds(to: selector) else { return nil }
            typealias Getter = @convention(c) (AnyObject, Selector) -> UInt64
            return unsafeBitCast(object.method(for: selector), to: Getter.self)(object, selector)
        }
        let buffer = try gpu.buffer(256)
        var asked: [(String, UInt64?, UInt64?)] = []
        let timing = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            asked.append(("blit", traceId(blit), traceId(blit)))
            blit.fill(buffer: buffer, range: 0..<256, value: 1)
            blit.endEncoding()
            let compute = commands.makeComputeCommandEncoder()!
            asked.append(("compute", traceId(compute), traceId(compute)))
            compute.endEncoding()
            asked.append(("command buffer", traceId(commands), traceId(commands)))
        }
        let kind = RawFacade.kind(of: gpu.device)
        var seen = Set<UInt64>()
        for (label, first, again) in asked {
            guard let first else {
                result.expect(kind == "m2v", "\(label) has no globalTraceObjectID")
                continue
            }
            result.expect(first != 0, "\(label) answered 0")
            result.expect(first == again, "\(label) answered \(first), then \(again ?? 0)")
            result.expect(seen.insert(first).inserted, "\(label) answered another object's ID \(first)")
        }
        result.metrics.set("ids", asked.map { "\($0.1 ?? 0)" }.joined(separator: ","))
        result.metrics.set("path", timing.path)
        if kind == "raw" { result.expect(timing.path == "raw", "drawn on \(timing.path)") }
        return result
    }
}

/// Core Animation's luma sum kernel (iOS 27's QuartzCore compute_sum_luma, at the 8-wide shape its pack record holds)
/// writing its result at an offset off 16 bytes, as backboardd binds its destination on boot 15 (raw sent each such
/// command buffer's compute to m2v). The same dispatch into an aligned destination is the reference: the shifted one's
/// bytes are the aligned one's moved by the offset, and nothing around them changes; on the raw facade both run on raw.
/// Where QuartzCore has no such kernel (iOS 17) it notes why and checks nothing.
struct KernelOffsetScene: GPUScene {
    let id = "gpu.raw.kernel_offset"
    let name = "A Core Animation kernel's buffer bound at an offset that is not a multiple of 16"

    static let offset = 4
    static let threads = 8
    static let sentinel: UInt8 = 0xA5

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        guard let library = try? gpu.device.makeLibrary(URL: CAState.library),
            let function = library.makeFunction(name: "compute_sum_luma")
        else {
            result.notes.append("no compute_sum_luma in QuartzCore here")
            return result
        }
        let pipeline = try await gpu.device.makeComputePipelineState(function: function)
        // tex_origin, tex_bounds (ushort2 each), iterations, thread_group_count (uint), pxl_per_sample, level (uchar)
        var uniforms = [UInt8](repeating: 0, count: 256)
        let words: [UInt32] = [0, 64 | 64 << 16, 1, UInt32(Self.threads)]
        words.withUnsafeBytes { uniforms.replaceSubrange(0..<16, with: $0) }
        uniforms[16] = 1
        let sums = (0..<Self.threads * 4).map { Float($0 % 7) * 0.125 + 0.0625 }
        let uniformBuffer = try gpu.buffer(uniforms)
        let sumBuffer = try gpu.buffer(sums)
        // the texture Core Animation leaves bound at slot 0, which the pack's key names
        let texture = try gpu.texture(.bgra8Unorm, 64, 64, usage: [.shaderRead])
        var outputs: [[UInt8]] = []
        var paths: [String] = []
        for offset in [0, Self.offset] {
            let destination = try gpu.buffer([UInt8](repeating: Self.sentinel, count: 64))
            let timing = try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(pipeline)
                encoder.setBuffer(uniformBuffer, offset: 0, index: 0)
                encoder.setBuffer(sumBuffer, offset: 0, index: 1)
                encoder.setBuffer(destination, offset: offset, index: 2)
                encoder.setTexture(texture, index: 0)
                encoder.setThreadgroupMemoryLength(16 * Self.threads, index: 0)
                encoder.dispatchThreadgroups(
                    MTLSize(width: 1, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: Self.threads, height: 1, depth: 1))
                encoder.endEncoding()
            }
            paths.append(timing.path)
            let bytes = destination.contents().assumingMemoryBound(to: UInt8.self)
            outputs.append(Array(UnsafeBufferPointer(start: bytes, count: 64)))
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        let (aligned, shifted) = (outputs[0], outputs[1])
        let written = aligned.firstIndex { $0 != Self.sentinel } != nil
        result.expect(written, "the aligned dispatch wrote nothing")
        result.expect(
            Array(shifted[Self.offset..<64]) == Array(aligned[0..<(64 - Self.offset)]),
            "the shifted result differs from the aligned one moved by \(Self.offset) bytes")
        result.expect(
            shifted[0..<Self.offset].allSatisfy { $0 == Self.sentinel }, "bytes before the bind offset were written")
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(paths == ["raw", "raw"], "the aligned and shifted dispatches took \(paths), not raw")
        }
        return result
    }
}

/// A runtime sampler with Cubic min and mag filters (MTLSamplerMinMagFilterCubic, raw value 2: RenderBox's icon sampler,
/// which raw refused as call.sampler on boots 15 and 16), 4x magnified. A linear ramp comes out a linear ramp away from the
/// clamped edges, as every bicubic filter does; one bright texel comes out neither the linear filter's tent nor the nearest
/// filter's square (the program filters bicubically, it is no placeholder), and its picture is held to an Apple GPU's
/// (impulse).
struct CubicSamplerScene: GPUScene {
    let id = "gpu.raw.cubic_sampler"
    let name = "Samplers with cubic filters, magnified"

    static let source = 16
    static let target = 64

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let descriptor = MTLSamplerDescriptor()
        descriptor.minFilter = MTLSamplerMinMagFilter(rawValue: 2)!
        descriptor.magFilter = MTLSamplerMinMagFilter(rawValue: 2)!
        descriptor.sAddressMode = .clampToEdge
        descriptor.tAddressMode = .clampToEdge
        descriptor.lodAverage = true
        guard let sampler = gpu.device.makeSamplerState(descriptor: descriptor) else {
            result.expect(false, "makeSamplerState returned nil for a cubic sampler")
            return result
        }
        let pipeline = try gpu.render(vertex: "textured_vertex", fragment: "sampled_fragment", format: .rgba8Unorm)
        let (s, t) = (Self.source, Self.target)
        let ramp = Raster(width: s, height: s, channels: 4) { x, y, c in
            [Float(x) + 0.5, Float(y) + 0.5, 0, Float(s)][c] / Float(s)
        }
        let impulse = Raster(width: s, height: s, channels: 4) { x, y, c in
            c == 3 ? 1 : (x == s / 2 && y == s / 2 ? 1 : 0)
        }
        var paths: [String] = []
        var drawn: [Raster] = []
        for picture in [ramp, impulse] {
            let texture = try gpu.texture(.rgba8Unorm, s, s, usage: [.shaderRead])
            try await gpu.upload(picture.samples.map { UInt8(($0 * 255).rounded()) }, to: texture, bytesPerRow: s * 4)
            let out = try gpu.texture(.rgba8Unorm, t, t)
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = out
                pass.colorAttachments[0].loadAction = .dontCare
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var corners = GPU.fullScreen
                var uvs: [SIMD2<Float>] = [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)]
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
                encoder.setFragmentTexture(texture, index: 0)
                encoder.setFragmentSamplerState(sampler, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            paths.append(timing.path)
            drawn.append(try await gpu.raster(out, .rgba8Unorm))
        }
        let (rampOut, impulseOut) = (drawn[0], drawn[1])
        let inner = 8..<(t - 8)
        let rampWant = Raster(width: t, height: t, channels: 4) { x, y, c in
            [(Float(x) + 0.5) / Float(t), (Float(y) + 0.5) / Float(t), 0, 1][c]
        }
        var rampWrong = 0
        for y in inner {
            for x in inner where (0..<2).contains(where: { abs(rampOut[x, y, $0] - rampWant[x, y, $0]) > 2.01 / 255 }) {
                rampWrong += 1
            }
        }
        result.expect(rampWrong == 0, "the ramp is not a ramp at \(rampWrong) inner pixels")
        let (linear, nearest) = (Self.magnified(impulse, linear: true), Self.magnified(impulse, linear: false))
        let differs = { (other: Raster) in
            (0..<t).contains { y in (0..<t).contains { x in abs(impulseOut[x, y, 0] - other[x, y, 0]) > 4.01 / 255 } }
        }
        result.expect(
            differs(linear) && differs(nearest), "the impulse came out as a linear or nearest filter draws it")
        let bytes = impulseOut.samples.map { UInt8(($0 * 255).rounded()) }
        result.picture("impulse", bytes: bytes, metalFormat: MTLPixelFormat.rgba8Unorm.rawValue, width: t, height: t)
        result.metrics.set("paths", paths.joined(separator: ","))
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(paths.allSatisfy { $0 == "raw" }, "drawn on \(paths.joined(separator: ","))")
        }
        return result
    }

    /// `image`'s red channel 4x magnified through a clamp-to-edge linear or nearest filter, sampled at pixel centres
    static func magnified(_ image: Raster, linear: Bool) -> Raster {
        let (s, t) = (Float(source), Float(target))
        let at = { (x: Int, y: Int) in image[min(max(x, 0), source - 1), min(max(y, 0), source - 1), 0] }
        return Raster(width: target, height: target, channels: 1) { x, y, _ in
            let (u, v) = ((Float(x) + 0.5) / t * s - 0.5, (Float(y) + 0.5) / t * s - 0.5)
            guard linear else { return at(Int((u + 0.5).rounded(.down)), Int((v + 0.5).rounded(.down))) }
            let (x0, y0) = (Int(u.rounded(.down)), Int(v.rounded(.down)))
            let (fx, fy) = (u - Float(x0), v - Float(y0))
            let top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx
            let bottom = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx
            return top * (1 - fy) + bottom * fy
        }
    }
}
