import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The device's 2048 sampler header slots (drivers/TegraGM20BMetal/raw/runtime raw_gpu.cpp takeHeader). Samplers of one
/// descriptor share their compare form's slot, so 1500 of them all live. A draw sampling a converted '420f' texture
/// through a wrapping sampler takes one more header for the texture's address map. With every slot taken, a new sampler
/// is nil, and the draw's command buffer fails with MTLCommandBufferErrorOutOfMemory, as Metal does for a resource it
/// has no room for. Once the samplers are released, the same draw completes. Nothing may ask for the m2v device
/// (m2v_needs_asked names what did).
struct SamplerTableTest: HWTest {
    let id = "gpu.raw.sampler_table"
    let name = "Samplers share header slots; a full table is nil and an out-of-memory command buffer, never m2v"
    let subsystem = Subsystem.metal

    static let width = 64, height = 48

    static func wrapping(lodMax: Float) -> MTLSamplerDescriptor {
        let d = MTLSamplerDescriptor()
        d.sAddressMode = .repeat
        d.tAddressMode = .repeat
        d.minFilter = .linear
        d.magFilter = .linear
        d.mipFilter = .nearest
        d.lodMaxClamp = lodMax
        return d
    }

    static func distinct(_ i: Int) -> MTLSamplerDescriptor {
        let modes: [MTLSamplerAddressMode] = [.clampToEdge, .repeat, .mirrorRepeat, .clampToZero]
        let d = MTLSamplerDescriptor()
        d.sAddressMode = modes[i % 4]
        d.tAddressMode = modes[i % 4]
        d.mipFilter = .linear
        d.lodMaxClamp = 1 + Float(i / 4) / 64
        return d
    }

    /// A '420f' surface of mid-grey codes, and the converted texture Metal makes of it
    static func grey(_ device: MTLDevice) throws -> (CVPixelBuffer, MTLTexture) {
        var made: CVPixelBuffer?
        let attributes: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        CVPixelBufferCreate(
            nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attributes as CFDictionary, &made)
        guard let pixels = made, let surface = CVPixelBufferGetIOSurface(pixels)?.takeUnretainedValue() else {
            throw TestFailure("no '420f' IOSurface pixel buffer")
        }
        CVBufferSetAttachment(
            pixels, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
        CVPixelBufferLockBaseAddress(pixels, [])
        for plane in 0..<2 {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(pixels, plane) else { continue }
            memset(
                base, 128,
                CVPixelBufferGetBytesPerRowOfPlane(pixels, plane) * CVPixelBufferGetHeightOfPlane(pixels, plane))
        }
        CVPixelBufferUnlockBaseAddress(pixels, [])
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 500)!, width: width, height: height, mipmapped: false)
        descriptor.usage = .shaderRead
        guard let texture = device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no converted 4:2:0 texture")
        }
        return (pixels, texture)
    }

    static func draw(
        _ gpu: GPU, _ state: MTLRenderPipelineState, _ texture: MTLTexture, _ sampler: MTLSamplerState,
        into target: MTLTexture
    ) throws -> MTLCommandBuffer {
        let buffer = try gpu.commit { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            guard let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else {
                throw TestFailure("no render encoder")
            }
            let corners: [SIMD2<Float>] = [[-1, -1], [1, -1], [-1, 1], [1, 1]]
            let uvs: [Float] = [0, 1, 1, 1, 0, 0, 1, 0]
            encoder.setRenderPipelineState(state)
            encoder.setVertexBytes(corners, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 0)
            encoder.setVertexBytes(uvs, length: MemoryLayout<Float>.stride * 8, index: 1)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.setFragmentSamplerState(sampler, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        buffer.waitUntilCompleted()
        return buffer
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = OwnAnswersTest.needs(gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []

        var held = (0..<1500).compactMap { _ in gpu.device.makeSamplerState(descriptor: Self.distinct(1)) }
        metrics.set("one_descriptor_samplers", held.count)
        if held.count != 1500 { wrong.append("shared_compare_form") }

        let state = try gpu.render(
            vertex: "device_textured_vertex", fragment: "sampled_fragment", format: .bgra8Unorm)
        let (pixels, planar) = try Self.grey(gpu.device)
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        guard let first = gpu.device.makeSamplerState(descriptor: Self.wrapping(lodMax: 4)),
            let afterFull = gpu.device.makeSamplerState(descriptor: Self.wrapping(lodMax: 8))
        else { throw TestFailure("no wrapping sampler") }
        let control = try Self.draw(gpu, state, planar, first, into: target)
        metrics.set("control_status", Int(control.status.rawValue))
        if control.status != .completed { wrong.append("control_draw") }
        let at = (Self.height / 2 * Self.width + Self.width / 2) * 4
        let centre = Array(try await gpu.read(target, bytesPerPixel: 4)[at..<at + 4])
        metrics.set("control_bgra", centre.map(String.init).joined(separator: ","))
        if !zip(centre, [128, 128, 128, 255]).allSatisfy({ abs(Int($0) - $1) <= 2 }) {
            wrong.append("control_pixel")
        }

        for i in 0..<4096 {
            guard let s = gpu.device.makeSamplerState(descriptor: Self.distinct(i)) else { break }
            held.append(s)
        }
        while let s = gpu.device.makeSamplerState(descriptor: Self.distinct(0)) { held.append(s) }
        metrics.set("held_samplers", held.count)
        if gpu.device.makeSamplerState(descriptor: Self.wrapping(lodMax: 12)) != nil { wrong.append("table_not_full") }
        let full = try Self.draw(gpu, state, planar, afterFull, into: target)
        metrics.set("full_status", Int(full.status.rawValue))
        let fullError = (full.error as NSError?)?.code ?? 0
        metrics.set("full_error", fullError)
        if full.status != .error || fullError != Int(MTLCommandBufferError.Code.outOfMemory.rawValue) {
            wrong.append("full_table_draw")
        }

        held.removeAll()
        let freed = try Self.draw(gpu, state, planar, afterFull, into: target)
        metrics.set("freed_status", Int(freed.status.rawValue))
        if freed.status != .completed { wrong.append("freed_draw") }
        withExtendedLifetime(pixels) {}

        let asked = OwnAnswersTest.needs(gpu.device).filter { $0.value > (before[$0.key] ?? 0) }.keys.sorted()
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
