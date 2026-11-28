import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// A surface plane at offset 388 with rows of 52 bytes, both off 16 (IOSurface makes a plane at any offset and rows that
/// cover it). Metal's GPU reads such a plane from its offset and rows rounded down to 16 (384, 48 apart), and the CPU's
/// replaceRegion and getBytes use the plane's own (m2v linear_plan.rs, measured on an M3 Max and an M4). The driver makes
/// the texture at that GPU layout and keeps the CPU rows apart (drivers/TegraGM20BMetal TegraGM20BMetalDevice.mm,
/// TegraGM20BMetalTexture.mm). A plane whose rows overlap once rounded (13 texels in rows of 60) is made, as an M4 Max
/// makes it, and getBytes reads it at its own rows (facade_texture_rows_test). No command buffer may fail (failed_cbs=).
struct PlaneRows16Test: HWTest {
    let id = "gpu.gm20b.plane_rows16"
    let name = "A surface plane off 16 is read at Metal's GPU layout and written at its own rows"
    let subsystem = Subsystem.metal

    static func plane(width: Int, offset: Int, rowBytes: Int) -> [IOSurfacePropertyKey: Any] {
        [
            .planeWidth: width, .planeHeight: 8, .planeBytesPerElement: 4, .planeBytesPerRow: rowBytes,
            .planeOffset: offset, .planeSize: rowBytes * 8,
        ]
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []

        let width = 12, height = 8
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .pixelFormat: 0x4247_5241, .allocSize: 4096,
            .planeInfo: [
                Self.plane(width: 12, offset: 0, rowBytes: 48), Self.plane(width: 12, offset: 388, rowBytes: 52),
                Self.plane(width: 13, offset: 1024, rowBytes: 60),
            ],
        ]
        guard let made = IOSurface(properties: properties), made.planeCount == 3 else {
            throw TestFailure("no three-plane IOSurface")
        }
        let surface = unsafeBitCast(made, to: IOSurfaceRef.self)
        let base = made.baseAddress.bindMemory(to: UInt8.self, capacity: 4096)
        for i in 0 ..< 4096 { base[i] = UInt8((i * 29 + 7) & 255) }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 1) else {
            return .fail("no texture over plane 1", metrics)
        }
        descriptor.width = 13
        if let overlapping = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 2) {
            var row = [UInt8](repeating: 0, count: 13 * 4)
            overlapping.getBytes(&row, bytesPerRow: 13 * 4, from: MTLRegionMake2D(0, 2, 13, 1), mipmapLevel: 0)
            if row != Array(UnsafeBufferPointer(start: base + 1024 + 2 * 60, count: 13 * 4)) {
                wrong.append("overlapping_cpu_rows")
            }
        } else {
            wrong.append("overlapping_rows")
        }

        let texel: [UInt8] = [1, 2, 3, 4]
        texture.replace(region: MTLRegionMake2D(5, 3, 1, 1), mipmapLevel: 0, withBytes: texel, bytesPerRow: 4)
        let written = 388 + 3 * 52 + 5 * 4
        if Array(UnsafeBufferPointer(start: base + written, count: 4)) != texel { wrong.append("cpu_rows") }
        var back = [UInt8](repeating: 0, count: 4)
        texture.getBytes(&back, bytesPerRow: 4, from: MTLRegionMake2D(5, 3, 1, 1), mipmapLevel: 0)
        if back != texel { wrong.append("cpu_read") }

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
        for y in 0 ..< height {
            for x in 0 ..< width {
                let at = 384 + y * 48 + x * 4, pixel = (y * width + x) * 4
                if Array(out[pixel ..< (pixel + 4)]) != Array(UnsafeBufferPointer(start: base + at, count: 4)) {
                    mismatched += 1
                }
            }
        }
        metrics.set("wrong_texels", mismatched)
        if mismatched != 0 { wrong.append("gpu_layout") }

        let failed = FailedCounts.judged(
            before: before, after: GM20BFacade.counters(of: gpu.device), own: gpu.paths, &metrics
        )
        if !failed.1 { wrong.append(failed.0) }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
