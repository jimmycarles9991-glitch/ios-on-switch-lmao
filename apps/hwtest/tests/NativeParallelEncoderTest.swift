import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A parallel render encoder of three sub-encoders made in order A, B, C, encoded on three threads at once and ended
/// C, B, A, over a colour target and a memoryless depth attachment. A sets a scissor and draws red; B draws green at
/// the same depth with Metal's default scissor (all of it), so the depth A wrote keeps A's half red; C draws blue over
/// the bottom quarter with no depth test, last in creation order whatever order the sub-encoders ended in. A parallel
/// encoder of no sub-encoder still clears. Both command buffers stay raw. A group whose second sub-encoder clamps depth
/// stays raw too: its quad past the far plane is drawn, where the third's, which clips, is not.
struct NativeParallelEncoderTest: NativeObjectTest {
    let id = "metal.native.parallel_encoder"
    let name = "Parallel render encoders: sub-encoders in creation order, one pass, Metal's default state each"

    static let size = 64

    static func depthState(
        _ device: MTLDevice, _ compare: MTLCompareFunction, write: Bool
    ) throws -> MTLDepthStencilState {
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = compare
        descriptor.isDepthWriteEnabled = write
        guard let state = device.makeDepthStencilState(descriptor: descriptor) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        return state
    }

    static func want(_ x: Int, _ y: Int) -> [UInt8] {
        y >= size * 3 / 4 ? [0, 0, 255, 255] : x < size / 2 ? [255, 0, 0, 255] : [0, 255, 0, 255]
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let size = Self.size
        let device = gpu.device
        let color = try gpu.texture(.rgba8Unorm, size, size)
        let depth = try gpu.texture(.depth32Float, size, size, usage: [.renderTarget], storage: .memoryless)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float)
        let less = try Self.depthState(device, .less, write: true)
        let always = try Self.depthState(device, .always, write: false)
        var work = NativeWork(objects: [less, always])
        var parallelObjects: [AnyObject] = []
        let drawn = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .dontCare
            guard let parallel = commands.makeParallelRenderCommandEncoder(descriptor: pass) else {
                throw TestFailure("makeParallelRenderCommandEncoder returned nil")
            }
            parallelObjects.append(parallel)
            let subs = (0..<3).compactMap { _ in parallel.makeRenderCommandEncoder() }
            guard subs.count == 3 else { throw TestFailure("a sub-encoder was nil") }
            DispatchQueue.concurrentPerform(iterations: 3) { i in
                let encoder = subs[i]
                encoder.setRenderPipelineState(pipeline)
                switch i {
                case 0:
                    encoder.setDepthStencilState(less)
                    encoder.setScissorRect(MTLScissorRect(x: 0, y: 0, width: size / 2, height: size))
                    GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(1, 0, 0, 1))
                case 1:
                    encoder.setDepthStencilState(less)
                    GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0, 1, 0, 1))
                default:
                    encoder.setDepthStencilState(always)
                    let bottom = GPU.pixelQuad(0, size * 3 / 4, size, size, width: size, height: size)
                    GPU.draw(encoder, corners: bottom, color: SIMD4(0, 0, 1, 1))
                }
            }
            for encoder in subs.reversed() { encoder.endEncoding() }
            parallel.endEncoding()
        }
        work.objects += parallelObjects
        work.paths.append(drawn.path)
        let pixels = try await gpu.read(color, bytesPerPixel: 4)
        var wrong = 0
        for y in 0..<size {
            for x in 0..<size where Array(pixels.pixel(x, y, width: size)) != Self.want(x, y) { wrong += 1 }
        }
        metrics.set("wrong_pixels", wrong)
        if wrong != 0 {
            work.failures.append("\(wrong) pixels differ from creation order, the kept depth or the default scissor")
        }

        let cleared = try gpu.texture(.rgba8Unorm, 16, 16)
        let empty = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = cleared
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 1, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeParallelRenderCommandEncoder(descriptor: pass)?.endEncoding()
        }
        work.paths.append(empty.path)
        try await clampedGroup(gpu, &work, &metrics)
        let magenta = try await gpu.read(cleared, bytesPerPixel: 4)
        let unclear = (0..<256).filter {
            Array(magenta.pixel($0 % 16, $0 / 16, width: 16)) != [255, 0, 255, 255]
        }.count
        metrics.set("unclear_pixels", unclear)
        if unclear != 0 { work.failures.append("an empty parallel encoder left \(unclear) pixels uncleared") }
        return work
    }

    /// A draws red everywhere; B clamps depth and draws green past the far plane over the right half, drawn at the far
    /// plane; C clips (Metal's default each sub-encoder) and draws blue past the far plane over everything, clipped away
    func clampedGroup(_ gpu: GPU, _ work: inout NativeWork, _ metrics: inout Metrics) async throws {
        let size = 32
        let target = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let pastFar = GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 1.5)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            guard let parallel = commands.makeParallelRenderCommandEncoder(descriptor: pass),
                let a = parallel.makeRenderCommandEncoder(), let b = parallel.makeRenderCommandEncoder(),
                let c = parallel.makeRenderCommandEncoder()
            else { throw TestFailure("no parallel encoder or sub-encoder") }
            a.setRenderPipelineState(pipeline)
            GPU.draw(a, corners: GPU.fullScreen, color: SIMD4(1, 0, 0, 1))
            b.setRenderPipelineState(pipeline)
            b.setDepthClipMode(.clamp)
            b.setScissorRect(MTLScissorRect(x: size / 2, y: 0, width: size / 2, height: size))
            GPU.draw(b, corners: pastFar, color: SIMD4(0, 1, 0, 1))
            c.setRenderPipelineState(pipeline)
            GPU.draw(c, corners: pastFar, color: SIMD4(0, 0, 1, 1))
            c.endEncoding()
            b.endEncoding()
            a.endEncoding()
            parallel.endEncoding()
        }
        metrics.set("clamped_group_path", timing.path)
        if timing.path != "raw" { work.failures.append("the clamped group went \(timing.path)") }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = (0..<size * size).filter {
            let want: [UInt8] = $0 % size < size / 2 ? [255, 0, 0, 255] : [0, 255, 0, 255]
            return Array(pixels.pixel($0 % size, $0 / size, width: size)) != want
        }.count
        metrics.set("clamped_group_wrong", wrong)
        if wrong != 0 { work.failures.append("\(wrong) pixels wrong in the clamped group") }
    }
}
