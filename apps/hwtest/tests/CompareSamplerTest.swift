import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A runtime sampler with a compare function on the raw path (drivers/TegraGM20BMetal/raw/runtime/raw_sampler.h
/// compareHeader): one state samples a depth image through it by comparison and another plainly, in one pass, so each
/// draw takes the sampler header its state's use names (m2v makes the slot's sampler per use). Each texel is compared
/// against 0.5 under Less and GreaterEqual: 1 where reference OP texel holds, as Vulkan's Dref and Metal define it.
struct CompareSamplerTest: HWTest {
    let id = "gpu.raw.compare_sampler"
    let name = "A sampler's compare function on raw: compared and plain reads of one depth image through one sampler"
    let subsystem = Subsystem.metal

    static let side = 8
    static let reference: Float = 0.5

    static func depth(_ x: Int, _ y: Int) -> Float { (Float(x + side * y) + 0.5) / 64 }

    static func compared(_ function: MTLCompareFunction, _ texel: Float) -> Float {
        switch function {
        case .less: return reference < texel ? 1 : 0
        case .greaterEqual: return reference >= texel ? 1 : 0
        default: return -1
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let fill = try gpu.render(fragment: "compare_fill_depth", format: .r32Float, depth: .depth32Float)
        let depthWrite = MTLDepthStencilDescriptor()
        depthWrite.depthCompareFunction = .always
        depthWrite.isDepthWriteEnabled = true
        guard let writes = gpu.device.makeDepthStencilState(descriptor: depthWrite) else {
            throw TestFailure("no depth state")
        }
        let comparedState = try gpu.render(fragment: "compare_sample_compared", format: .r32Float)
        let plainState = try gpu.render(fragment: "compare_sample_plain", format: .r32Float)
        let depth = try gpu.texture(.depth32Float, Self.side, Self.side)
        let scratch = try gpu.texture(.r32Float, Self.side, Self.side)
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: [String] = []
        paths.append(
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = scratch
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .dontCare
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(fill)
                encoder.setDepthStencilState(writes)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }.path)
        for function in [MTLCompareFunction.less, .greaterEqual] {
            let descriptor = MTLSamplerDescriptor()
            descriptor.compareFunction = function
            guard let sampler = gpu.device.makeSamplerState(descriptor: descriptor) else {
                throw TestFailure("no sampler")
            }
            let comparedOut = try gpu.texture(.r32Float, Self.side, Self.side)
            let plainOut = try gpu.texture(.r32Float, Self.side, Self.side)
            paths.append(
                try await gpu.run { commands in
                    for (state, target) in [(comparedState, comparedOut), (plainState, plainOut)] {
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = target
                        pass.colorAttachments[0].loadAction = .clear
                        pass.colorAttachments[0].storeAction = .store
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        encoder.setRenderPipelineState(state)
                        var corners = GPU.fullScreen
                        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                        var reference = Self.reference
                        encoder.setFragmentBytes(&reference, length: MemoryLayout<Float>.stride, index: 0)
                        encoder.setFragmentTexture(depth, index: 0)
                        encoder.setFragmentSamplerState(sampler, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }
                }.path)
            let comparedBytes = try await gpu.read(comparedOut, bytesPerPixel: 4)
            let plainBytes = try await gpu.read(plainOut, bytesPerPixel: 4)
            var comparedOff = 0, plainWorst: Float = 0
            for y in 0..<Self.side {
                for x in 0..<Self.side {
                    let at = 4 * (x + Self.side * y)
                    let got = comparedBytes.withUnsafeBytes { $0.load(fromByteOffset: at, as: Float.self) }
                    let plain = plainBytes.withUnsafeBytes { $0.load(fromByteOffset: at, as: Float.self) }
                    comparedOff += got == Self.compared(function, Self.depth(x, y)) ? 0 : 1
                    plainWorst = max(plainWorst, abs(plain - Self.depth(x, y)))
                }
            }
            let label = function == .less ? "less" : "greater_equal"
            metrics.set("\(label)_compared_off", comparedOff)
            metrics.set("\(label)_plain_worst", Double(plainWorst), digits: 6)
            if comparedOff > 0 { wrong.append("\(label)_compared") }
            if plainWorst > 1e-6 { wrong.append("\(label)_plain") }
        }
        metrics.set("paths", paths.allSatisfy { $0 == "raw" } ? "raw" : paths.joined(separator: ","))
        if paths.contains(where: { $0 != "raw" }) { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
