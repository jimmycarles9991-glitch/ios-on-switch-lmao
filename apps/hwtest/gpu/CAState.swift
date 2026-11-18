import Foundation
import HWTestCore
import Metal
import simd

/// Core Animation's own render pipeline states, made in this process from QuartzCore's default.metallib exactly as
/// the corpus line records them. The raw facade looks up a state whose functions are one framework library's in
/// its pack, so these are the app's way onto the raw path's draws (the app's own shaders always go to m2v).
public enum CAState {
    /// The system's QuartzCore library; the Mac's goldens writers point it at the IPSW's.
    #if os(macOS)
        nonisolated(unsafe) public static var library = URL(
            fileURLWithPath: "/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib")
    #else
        nonisolated(unsafe) public static var library = URL(
            fileURLWithPath: "/System/Library/Frameworks/QuartzCore.framework/default.metallib")
    #endif

    /// float4 position, float2 two texture coordinates, half4 colour: CA's 48-byte vertex (a0 to a3, buffer 1).
    public struct Vertex {
        public var position: SIMD4<Float>
        public var texcoord0: SIMD2<Float>
        public var texcoord1: SIMD2<Float>
        public var color: SIMD4<Float16>
        public var pad: SIMD4<Float16> = .zero

        public init(_ x: Float, _ y: Float, u: Float, v: Float, color: SIMD4<Float16>) {
            position = SIMD4(x, y, 0, 1)
            texcoord0 = SIMD2(u, v)
            texcoord1 = SIMD2(u, v)
            self.color = color
        }
    }

    public static func pipeline(
        _ line: PipelineLine, library: MTLLibrary, device: MTLDevice
    ) throws
        -> MTLRenderPipelineState
    {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.label = line.label
        descriptor.vertexFunction = try function(line.vertex, library)
        descriptor.fragmentFunction = try function(line.fragment, library)
        descriptor.rasterSampleCount = line.samples
        descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClass(rawValue: line.topology) ?? .unspecified
        descriptor.depthAttachmentPixelFormat = MTLPixelFormat(rawValue: line.depthFormat) ?? .invalid
        descriptor.stencilAttachmentPixelFormat = MTLPixelFormat(rawValue: line.stencilFormat) ?? .invalid
        for color in line.colors {
            let f = color.fields
            let attachment = descriptor.colorAttachments[color.index]!
            attachment.pixelFormat = MTLPixelFormat(rawValue: f[0]) ?? .invalid
            attachment.writeMask = MTLColorWriteMask(rawValue: f[1])
            attachment.isBlendingEnabled = f[2] != 0
            attachment.sourceRGBBlendFactor = MTLBlendFactor(rawValue: f[3]) ?? .one
            attachment.destinationRGBBlendFactor = MTLBlendFactor(rawValue: f[4]) ?? .zero
            attachment.rgbBlendOperation = MTLBlendOperation(rawValue: f[5]) ?? .add
            attachment.sourceAlphaBlendFactor = MTLBlendFactor(rawValue: f[6]) ?? .one
            attachment.destinationAlphaBlendFactor = MTLBlendFactor(rawValue: f[7]) ?? .zero
            attachment.alphaBlendOperation = MTLBlendOperation(rawValue: f[8]) ?? .add
        }
        if !line.attributes.isEmpty {
            let vertices = MTLVertexDescriptor()
            for attribute in line.attributes {
                let f = attribute.fields
                vertices.attributes[attribute.index].format = MTLVertexFormat(rawValue: f[0]) ?? .invalid
                vertices.attributes[attribute.index].offset = Int(f[1])
                vertices.attributes[attribute.index].bufferIndex = Int(f[2])
                vertices.layouts[Int(f[2])].stride = Int(f[3])
                vertices.layouts[Int(f[2])].stepFunction = MTLVertexStepFunction(rawValue: f[4]) ?? .perVertex
                vertices.layouts[Int(f[2])].stepRate = Int(f[5])
            }
            descriptor.vertexDescriptor = vertices
        }
        return try device.makeRenderPipelineState(descriptor: descriptor)
    }

    static func function(_ function: PipelineLine.Function, _ library: MTLLibrary) throws -> MTLFunction {
        if function.constants.isEmpty {
            guard let made = library.makeFunction(name: function.name) else {
                throw TestFailure("QuartzCore has no function \(function.name)")
            }
            return made
        }
        let values = MTLFunctionConstantValues()
        for constant in function.constants {
            guard let type = MTLDataType(rawValue: constant.type) else {
                throw TestFailure("\(function.name): constant \(constant.name) of type \(constant.type)")
            }
            constant.bytes.withUnsafeBytes {
                values.setConstantValue($0.baseAddress!, type: type, withName: constant.name)
            }
        }
        return try library.makeFunction(name: function.name, constantValues: values)
    }

    /// A rectangle in clip space as CA's vertices (a strip of four), its texture coordinates spanning [0, 1].
    public static func rectangle(x0: Float, y0: Float, x1: Float, y1: Float, color: SIMD4<Float16>) -> [Vertex] {
        [
            Vertex(x0, y0, u: 0, v: 1, color: color), Vertex(x1, y0, u: 1, v: 1, color: color),
            Vertex(x0, y1, u: 0, v: 0, color: color), Vertex(x1, y1, u: 1, v: 0, color: color),
        ]
    }

    /// The bindings QuartzCore's vertex functions read (vertices at buffer 1, fixed_vert's float4x4 transform at
    /// buffer 2, column-major) with an identity transform, and zeros for every other buffer slot a function may read,
    /// as the raw pack's probe binds them.
    public static func bind(_ encoder: MTLRenderCommandEncoder, vertices: [Vertex], source: MTLTexture?) {
        var vertices = vertices
        var identity = matrix_identity_float4x4
        let zeros = [UInt8](repeating: 0, count: 256)
        encoder.setVertexBytes(&vertices, length: MemoryLayout<Vertex>.stride * vertices.count, index: 1)
        encoder.setVertexBytes(&identity, length: MemoryLayout<simd_float4x4>.size, index: 2)
        for slot in [0, 3] { encoder.setVertexBytes(zeros, length: zeros.count, index: slot) }
        for slot in 0..<4 { encoder.setFragmentBytes(zeros, length: zeros.count, index: slot) }
        if let source {
            encoder.setFragmentTexture(source, index: 0)
            encoder.setFragmentTexture(source, index: 1)
        }
    }
}
