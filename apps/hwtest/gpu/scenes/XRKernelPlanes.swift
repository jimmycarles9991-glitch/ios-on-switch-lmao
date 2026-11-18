import Foundation
import HWTestCore
import IOSurface
import Metal

/// RGB10A8_2P_XR10 over a 'b3a8' surface of Metal's two planes, 64 texels wide (rows of 256 and 64 bytes): plane 0 the
/// colour words, plane 1 the alpha bytes, as kernels reach them (gpu.raw.xr_kernel_store, gpu.raw.xr_kernel_read).
struct XRKernelPlanes {
    static let width = 64
    static let format = MTLPixelFormat(rawValue: 550)!

    let surface: IOSurface
    let texture: MTLTexture
    let height: Int

    init(_ gpu: GPU, texels: Int, usage: MTLTextureUsage) throws {
        let (w, h) = (Self.width, (texels + Self.width - 1) / Self.width)
        guard
            let surface = IOSurface(properties: [
                .width: w, .height: h, .pixelFormat: UInt32(0x6233_6138),  // 'b3a8'
                .allocSize: w * 5 * h,
                .planeInfo: [
                    [
                        IOSurfacePropertyKey.planeWidth: w, .planeHeight: h, .planeBytesPerElement: 4,
                        .planeBytesPerRow: w * 4, .planeOffset: 0, .planeSize: w * 4 * h,
                    ],
                    [
                        IOSurfacePropertyKey.planeWidth: w, .planeHeight: h, .planeBytesPerElement: 1,
                        .planeBytesPerRow: w, .planeOffset: w * 4 * h, .planeSize: w * h,
                    ],
                ],
            ])
        else { throw TestFailure("IOSurface creation failed") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: Self.format, width: w, height: h, mipmapped: false)
        descriptor.usage = usage
        descriptor.storageMode = .shared
        guard
            let texture = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { throw TestFailure("makeTexture(RGB10A8_2P_XR10, usage \(usage.rawValue)) over two planes returned nil") }
        self.surface = surface
        self.texture = texture
        self.height = h
    }

    /// Texel `i`'s word and byte
    func texel(_ i: Int) -> (word: UInt32, byte: UInt8) {
        surface.lock(options: .readOnly, seed: nil)
        defer { surface.unlock(options: .readOnly, seed: nil) }
        let words = surface.baseAddressOfPlane(at: 0).assumingMemoryBound(to: UInt32.self)
        let bytes = surface.baseAddressOfPlane(at: 1).assumingMemoryBound(to: UInt8.self)
        return (words[i], bytes[i])
    }

    /// Every texel's word and byte, `fill(i)` for texel `i`
    func plant(_ fill: (Int) -> (word: UInt32, byte: UInt8)) {
        surface.lock(options: [], seed: nil)
        defer { surface.unlock(options: [], seed: nil) }
        let words = surface.baseAddressOfPlane(at: 0).assumingMemoryBound(to: UInt32.self)
        let bytes = surface.baseAddressOfPlane(at: 1).assumingMemoryBound(to: UInt8.self)
        for i in 0..<(Self.width * height) {
            (words[i], bytes[i]) = fill(i)
        }
    }

    /// One dispatch of `pipeline` over every texel, one thread each, with `buffer` at index 0 and the texture at `slots`
    static func dispatch(
        _ gpu: GPU, _ pipeline: MTLComputePipelineState, _ planes: XRKernelPlanes, buffer: MTLBuffer, slots: [Int]
    ) async throws -> String {
        let timing = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            for slot in slots { encoder.setTexture(planes.texture, index: slot) }
            encoder.setBuffer(buffer, offset: 0, index: 0)
            encoder.dispatchThreadgroups(
                MTLSize(width: planes.height, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: width, height: 1, depth: 1))
            encoder.endEncoding()
        }
        return timing.path
    }
}
