import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The features the device has no form of are NO wherever Metal lets a caller ask, and every creation past a NO fails:
/// an amplification count past the amplify macro's 8 mapping words. What both builds answer YES is made: a state of
/// amplification 8, one of a dynamic attribute stride (render and compute stage-in) and a post-tessellation state at
/// the device's maxTessellationFactor, 64 (metal.native.amplification, metal.native.dynamic_stride and
/// metal.native.tessellation draw them). Predicated loops nest as deep as the raw path lays them down
/// (raw_loop_copies.h LoopCopies::kMaxDepth, 8; gpu.raw.predication runs them). Linear textures align to the pitch
/// texture header's 32 bytes. The amplification count Metal defaults to, 1, draws on the raw path.
struct NativeUnsupportedTest: NativeObjectTest {
    let id = "metal.native.unsupported"
    let name = "Features the device has no form of answer NO and refuse their creations; amplification 1 draws raw"

    /// raw_loop_copies.h LoopCopies::kMaxDepth
    static let loopDepth = 8

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
        let nesting = Self.answer(device, "maxPredicatedNestingDepth")
        metrics.set("predicated_nesting", nesting ?? -1)
        if nesting != Self.loopDepth {
            work.failures.append("maxPredicatedNestingDepth is not the raw path's loop depth")
        }
        for (selector, want, field) in [
            ("supportsDynamicAttributeStride", 1, "dynamic_stride"),
            ("maxVertexAmplificationCount", NativeAnswers.amplificationCount, "max_amplification_count"),
            ("maxTessellationFactor", NativeAnswers.tessellationFactor, "max_tessellation_factor"),
        ] {
            if let wrong = NativeAnswers.check(device, selector, want, field: field, &metrics) {
                work.failures.append(wrong)
            }
        }
        let largest = NativeAnswers.largestAmplification(device)
        metrics.set("amplifications", largest)
        if largest != NativeAnswers.amplifications || !device.supportsVertexAmplificationCount(1) {
            work.failures.append("supportsVertexAmplificationCount: answers YES up to \(largest)")
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
        tessellated.maxTessellationFactor = NativeAnswers.tessellationFactor
        let amplified = MTLRenderPipelineDescriptor()
        amplified.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        amplified.fragmentFunction = gpu.library.makeFunction(name: "solid_fragment")
        amplified.colorAttachments[0].pixelFormat = .rgba8Unorm
        let strided = amplified.copy() as! MTLRenderPipelineDescriptor
        let pastMacro = amplified.copy() as! MTLRenderPipelineDescriptor
        amplified.maxVertexAmplificationCount = NativeAnswers.amplifications
        pastMacro.maxVertexAmplificationCount = NativeAnswers.amplifications + 1
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float4
        vertices.attributes[0].bufferIndex = 1
        vertices.layouts[1].stride = Int(bitPattern: UInt.max)  // MTLBufferLayoutStrideDynamic
        strided.vertexDescriptor = vertices
        var made: [String] = []
        for (what, descriptor) in [("tessellation64", tessellated), ("amplification8", amplified), ("stride", strided)]
        {
            if let error = Self.refused({
                work.objects.append(try device.makeRenderPipelineState(descriptor: descriptor))
            }) {
                work.failures.append("the \(what) state was refused: \(error)")
            } else {
                made.append(what)
            }
        }
        var refusals: [String] = []
        if let error = Self.refused({ _ = try device.makeRenderPipelineState(descriptor: pastMacro) }) {
            refusals.append("amplification9")
            metrics.set("amplification9_error", error)
        } else {
            work.failures.append("a state of amplification 9 was made")
        }
        let compute = MTLComputePipelineDescriptor()
        compute.computeFunction = gpu.library.makeFunction(name: "fma_loop")
        let stageIn = MTLStageInputOutputDescriptor()
        stageIn.attributes[0].format = .float4
        stageIn.attributes[0].bufferIndex = 2
        stageIn.layouts[2].stride = Int(bitPattern: UInt.max)
        compute.stageInputDescriptor = stageIn
        if let error = Self.refused({ _ = try device.makeComputePipelineState(descriptor: compute, options: []) }) {
            work.failures.append("the dynamic-stride stage-in state was refused: \(error)")
        } else {
            made.append("stage_in")
        }
        metrics.set("made", made.joined(separator: ","))
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
