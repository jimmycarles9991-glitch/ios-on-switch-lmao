import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Vertex layout strides past what a fixed-function vertex stream fetches (SET_VERTEX_STREAM_A_FORMAT's 12 bits), which
/// Metal takes on both identities (the AGX drivers keep a bind's stride as 32 bits and check no layout's). One state
/// reads its positions 2052 bytes apart and its colours 4100 bytes apart, both pulled in the shader, and a tint 16 bytes
/// apart that fixed-function input fetches after them: two quads, each half its own colour. Where the device answers
/// dynamic attribute stride (24B119), a state of a dynamic position layout draws the left half at a bind stride of 16
/// and the right half at 4112 in one pass, the second from its stride twin. Every texel exact.
struct NativeWideStrideTest: NativeObjectTest {
    let id = "metal.native.wide_stride"
    let name = "Vertex layout strides past a vertex stream's 12 bits draw on the raw path"

    static let size = 32
    static let positionStride = 2052
    static let colourStride = 4100
    static let wideBind = 4112

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct In { float2 position [[attribute(0)]]; float4 colour [[attribute(1)]]; float4 tint [[attribute(2)]]; };
        struct Out { float4 position [[position]]; float4 colour [[flat]]; };
        vertex Out wide_vertex(In in [[stage_in]]) { return {float4(in.position, 0, 1), in.colour * in.tint}; }
        struct Plain { float2 position [[attribute(0)]]; };
        vertex Out dynamic_vertex(Plain in [[stage_in]], constant float4 &colour [[buffer(2)]]) {
            return {float4(in.position, 0, 1), colour};
        }
        fragment float4 wide_fragment(Out in [[stage_in]]) { return in.colour; }
        """

    static let quads: [(x0: Float, x1: Float, colour: SIMD4<Float>)] = [
        (-1, 0, SIMD4(1, 0, 0, 1)), (0, 1, SIMD4(0, 0, 1, 1)),
    ]

    /// `values` (each `count` floats) `stride` bytes apart, junk between them
    static func laidOut(_ values: [[Float]], stride: Int) -> [Float] {
        let floats = stride / 4
        var out = [Float](repeating: 1e6, count: floats * values.count)
        for (k, v) in values.enumerated() {
            for (c, x) in v.enumerated() { out[k * floats + c] = x }
        }
        return out
    }

    static func corners(_ x0: Float, _ x1: Float) -> [[Float]] {
        [[x0, -1], [x1, -1], [x0, 1], [x0, 1], [x1, -1], [x1, 1]]
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        try await drawStatic(gpu, library, &work, &metrics)
        let dynamic =
            (device as AnyObject as! NSObject).value(forKey: "supportsDynamicAttributeStride") as? Bool ?? false
        metrics.set("dynamic_stride", dynamic ? "yes" : "no")
        if dynamic { try await drawDynamic(gpu, library, &work, &metrics) }
        return work
    }

    func drawStatic(_ gpu: GPU, _ library: MTLLibrary, _ work: inout NativeWork, _ metrics: inout Metrics) async throws
    {
        let vertices = MTLVertexDescriptor()
        for (a, (format, buffer, stride)) in [
            (MTLVertexFormat.float2, 1, Self.positionStride), (.float4, 2, Self.colourStride), (.float4, 3, 16),
        ].enumerated() {
            vertices.attributes[a].format = format
            vertices.attributes[a].bufferIndex = buffer
            vertices.layouts[buffer].stride = stride
        }
        let pipeline = try await makeState(gpu, library, "wide_vertex", vertices)
        work.objects.append(pipeline)
        let positions = Self.quads.flatMap { Self.corners($0.x0, $0.x1) }
        let colours = Self.quads.flatMap { q in
            [[Float]](repeating: [q.colour.x, q.colour.y, q.colour.z, q.colour.w], count: 6)
        }
        let buffers = try [
            gpu.buffer(Self.laidOut(positions, stride: Self.positionStride)),
            gpu.buffer(Self.laidOut(colours, stride: Self.colourStride)),
            gpu.buffer([Float](repeating: 1, count: 4 * positions.count)),
        ]
        let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let timing = try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
            encoder.setRenderPipelineState(pipeline)
            for (k, b) in buffers.enumerated() { encoder.setVertexBuffer(b, offset: 0, index: k + 1) }
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: positions.count)
            encoder.endEncoding()
        }
        work.paths.append(timing.path)
        try await check(gpu, target, "static", &work, &metrics)
    }

    func drawDynamic(_ gpu: GPU, _ library: MTLLibrary, _ work: inout NativeWork, _ metrics: inout Metrics) async throws
    {
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float2
        vertices.attributes[0].bufferIndex = 1
        vertices.layouts[1].stride = Int(bitPattern: UInt.max)  // MTLBufferLayoutStrideDynamic
        let pipeline = try await makeState(gpu, library, "dynamic_vertex", vertices)
        work.objects.append(pipeline)
        let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        var binds: [(stride: Int, buffer: MTLBuffer, colour: SIMD4<Float>)] = []
        for (q, stride) in zip(Self.quads, [16, Self.wideBind]) {
            binds.append((stride, try gpu.buffer(Self.laidOut(Self.corners(q.x0, q.x1), stride: stride)), q.colour))
        }
        let timing = try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
            encoder.setRenderPipelineState(pipeline)
            for (stride, buffer, colour) in binds {
                var c = colour
                encoder.setVertexBuffer(buffer, offset: 0, attributeStride: stride, index: 1)
                encoder.setVertexBytes(&c, length: 16, index: 2)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
            }
            encoder.endEncoding()
        }
        work.paths.append(timing.path)
        try await check(gpu, target, "dynamic", &work, &metrics)
    }

    func makeState(
        _ gpu: GPU, _ library: MTLLibrary, _ vertex: String, _ vertices: MTLVertexDescriptor
    ) async throws
        -> MTLRenderPipelineState
    {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: vertex)
        descriptor.fragmentFunction = library.makeFunction(name: "wide_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.vertexDescriptor = vertices
        return try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
    }

    static func pass(_ target: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    func check(
        _ gpu: GPU, _ target: MTLTexture, _ form: String, _ work: inout NativeWork, _ metrics: inout Metrics
    )
        async throws
    {
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = (0..<Self.size * Self.size).filter {
            let x = $0 % Self.size
            let expected: [UInt8] = x < Self.size / 2 ? [255, 0, 0, 255] : [0, 0, 255, 255]
            return Array(pixels.pixel(x, $0 / Self.size, width: Self.size)) != expected
        }.count
        metrics.set("\(form)_wrong_pixels", wrong)
        if wrong != 0 { work.failures.append("\(form): \(wrong) of \(Self.size * Self.size) pixels wrong") }
    }
}
