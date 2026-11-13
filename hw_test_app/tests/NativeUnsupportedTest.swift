import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The features the device has no form of are NO wherever Metal lets a caller ask, and every creation past a NO fails:
/// no predication, and on 21H461's A10X one view of amplification and no dynamic attribute stride (render or compute
/// stage-in), which 24B119's A13 answers YES (metal.native.amplification, metal.native.dynamic_stride). A
/// post-tessellation vertex function reads as one, and a state of a maximum tessellation factor past the device's is
/// refused (metal.native.tessellation draws patches). Linear textures align to the pitch texture header's 32 bytes. The
/// amplification count Metal defaults to, 1, draws on the raw path.
struct NativeUnsupportedTest: NativeObjectTest {
    let id = "metal.native.unsupported"
    let name = "Features the device has no form of answer NO and refuse their creations; amplification 1 draws raw"

    static let patchSource = """
        #include <metal_stdlib>
        using namespace metal;
        struct ControlPoint { float4 position [[attribute(0)]]; };
        struct Patch { patch_control_point<ControlPoint> points; };
        [[patch(triangle, 3)]] vertex float4 tri_post(Patch patch [[stage_in]], float3 uvw [[position_in_patch]]) {
            return patch.points[0].position * uvw.x + patch.points[1].position * uvw.y +
                   patch.points[2].position * uvw.z;
        }
        fragment float4 tri_fragment() { return float4(1); }
        """

    /// What a device query answers through its selector (nil where the device has no method for it)
    static func answer(_ device: MTLDevice, _ selector: String) -> Int? {
        let object = device as AnyObject as! NSObject
        guard object.responds(to: NSSelectorFromString(selector)) else { return nil }
        return (object.value(forKey: selector) as? NSNumber)?.intValue
    }

    /// The error a creation answered, as "domain/code" (nil where it made its object)
    static func refused(_ make: () throws -> Void) -> String? {
        do {
            try make()
            return nil
        } catch {
            let error = error as NSError
            return "\(error.domain)/\(error.code)"
        }
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        // 24B119's A13 amplifies (supportsVertexAmplificationCount: 0 to 8, maxVertexAmplificationCount 2) and binds
        // dynamic strides; 21H461's A10X does neither
        let a13 = device.name.contains("A13")
        metrics.set("identity", a13 ? "A13" : "A10X")
        let zeroes =
            ["maxPredicatedNestingDepth"]
            + (a13 ? [] : ["supportsDynamicAttributeStride"])
        for selector in zeroes where Self.answer(device, selector) != 0 {
            work.failures.append("\(selector) answers \(Self.answer(device, selector).map(String.init) ?? "nothing")")
        }
        if a13 && Self.answer(device, "supportsDynamicAttributeStride") != 1 {
            work.failures.append("supportsDynamicAttributeStride is not the A13's YES")
        }
        if Self.answer(device, "maxVertexAmplificationCount") != (a13 ? 2 : 1) {
            work.failures.append("maxVertexAmplificationCount is not the target's")
        }
        let amplifiesAbove1 = (2...8).contains(where: device.supportsVertexAmplificationCount)
        if !device.supportsVertexAmplificationCount(1) || amplifiesAbove1 != a13
            || device.supportsVertexAmplificationCount(9)
        {
            work.failures.append("supportsVertexAmplificationCount: is not the target's")
        }
        let patchLibrary = try await device.makeLibrary(source: Self.patchSource, options: nil)
        guard let post = patchLibrary.makeFunction(name: "tri_post") else { throw TestFailure("no tri_post") }
        metrics.set("patch", "\(post.patchType.rawValue)x\(post.patchControlPointCount)")
        if post.patchType != .triangle || post.patchControlPointCount != 3 {
            work.failures.append("tri_post reads as patch \(post.patchType.rawValue) of \(post.patchControlPointCount)")
        }
        let tessellated = MTLRenderPipelineDescriptor()
        tessellated.vertexFunction = post
        tessellated.fragmentFunction = patchLibrary.makeFunction(name: "tri_fragment")
        tessellated.colorAttachments[0].pixelFormat = .rgba8Unorm
        tessellated.maxTessellationFactor = 64
        let pastFactor = (Self.answer(device, "maxTessellationFactor") ?? 0) < 64
        metrics.set("max_tessellation_factor", Self.answer(device, "maxTessellationFactor") ?? -1)
        let amplified = MTLRenderPipelineDescriptor()
        amplified.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        amplified.fragmentFunction = gpu.library.makeFunction(name: "solid_fragment")
        amplified.colorAttachments[0].pixelFormat = .rgba8Unorm
        let strided = amplified.copy() as! MTLRenderPipelineDescriptor
        amplified.maxVertexAmplificationCount = 2
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float4
        vertices.attributes[0].bufferIndex = 1
        vertices.layouts[1].stride = Int(bitPattern: UInt.max)  // MTLBufferLayoutStrideDynamic
        strided.vertexDescriptor = vertices
        var refusals: [String] = []
        let refusable =
            (pastFactor ? [("tessellation", tessellated)] : [])
            + (a13 ? [] : [("amplification", amplified), ("stride", strided)])
        for (what, descriptor) in refusable {
            if let error = Self.refused({ _ = try device.makeRenderPipelineState(descriptor: descriptor) }) {
                refusals.append(what)
                metrics.set("\(what)_error", error)
            } else {
                work.failures.append("a \(what) state was made")
            }
        }
        let compute = MTLComputePipelineDescriptor()
        compute.computeFunction = gpu.library.makeFunction(name: "fma_loop")
        let stageIn = MTLStageInputOutputDescriptor()
        stageIn.attributes[0].format = .float4
        stageIn.attributes[0].bufferIndex = 2
        stageIn.layouts[2].stride = Int(bitPattern: UInt.max)
        compute.stageInputDescriptor = stageIn
        if !a13, let error = Self.refused({ _ = try device.makeComputePipelineState(descriptor: compute, options: []) })
        {
            refusals.append("stage_in")
            metrics.set("stage_in_error", error)
        } else if !a13 {
            work.failures.append("a dynamic-stride stage-in state was made")
        }
        metrics.set("refused", refusals.joined(separator: ","))
        let alignments = [MTLPixelFormat.r8Unorm, .rgba8Unorm, .bgra8Unorm, .rgba16Float].map {
            device.minimumLinearTextureAlignment(for: $0)
        }
        metrics.set("linear_alignment", alignments.map(String.init).joined(separator: ","))
        if alignments.contains(where: { $0 < 32 }) {
            work.failures.append("a linear texture alignment below the pitch texture header's 32 bytes")
        }

        let size = 32
        let target = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexAmplificationCount(1, viewMappings: nil)
            GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0, 1, 0, 1))
            encoder.endEncoding()
        }
        work.paths.append(timing.path)
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = (0..<size * size).filter {
            Array(pixels.pixel($0 % size, $0 / size, width: size)) != [0, 255, 0, 255]
        }.count
        metrics.set("wrong_pixels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) pixels of the amplification-1 draw are wrong") }
        return work
    }
}
