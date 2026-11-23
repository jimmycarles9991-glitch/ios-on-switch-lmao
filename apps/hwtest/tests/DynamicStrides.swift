import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A render state of five vertex buffers, each of an MTLBufferLayoutStrideDynamic layout, bound at strides past the
/// GPU's 12-bit stride field (4112 to 8192 bytes, junk between the vertices): the position from buffer 1, red, green,
/// blue and alpha from buffers 2 to 5 at 0.2, 0.4, 0.6 and 1. Every pixel of the quad reads (51, 102, 153, 255); a
/// stride read wrong reads junk.
enum DynamicStrides {
    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct In {
        float2 position [[attribute(0)]];
        float r [[attribute(1)]];
        float g [[attribute(2)]];
        float b [[attribute(3)]];
        float a [[attribute(4)]];
    };
    struct Out { float4 position [[position]]; float4 colour; };
    vertex Out five_strides_vertex(In in [[stage_in]]) {
        return {float4(in.position, 0, 1), float4(in.r, in.g, in.b, in.a)};
    }
    fragment float4 five_strides_fragment(Out in [[stage_in]]) { return in.colour; }
    """
    static let strides = [4112, 4128, 4160, 4224, 8192]
    static let side = 16
    static let corners: [SIMD2<Float>] = [[-1, -1], [1, -1], [-1, 1], [1, 1]]

    /// The pixels off (51, 102, 153, 255) and the draw's command buffer's path
    static func run(_ gpu: GPU) async throws -> (wrong: Int, path: String) {
        let library = try await gpu.device.makeLibrary(source: source, options: nil)
        let vertices = MTLVertexDescriptor()
        for k in 0 ..< 5 {
            vertices.attributes[k].format = k == 0 ? .float2 : .float
            vertices.attributes[k].bufferIndex = k + 1
            vertices.layouts[k + 1].stride = Int(bitPattern: UInt.max) // MTLBufferLayoutStrideDynamic
        }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "five_strides_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "five_strides_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.vertexDescriptor = vertices
        let state = try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
        let values: [Float] = [0.2, 0.4, 0.6, 1]
        let buffers = try strides.enumerated().map { k, stride -> MTLBuffer in
            var floats = [Float](repeating: 1e6, count: stride / 4 * corners.count)
            for (v, corner) in corners.enumerated() {
                if k == 0 {
                    floats[v * stride / 4] = corner.x
                    floats[v * stride / 4 + 1] = corner.y
                } else {
                    floats[v * stride / 4] = values[k - 1]
                }
            }
            return try gpu.buffer(floats)
        }
        let target = try gpu.texture(.rgba8Unorm, side, side)
        let path = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            for (k, buffer) in buffers.enumerated() {
                encoder.setVertexBuffer(buffer, offset: 0, attributeStride: strides[k], index: k + 1)
            }
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }.path
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = (0 ..< side * side).filter { Array(pixels[4 * $0 ..< 4 * $0 + 4]) != [51, 102, 153, 255] }.count
        return (wrong, path)
    }
}
