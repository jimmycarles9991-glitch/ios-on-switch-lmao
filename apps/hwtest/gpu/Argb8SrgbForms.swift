import Metal

/// ARGB8Unorm_sRGB (85) against RGBA8Unorm_sRGB (71) with its channels rotated (85's bytes hold alpha, red, green,
/// blue), on the GPU that runs it: every code of every channel read by a kernel, sampled linearly between neighbours,
/// written from floats by a pass and by a kernel. m2v names 85 but holds none of it (metal/format/mod.rs
/// formats_named_not_held) until these are measured on Apple's GPU: every field 0 on an M4 Max (host_gpu_tests
/// Argb8SrgbFormTests) is what holding it as R8G8B8A8_SRGB under ARGB8Unorm's rotated view needs.
public enum Argb8SrgbForms {
    public struct Reading: Sendable {
        public var readsOff = -1, linearOff = -1, passWritesOff = -1, kernelWritesOff = -1
        public var first = "-"

        public var line: String {
            "reads_off=\(readsOff) linear_off=\(linearOff) pass_writes_off=\(passWritesOff) "
                + "kernel_writes_off=\(kernelWritesOff) first=\(first)"
        }
    }

    static let width = 256
    static let argb8Srgb = MTLPixelFormat(rawValue: 85)!

    /// Texel i's red, green, blue and alpha codes: each channel runs through every code
    static func codes(_ i: Int) -> [UInt8] {
        [UInt8(i), UInt8(255 - i), UInt8((i * 37 + 11) & 255), UInt8((i * 101 + 3) & 255)]
    }

    /// Texel i's floats for the writes: each channel steps across every code and lands between two of them
    static func values(_ i: Int) -> SIMD4<Float> {
        SIMD4(Float(i) / 256 + 1 / 1024, Float(255 - i) / 255, Float((i * 37 + 11) & 255) / 256, Float(i % 7) / 6)
    }

    static func rotated(_ rgba: [UInt8]) -> [UInt8] { [rgba[3], rgba[0], rgba[1], rgba[2]] }

    public static func measure(_ gpu: GPU) async throws -> Reading {
        var r = Reading()
        let usage: MTLTextureUsage = [.shaderRead, .renderTarget, .shaderWrite]
        let srgb = try gpu.texture(.rgba8Unorm_srgb, width, 1, usage: usage, storage: .shared)
        let argb = try gpu.texture(argb8Srgb, width, 1, usage: usage, storage: .shared)
        let region = MTLRegionMake2D(0, 0, width, 1)
        let rgbaBytes = (0 ..< width).flatMap(codes)
        srgb.replace(region: region, mipmapLevel: 0, withBytes: rgbaBytes, bytesPerRow: width * 4)
        argb.replace(
            region: region, mipmapLevel: 0, withBytes: (0 ..< width).flatMap { rotated(codes($0)) },
            bytesPerRow: width * 4
        )
        let reads = (try await gpu.texels(srgb), try await gpu.texels(argb))
        r.readsOff = off(reads.0, reads.1, "read", &r.first)
        let linear = (try await sampled(gpu, srgb), try await sampled(gpu, argb))
        r.linearOff = off(linear.0, linear.1, "linear", &r.first)
        let passes = (try await passWritten(gpu, .rgba8Unorm_srgb), try await passWritten(gpu, argb8Srgb))
        r.passWritesOff = bytesOff(passes.0, passes.1, "pass", &r.first)
        let kernels = (try await kernelWritten(gpu, .rgba8Unorm_srgb), try await kernelWritten(gpu, argb8Srgb))
        r.kernelWritesOff = bytesOff(kernels.0, kernels.1, "kernel", &r.first)
        return r
    }

    static func off(_ a: [SIMD4<Float>], _ b: [SIMD4<Float>], _ what: String, _ first: inout String) -> Int {
        let differ = a.indices.filter { a[$0] != b[$0] }
        if first == "-", let i = differ.first { first = "\(what)_\(i)_\(a[i])_\(b[i])" }
        return differ.count
    }

    static func bytesOff(_ rgba: [UInt8], _ argb: [UInt8], _ what: String, _ first: inout String) -> Int {
        let differ = (0 ..< width).filter { i in
            rotated(Array(rgba[i * 4 ..< i * 4 + 4])) != Array(argb[i * 4 ..< i * 4 + 4])
        }
        if first == "-", let i = differ.first {
            first = "\(what)_\(i)_\(Array(rgba[i * 4 ..< i * 4 + 4]))_\(Array(argb[i * 4 ..< i * 4 + 4]))"
        }
        return differ.count
    }

    /// sample_texture_linear: each texel sampled a quarter of the way into its left neighbour
    static func sampled(_ gpu: GPU, _ texture: MTLTexture) async throws -> [SIMD4<Float>] {
        let out = try gpu.buffer(width * 16)
        let pipeline = try gpu.compute("sample_texture_linear")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: width, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(
                    width: 64,
                    height: 1,
                    depth: 1
                )
            )
            encoder.endEncoding()
        }
        return Array(UnsafeBufferPointer(start: out.contents().bindMemory(to: SIMD4<Float>.self, capacity: width),
                                         count: width))
    }

    /// texel_fragment over a float source of values(_:), into a target of `format`, as its bytes
    static func passWritten(_ gpu: GPU, _ format: MTLPixelFormat) async throws -> [UInt8] {
        let source = try gpu.texture(.rgba32Float, width, 1, usage: [.shaderRead], storage: .shared)
        let floats = (0 ..< width).map(values)
        floats.withUnsafeBytes {
            source.replace(region: MTLRegionMake2D(0, 0, width, 1), mipmapLevel: 0, withBytes: $0.baseAddress!,
                           bytesPerRow: width * 16)
        }
        let target = try gpu.texture(format, width, 1, usage: [.renderTarget, .shaderRead], storage: .shared)
        let state = try gpu.render(fragment: "texel_fragment", format: format)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(source, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        return try await gpu.read(target, bytesPerPixel: 4)
    }

    /// xr_kernel_store of values(_:) into a texture of `format`, as its bytes
    static func kernelWritten(_ gpu: GPU, _ format: MTLPixelFormat) async throws -> [UInt8] {
        let target = try gpu.texture(format, width, 1, usage: [.shaderWrite, .shaderRead], storage: .shared)
        let data = try gpu.buffer((0 ..< width).map(values))
        let pipeline = try gpu.compute("xr_kernel_store")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(target, index: 0)
            encoder.setBuffer(data, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: width, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(
                    width: 64,
                    height: 1,
                    depth: 1
                )
            )
            encoder.endEncoding()
        }
        return try await gpu.read(target, bytesPerPixel: 4)
    }
}
