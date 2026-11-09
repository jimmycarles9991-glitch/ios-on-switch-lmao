import Foundation
import HWTestCore
import Metal

/// A fragment function calling custom_fn, a visible function its pipeline links under that name, as RenderBox draws iOS
/// 27's icon effects: the linked function is made with MTLFunctionDescriptor.specializedName "custom_fn" and one uint
/// constant at index 1, and the state takes it through fragmentLinkedFunctions.privateFunctions. The two constant values
/// give two colours, so a state keyed without the linked function's constants draws one of them wrong.
struct LinkedFunctionScene: GPUScene {
    let id = "gpu.draw.linked_function"
    let name = "A linked function under its specialized name"

    static let size = 8

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        for (variant, want) in [(UInt32(0), [UInt8]([255, 0, 0, 255])), (2, [0, 0, 255, 255])] {
            let effect = MTLFunctionDescriptor()
            effect.name = "linked_effect_v1"
            effect.specializedName = "custom_fn"
            let values = MTLFunctionConstantValues()
            var value = variant
            values.setConstantValue(&value, type: .uint, index: 1)
            effect.constantValues = values
            let function = try await gpu.library.makeFunction(descriptor: effect)
            result.expect(function.name == "custom_fn", "the specialized function is named \(function.name)")
            let pipeline = try gpu.render(fragment: "linked_fragment", format: .rgba8Unorm) { descriptor in
                let linked = MTLLinkedFunctions()
                linked.privateFunctions = [function]
                descriptor.fragmentLinkedFunctions = linked
            }
            let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            let got = try await gpu.read(target, bytesPerPixel: 4)
            result.compare(
                "custom_fn variant \(variant)", bytes: got,
                expected: [[UInt8]](repeating: want, count: Self.size * Self.size).flatMap { $0 },
                format: .rgba8Unorm, width: Self.size, height: Self.size, tolerance: 0)
        }
        return result
    }
}
