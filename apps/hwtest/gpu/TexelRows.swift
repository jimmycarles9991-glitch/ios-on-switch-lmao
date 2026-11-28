import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A BGRA8Unorm texel buffer over a 'BGRA' 64x16 of rows 320 (64 bytes of padding a row) that a BGRA8 target drew
/// into first, which on the device makes the kernel lay the surface out block-linear (gpu.gm20b.texel_rows). Metal makes
/// such a buffer up to one row of the surface, 80 texels here (an M4 Max asserts "bytesPerRow (320) must be greater or
/// equal to (324)" at 81), and reads and writes its bytes in order: element k is texel (k, 0) for k < 64 and row 0's
/// padding past it, which reads 0 while nothing wrote it and which no 2D texel shows.
public enum TexelRows {
    public static let texels = 80

    public struct Reading: Sendable {
        public var made = false
        public var paths: [String] = []
        /// Texels off at each step: the buffer's reads of row 0 and of its padding; row 0 of the 2D target after a
        /// kernel store, a fragment store, a pass into the buffer and replaceRegion; getBytes of the buffer; the rows
        /// the buffer does not reach after every step
        public var readsOff = -1, paddingOff = -1, kernelOff = -1, fragmentOff = -1, attachmentOff = -1
        public var getOff = -1, replaceOff = -1, outsideOff = -1
    }

    static func rgba(_ r: Int, _ g: Int, _ b: Int, _ a: Int) -> SIMD4<Float> {
        SIMD4(Float(r), Float(g), Float(b), Float(a)) / 255
    }

    /// texel_rows_colours at (x, y), texel_rows_kernel_store's and texel_rows_fragment_store's element k
    static func drawn(_ x: Int, _ y: Int) -> SIMD4<Float> { rgba(4 * x, 16 * y, 128, 255) }
    static func kernelStored(_ k: Int) -> SIMD4<Float> { rgba(k & 255, 90, (3 * k) & 255, 255) }
    static func fragmentStored(_ k: Int) -> SIMD4<Float> { rgba((k + 1) & 255, 165, 7, 255) }
    static let green = rgba(0, 255, 0, 255)

    static func off(_ got: ArraySlice<SIMD4<Float>>, _ want: (Int) -> SIMD4<Float>) -> Int {
        got.indices.filter { i in (0 ..< 4).contains { abs(got[i][$0] - want(i)[$0]) > 0.5 / 255 } }.count
    }

    public static func observe(_ gpu: GPU) async throws -> Reading {
        var r = Reading()
        guard let surface = SurfaceReads.surface("BGRA", 64, 16, rows: 320, allocation: 320 * 16, fill: { _ in 0 }),
              let target = SurfaceReads.texture(
                  gpu, MTLPixelFormat.bgra8Unorm.rawValue, 64, 16, over: surface, usage: [.renderTarget, .shaderRead]
              )
        else { throw TestFailure("no BGRA8 target over a 'BGRA' 64x16 of rows 320") }
        let red = MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)
        r.paths.append(try await RowsUnder16.pass(gpu, target, clear: red, fragment: "texel_rows_colours"))
        let descriptor = MTLTextureDescriptor.textureBufferDescriptor(
            with: .bgra8Unorm, width: texels, resourceOptions: [], usage: [.shaderRead, .shaderWrite, .renderTarget]
        )
        guard let buffer = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return r
        }
        let line = (MTLSize(width: texels, height: 1, depth: 1), MTLSize(width: 16, height: 1, depth: 1))
        r.made = true
        var row = Array(repeating: SIMD4<Float>(), count: 64)

        let out = try gpu.buffer(texels * 16)
        let read = try gpu.compute("read_texture_buffer")
        r.paths.append(try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(read)
            encoder.setTexture(buffer, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(line.0, threadsPerThreadgroup: line.1)
            encoder.endEncoding()
        }.facadePath)
        let read4 = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: texels)
        let elements = Array(UnsafeBufferPointer(start: read4, count: texels))
        r.readsOff = off(elements[0 ..< 64]) { drawn($0, 0) }
        r.paddingOff = off(elements[64 ..< texels]) { _ in SIMD4() }

        let store = try gpu.compute("texel_rows_kernel_store")
        r.paths.append(try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(store)
            encoder.setTexture(buffer, index: 0)
            encoder.dispatchThreads(line.0, threadsPerThreadgroup: line.1)
            encoder.endEncoding()
        }.facadePath)
        var image = try await SurfaceReads.read(gpu, target)
        r.kernelOff = off(image[0 ..< 64], kernelStored)

        let scratch = try gpu.texture(.bgra8Unorm, texels, 1)
        let fragment = try gpu.render(fragment: "texel_rows_fragment_store", format: .bgra8Unorm)
        r.paths.append(
            try await draw(gpu, into: scratch, fragment, load: false) { $0.setFragmentTexture(buffer, index: 0) }
        )
        image = try await SurfaceReads.read(gpu, target)
        r.fragmentOff = off(image[0 ..< 64], fragmentStored)

        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        var colour = green
        r.paths.append(try await draw(gpu, into: buffer, solid, load: true, x: (-0.5, 0.5)) {
            $0.setFragmentBytes(&colour, length: 16, index: 0)
        })
        image = try await SurfaceReads.read(gpu, target)
        r.attachmentOff = off(image[0 ..< 64]) { (20 ..< 60).contains($0) ? green : fragmentStored($0) }

        for x in 0 ..< 64 { row[x] = image[x] }
        var got = [UInt8](repeating: 0, count: 50 * 4)
        got.withUnsafeMutableBytes {
            buffer.getBytes($0.baseAddress!, bytesPerRow: 200, from: MTLRegionMake1D(10, 50), mipmapLevel: 0)
        }
        let bgra = { (k: Int) in rgba(Int(got[4 * k + 2]), Int(got[4 * k + 1]), Int(got[4 * k]), Int(got[4 * k + 3])) }
        r.getOff = off((0 ..< 50).map(bgra)[...]) { row[10 + $0] }

        let pattern = (0 ..< 200).map { UInt8(truncatingIfNeeded: 11 * $0 + 3) }
        pattern.withUnsafeBytes {
            let region = MTLRegionMake1D(10, 50)
            buffer.replace(region: region, mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 200)
        }
        image = try await SurfaceReads.read(gpu, target)
        r.replaceOff = off(image[0 ..< 64]) { x in
            guard (10 ..< 60).contains(x) else { return row[x] }
            let k = 4 * (x - 10)
            return rgba(Int(pattern[k + 2]), Int(pattern[k + 1]), Int(pattern[k]), Int(pattern[k + 3]))
        }
        r.outsideOff = off(image[64...]) { drawn($0 % 64, $0 / 64) }
        return r
    }

    /// One pass into `target` of a quad across `x` (clip space) and the whole height, its pipeline `state`
    static func draw(
        _ gpu: GPU, into target: MTLTexture, _ state: MTLRenderPipelineState, load: Bool, x: (Float, Float) = (-1, 1),
        bind: (MTLRenderCommandEncoder) -> Void
    ) async throws -> String {
        try await gpu.run(tolerateError: true) { commands in
            let descriptor = MTLRenderPassDescriptor()
            descriptor.colorAttachments[0].texture = target
            descriptor.colorAttachments[0].loadAction = load ? .load : .clear
            descriptor.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
            var corners = GPU.quad(x0: x.0, y0: -1, x1: x.1, y1: 1, z: 0.5)
            encoder.setRenderPipelineState(state)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            bind(encoder)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }.facadePath
    }
}
