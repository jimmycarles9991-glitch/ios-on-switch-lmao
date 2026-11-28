import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import Metal

/// What a gpu.gm20b.texture_shapes case checks of newTextureWithDescriptor (texture_shape.h,
/// test/device_shapes_test.cpp): each texture's route (raw's own, another object or nil), the bytes it gave back,
/// every made texture asked again after its work whether it is still raw's (twins_after), and every command buffer's
/// path (paths)
final class ShapeArms {
    let gpu: GPU
    private var metrics = Metrics()
    private var wrong: [String] = []
    private var made: [(String, MTLTexture)] = []

    init(_ gpu: GPU) {
        self.gpu = gpu
    }

    /// Why a case cannot run on `gpu`, nil where it can
    static func skip(_ gpu: GPU) -> Outcome? {
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        guard !GM20BFacade.counters(of: gpu.device).isEmpty else {
            return .skip("a release runtime: textures do not answer switchMetalObject")
        }
        return nil
    }

    private func answer(_ texture: MTLTexture?) -> String {
        texture.map { NativeObject.name($0) == nil ? "other" : "gm20b" } ?? "nil"
    }

    func route(_ label: String, _ texture: MTLTexture?, expect: String) {
        metrics.set("route_\(label)", answer(texture))
        if answer(texture) != expect { wrong.append("route_\(label)") }
        if let texture { made.append((label, texture)) }
    }

    func off(_ label: String, _ got: [UInt8], _ want: [UInt8]) {
        let count = got.count == want.count ? zip(got, want).filter { $0 != $1 }.count : max(got.count, want.count)
        metrics.set("off_\(label)", count)
        if count > 0 { wrong.append("off_\(label)") }
    }

    func set(_ key: String, _ value: Int) {
        metrics.set(key, value)
    }

    func outcome() -> Outcome {
        let twins = made.filter { answer($0.1) != "gm20b" }.map(\.0)
        metrics.set("twins_after", twins.isEmpty ? "-" : twins.joined(separator: "/"))
        if !twins.isEmpty { wrong.append("twins_after") }
        metrics.set("paths", gpu.paths.text)
        if gpu.paths.paths.keys.contains(where: { $0 != "gm20b" }) { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }

    func make(
        _ format: MTLPixelFormat, _ type: MTLTextureType, _ width: Int, _ height: Int, depth: Int = 1, levels: Int = 1,
        samples: Int = 1, array: Int = 1, storage: MTLStorageMode = .private, usage: MTLTextureUsage
    ) -> MTLTexture? {
        let d = MTLTextureDescriptor()
        d.textureType = type
        d.pixelFormat = format
        d.width = width
        d.height = height
        d.depth = depth
        d.mipmapLevelCount = levels
        d.sampleCount = samples
        d.arrayLength = array
        d.storageMode = storage
        d.usage = usage
        return gpu.device.makeTexture(descriptor: d)
    }

    /// The bytes replaceRegion put in that getBytes did not give back (empty when they all came back): `rows` rows of
    /// texels, or of blocks for a block-compressed texture, in `plane` of a 3D one
    func cpuRoundTrip(
        _ texture: MTLTexture, width: Int, height: Int, bytesPerRow: Int, rows: Int, level: Int = 0, slice: Int = 0,
        plane: Int = 0
    ) throws -> [UInt8] {
        let put = (0 ..< bytesPerRow * rows).map { UInt8(truncatingIfNeeded: $0 * 7 + 3) }
        let region = MTLRegionMake3D(0, 0, plane, width, height, 1)
        let image = texture.textureType == .type3D ? put.count : 0
        texture.replace(
            region: region, mipmapLevel: level, slice: slice, withBytes: put, bytesPerRow: bytesPerRow,
            bytesPerImage: image
        )
        var got = [UInt8](repeating: 0, count: put.count)
        texture.getBytes(
            &got, bytesPerRow: bytesPerRow, bytesPerImage: image, from: region, mipmapLevel: level, slice: slice
        )
        return zip(put, got).compactMap { $0 == $1 ? nil : $1 }
    }

    /// A `width` x 1 target's texels after a draw of `fragment` (TextureViews.metal, TextureShapes.metal) reads
    /// `texture`
    func drawn(
        _ texture: MTLTexture, fragment: String, width: Int, format: MTLPixelFormat = .rgba8Unorm
    ) async throws -> [UInt8] {
        let target = try gpu.texture(format, width, 1)
        let state = try gpu.render(fragment: fragment, format: format)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        return try await gpu.read(target, bytesPerPixel: format == .rgba32Float ? 16 : format == .rgba16Float ? 8 : 4)
    }

    /// The 4x4 texture's texels as a draw samples them (opaque_border_fragment's middle of a 12x12 RGBA8 target)
    func sampled(_ texture: MTLTexture) async throws -> [UInt8] {
        let state = try gpu.render(fragment: "opaque_border_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 12, 12)
        guard let sampler = gpu.device.makeSamplerState(descriptor: MTLSamplerDescriptor()) else {
            throw TestFailure("no sampler")
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.setFragmentSamplerState(sampler, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        return (4 ..< 8).flatMap { y in (4 ..< 8).flatMap { x in bytes[4 * (x + 12 * y) ..< 4 * (x + 12 * y) + 4] } }
    }
}
