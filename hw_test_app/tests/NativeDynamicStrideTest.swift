import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Dynamic attribute stride where the device answers it (24B119's A13; 21H461's A10X answers NO and refuses the state):
/// one state of an MTLBufferLayoutStrideDynamic layout draws a full-target quad from the same four corners laid out at
/// three strides, bound by setVertexBuffer:offset:attributeStride:atIndex: at 16 and 32 bytes (junk between the
/// corners) and by setVertexBytes:length:attributeStride:atIndex: at 8. Each draw covers the target; a stride read
/// wrong would not.
struct NativeDynamicStrideTest: NativeObjectTest {
    let id = "metal.native.dynamic_stride"
    let name = "A dynamic-stride vertex layout reads each bind's attributeStride on the raw path"

    static let size = 32

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct In { float2 position [[attribute(0)]]; };
        vertex float4 strided_quad(In in [[stage_in]]) { return float4(in.position, 0, 1); }
        fragment float4 strided_green() { return float4(0, 1, 0, 1); }
        """

    static let corners: [SIMD2<Float>] = [SIMD2(-1, -1), SIMD2(1, -1), SIMD2(-1, 1), SIMD2(1, 1)]

    /// The corners `stride` bytes apart, junk between them
    static func laidOut(_ stride: Int) -> [Float] {
        let floats = stride / 4
        var out = [Float](repeating: 1e6, count: floats * corners.count)
        for (k, c) in corners.enumerated() {
            out[k * floats] = c.x
            out[k * floats + 1] = c.y
        }
        return out
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float2
        vertices.attributes[0].bufferIndex = 1
        vertices.layouts[1].stride = Int(bitPattern: UInt.max)  // MTLBufferLayoutStrideDynamic
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "strided_quad")
        descriptor.fragmentFunction = library.makeFunction(name: "strided_green")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.vertexDescriptor = vertices
        let dynamic =
            (device as AnyObject as! NSObject).value(forKey: "supportsDynamicAttributeStride") as? Bool ?? false
        metrics.set("dynamic_stride", dynamic ? "yes" : "no")
        guard dynamic else {
            if (try? await device.makeRenderPipelineState(descriptor: descriptor)) != nil {
                work.failures.append("a dynamic-stride state was made where the device answers it NO")
            }
            return work
        }
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        work.objects.append(pipeline)
        for stride in [16, 32, 8] {
            let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
            let bytes = Self.laidOut(stride)
            let buffer = try gpu.buffer(bytes)
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                if stride == 8 {
                    var inline = bytes
                    encoder.setVertexBytes(&inline, length: inline.count * 4, attributeStride: stride, index: 1)
                } else {
                    encoder.setVertexBuffer(buffer, offset: 0, attributeStride: stride, index: 1)
                }
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            work.paths.append(timing.path)
            let pixels = try await gpu.read(target, bytesPerPixel: 4)
            let wrong = (0..<Self.size * Self.size).filter {
                Array(pixels.pixel($0 % Self.size, $0 / Self.size, width: Self.size)) != [0, 255, 0, 255]
            }.count
            metrics.set("stride\(stride)_wrong_pixels", wrong)
            if wrong != 0 { work.failures.append("stride \(stride): \(wrong) pixels wrong") }
        }
        return work
    }
}
