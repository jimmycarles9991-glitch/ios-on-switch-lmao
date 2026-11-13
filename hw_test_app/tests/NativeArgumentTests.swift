import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Argument encoders of argument descriptors: the encoder is the device's own, its length and alignment and every
/// argument's place are Metal's (an M3 Max's and an M4's: a pointer, a texture, a sampler, a float and a float4 take
/// 0, 8, 16, 24 and 32 of 48 bytes aligned to 16), a buffer is written as its GPU address plus the offset, a texture and
/// a sampler as real resource IDs (nonzero, distinct, past every ID m2v hands out), and an array's elements at their own
/// places. Five passes read through argument buffers, each drawn on the raw path with the exact colours: a colour behind a
/// pointer; two textures and a sampler written by the fragment function's own encoder (its buffer index form), so a draw
/// that reads texture 0 for every ID cannot pass; an array of three textures sampled through an array of two samplers of
/// other address modes (NativeArgumentDraws.swift); one pointer that is a bound buffer or an argument buffer's by pixel;
/// a texture and a colour behind a pointer to an argument buffer that the nested encoder wrote; and fourteen fragment or
/// thirteen vertex buffers (and twenty-eight kernel buffers) bound beside an argument buffer, whose last offsets m2v places
/// in the stage's argument table (NativeArgumentSpills.swift).
struct NativeArgumentEncoderTest: NativeObjectTest {
    let id = "metal.native.argument_encoder"
    let name = "Argument encoders are the device's own and write addresses and real resource IDs where Metal lays them"

    static func argument(_ type: MTLDataType, _ index: Int, count: Int = 0) -> MTLArgumentDescriptor {
        let argument = MTLArgumentDescriptor()
        argument.dataType = type
        argument.index = index
        argument.arrayLength = count
        return argument
    }

    static func word(_ buffer: MTLBuffer, at offset: Int) -> UInt64 {
        buffer.contents().loadUnaligned(fromByteOffset: offset, as: UInt64.self)
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        guard
            let encoder = device.makeArgumentEncoder(arguments: [
                Self.argument(.pointer, 0), Self.argument(.texture, 1), Self.argument(.sampler, 2),
                Self.argument(.float, 3), Self.argument(.float4, 4),
            ]),
            let arrays = device.makeArgumentEncoder(arguments: [
                Self.argument(.texture, 0, count: 3), Self.argument(.pointer, 3, count: 2),
                Self.argument(.float, 5, count: 4), Self.argument(.sampler, 9, count: 2),
            ]),
            let sampler = device.makeSamplerState(descriptor: MTLSamplerDescriptor())
        else { throw TestFailure("no argument encoder or sampler") }
        let textures = try (0..<3).map { _ in try gpu.texture(.rgba8Unorm, 4, 4) }
        var work = NativeWork(objects: [encoder, arrays, sampler, textures[0]])
        metrics.set(
            "encoded", "\(encoder.encodedLength)/\(encoder.alignment),\(arrays.encodedLength)/\(arrays.alignment)")
        if encoder.encodedLength != 48 || encoder.alignment != 16 || arrays.encodedLength != 72 || arrays.alignment != 8
        {
            work.failures.append("an encoded length or alignment is not Metal's")
        }
        let arguments = try gpu.buffer(256), target = try gpu.buffer(1024)
        encoder.setArgumentBuffer(arguments, offset: 64)
        encoder.setBuffer(target, offset: 32, index: 0)
        encoder.setTexture(textures[0], index: 1)
        encoder.setSamplerState(sampler, index: 2)
        let texture = textures[0].gpuResourceID._impl, samplerID = sampler.gpuResourceID._impl
        metrics.set("texture_id", String(texture, radix: 16))
        metrics.set("sampler_id", String(samplerID, radix: 16))
        if Self.word(arguments, at: 64) != target.gpuAddress + 32 {
            work.failures.append("a buffer is not its address")
        }
        if Self.word(arguments, at: 72) != texture || texture < 1 << 32 {
            work.failures.append("a texture is not its real resource ID")
        }
        if Self.word(arguments, at: 80) != samplerID || samplerID < 1 << 32 {
            work.failures.append("a sampler is not its real resource ID")
        }
        if encoder.constantData(at: 4) != arguments.contents() + 64 + 32 {
            work.failures.append("a constant is not at its place")
        }
        if encoder.makeArgumentEncoderForBuffer(atIndex: 0) != nil {
            work.failures.append("a descriptor layout answered a nested encoder")
        }
        if let function = gpu.library.makeFunction(name: "argument_nested_fragment") {
            let outer = function.makeArgumentEncoder(bufferIndex: 0)
            work.objects.append(outer)
            if let inner = outer.makeArgumentEncoderForBuffer(atIndex: 0) {
                work.objects.append(inner)
            } else {
                work.failures.append("a function's pointer to an argument buffer answered no nested encoder")
            }
        }
        let ids = Set(textures.map { $0.gpuResourceID._impl })
        if ids.count != textures.count || ids.contains(0) { work.failures.append("textures share a resource ID") }
        let arrayBuffer = try gpu.buffer(256)
        arrays.setArgumentBuffer(arrayBuffer, offset: 0)
        arrays.setTextures(textures, range: 0..<3)
        arrays.setSamplerStates([sampler, sampler], range: 9..<11)
        let placed = [0, 8, 16, 56, 64].map { Self.word(arrayBuffer, at: $0) }
        if placed != textures.map({ $0.gpuResourceID._impl }) + [samplerID, samplerID] {
            work.failures.append("an array's elements are not at their places")
        }
        let draws: [(String, (GPU) async throws -> String)] = [
            ("draw_path", drawThroughArguments), ("texture_draw_path", drawTexturesThroughArguments),
            ("array_draw_path", drawArraysThroughArguments), ("slot_draw_path", drawSlotThroughArguments),
            ("kernel_path", dispatchThroughArguments),
            ("spill_draw_path", { try await drawSpillThroughArguments($0, .fragment) }),
            ("spill_vertex_draw_path", { try await drawSpillThroughArguments($0, .vertex) }),
            ("spill_both_draw_path", { try await drawSpillThroughArguments($0, .both) }),
            ("spill_kernel_path", dispatchSpillThroughArguments),
        ]
        for (key, draw) in draws {
            var path = "error"
            do {
                path = try await draw(gpu)
            } catch {
                metrics.set("draw_error", "\(error)".replacingOccurrences(of: " ", with: "_"))
            }
            metrics.set(key, path)
            if path != "raw/exact" { work.failures.append("\(key) \(path), not raw/exact") }
        }
        var nested = "error"
        do {
            nested = try await drawNestedThroughArguments(gpu, &metrics)
        } catch {
            metrics.set("draw_error", "\(error)".replacingOccurrences(of: " ", with: "_"))
        }
        metrics.set("nested_draw_path", nested)
        if nested != "raw/exact" { work.failures.append("nested_draw_path \(nested), not raw/exact") }
        await heapUseCost(gpu, &metrics, &work.failures)
        return work
    }

