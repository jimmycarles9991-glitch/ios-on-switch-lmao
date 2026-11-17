import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A texture over a buffer at offset 48 with rows of 48 bytes (16 mod 32, which the texture header cannot address) is the
/// raw path's: a draw samples it through a pool image that the copy engine fills from the buffer first
/// (drivers/TegraGM20BMetal/raw/runtime/raw_gpu.cpp createBufferTexture). Every texel drawn must be the buffer's. One at an
/// offset or with rows off 16, which Metal's validation stops, is nil, as is one over an IOSurface without planes whose rows
/// are off 16 (Metal asserts there) or over a plane the surface lacks. Nothing may ask for the m2v device (m2v_needs_asked).
struct BufferTexture16Test: HWTest {
    let id = "gpu.raw.buffer_texture16"
    let name = "A texture over a buffer at 16 bytes is sampled through a pool image, without m2v"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = OwnAnswersTest.needs(gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []

        let width = 12, height = 8, offset = 48, rowBytes = 48
        let buffer = try gpu.buffer(offset + rowBytes * height)
        let bytes = buffer.contents().bindMemory(to: UInt8.self, capacity: offset + rowBytes * height)
        for i in 0..<(rowBytes * height) { bytes[offset + i] = UInt8((i * 37 + 11) & 255) }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let texture = buffer.makeTexture(descriptor: descriptor, offset: offset, bytesPerRow: rowBytes) else {
            return .fail("no texture at offset 48", metrics)
        }
        if buffer.makeTexture(descriptor: descriptor, offset: offset + 8, bytesPerRow: rowBytes) != nil {
            wrong.append("offset_off16")
        }
        if buffer.makeTexture(descriptor: descriptor, offset: offset, bytesPerRow: rowBytes + 8) != nil {
            wrong.append("rows_off16")
        }

        let properties: [IOSurfacePropertyKey: Any] = [
            .width: 60, .height: 16, .bytesPerElement: 4, .bytesPerRow: 248, .pixelFormat: 0x4247_5241,
        ]
        guard let narrow = IOSurface(properties: properties) else { throw TestFailure("no IOSurface") }
        metrics.set("narrow_bytes_per_row", narrow.bytesPerRow)
        let surfaceDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: 60, height: 16, mipmapped: false)
        surfaceDescriptor.usage = [.shaderRead]
        let surface = unsafeBitCast(narrow, to: IOSurfaceRef.self)
        if narrow.bytesPerRow & 15 != 0
            && gpu.device.makeTexture(descriptor: surfaceDescriptor, iosurface: surface, plane: 0) != nil
        {
            wrong.append("surface_rows_off16")
        }
        if gpu.device.makeTexture(descriptor: surfaceDescriptor, iosurface: surface, plane: 1) != nil {
            wrong.append("surface_missing_plane")
        }

        let state = try gpu.render(vertex: "device_textured_vertex", fragment: "sampled_fragment", format: .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, width, height)
        guard let sampler = gpu.device.makeSamplerState(descriptor: MTLSamplerDescriptor()) else {
            throw TestFailure("no sampler")
        }
        let drawn = try SamplerTableTest.draw(gpu, state, texture, sampler, into: target)
        metrics.set("status", Int(drawn.status.rawValue))
        if drawn.status != .completed { wrong.append("draw") }
        let out = try await gpu.read(target, bytesPerPixel: 4)
        var mismatched = 0
        for y in 0..<height {
            for x in 0..<width {
                let texel = offset + y * rowBytes + x * 4, pixel = (y * width + x) * 4
                let expected = [bytes[texel + 2], bytes[texel + 1], bytes[texel], bytes[texel + 3]]
                if Array(out[pixel..<(pixel + 4)]) != expected { mismatched += 1 }
            }
        }
        metrics.set("wrong_texels", mismatched)
        if mismatched != 0 { wrong.append("texels") }

        let asked = OwnAnswersTest.needs(gpu.device).filter { $0.value > (before[$0.key] ?? 0) }.keys.sorted()
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