    /// Where a pass that samples two textures through an argument buffer the fragment function's encoder wrote went, and
    /// whether each half drew its own texture's colour
    func drawTexturesThroughArguments(_ gpu: GPU) async throws -> String {
        guard let function = gpu.library.makeFunction(name: "argument_textures_fragment"),
            let sampler = gpu.device.makeSamplerState(descriptor: MTLSamplerDescriptor())
        else { return "no_function" }
        let encoder = function.makeArgumentEncoder(bufferIndex: 0)
        let first = try gpu.texture(.rgba8Unorm, 4, 4), second = try gpu.texture(.rgba8Unorm, 4, 4)
        let arguments = try gpu.buffer(max(encoder.encodedLength, 16))
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setTexture(first, index: 0)
        encoder.setTexture(second, index: 1)
        encoder.setSamplerState(sampler, index: 2)
        let pipeline = try gpu.render(fragment: "argument_textures_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 8, 8)
        let drawn = try await gpu.run { commands in
            for (texture, colour) in [
                (first, MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)),
                (second, MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)),
            ] {
                let clear = MTLRenderPassDescriptor()
                clear.colorAttachments[0].texture = texture
                clear.colorAttachments[0].loadAction = .clear
                clear.colorAttachments[0].clearColor = colour
                commands.makeRenderCommandEncoder(descriptor: clear)!.endEncoding()
            }
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            let render = commands.makeRenderCommandEncoder(descriptor: pass)!
            render.setRenderPipelineState(pipeline)
            render.useResource(first, usage: .read, stages: .fragment)
            render.useResource(second, usage: .read, stages: .fragment)
            var corners = GPU.fullScreen
            render.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            render.setFragmentBuffer(arguments, offset: 0, index: 0)
            render.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            render.endEncoding()
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let exact =
            Array(pixels.pixel(1, 4, width: 8)) == [255, 0, 0, 255]
            && Array(pixels.pixel(6, 4, width: 8)) == [0, 0, 255, 255]
        return "\(drawn.path)/\(exact ? "exact" : "wrong")"
    }

    /// Where a pass that reads its colour through an argument buffer's pointer went, and whether it drew that colour
    func drawThroughArguments(_ gpu: GPU) async throws -> String {
        guard let encoder = gpu.device.makeArgumentEncoder(arguments: [Self.argument(.pointer, 0)]) else {
            return "no_encoder"
        }
        let colour = try gpu.buffer([SIMD4<Float>(1, 0, 1, 1)]), arguments = try gpu.buffer(16)
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setBuffer(colour, offset: 0, index: 0)
        let pipeline = try gpu.render(fragment: "argument_colour_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 8, 8)
        let drawn = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            let render = commands.makeRenderCommandEncoder(descriptor: pass)!
            render.setRenderPipelineState(pipeline)
            render.useResource(colour, usage: .read, stages: .fragment)
            var corners = GPU.fullScreen
            render.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            render.setFragmentBuffer(arguments, offset: 0, index: 0)
            render.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            render.endEncoding()
        }
        let pixel = Array(try await gpu.read(target, bytesPerPixel: 4).pixel(4, 4, width: 8))
        return "\(drawn.path)/\(pixel == [255, 0, 255, 255] ? "exact" : "wrong")"
    }
}
