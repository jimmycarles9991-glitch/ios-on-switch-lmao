import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A compute kernel's store into a converted YCbCr view (R10Unorm_PACKED 588 over a 'p420' surface's luma plane,
/// YCBCR8_422_1P 501 over a '2vuy' surface), which an M4 Max's Metal drops: the surface's bytes stay as they were and a
/// later read() gives its old texels. The driver binds a null texture for the dropped store; a kernel reading a null
/// binding afterwards still reads 0.
enum KernelConvertedWrites {
    struct Result {
        var paths: [String] = []
        var bytesChanged = 0
        var readsChanged = 0
        var nullNonZero = 0
    }

    /// Each view, with the usage an M4 Max makes it with (501 over '2vuy' only shaderRead, the kernel's write regardless)
    struct View {
        let key: String
        let fourcc: OSType
        let format: UInt
        let width: Int
        let height: Int
        let usage: MTLTextureUsage
    }

    static let views = [
        View(
            key: "p420_588", fourcc: YcbcrPackedTargetTest.p420, format: 588, width: 96, height: 8,
            usage: [.shaderRead, .shaderWrite]
        ),
        View(
            key: "vuy_501", fourcc: kCVPixelFormatType_422YpCbCr8, format: 501, width: 34, height: 9, usage: .shaderRead
        ),
    ]

    static func surfaceBytes(_ surface: IOSurfaceRef) -> [UInt8] {
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        return Array(
            UnsafeRawBufferPointer(start: IOSurfaceGetBaseAddress(surface), count: IOSurfaceGetAllocSize(surface))
        )
    }

    static func run(_ gpu: GPU, _ view: View) async throws -> Result {
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, view.width, view.height, view.fourcc, attributes, &made) == kCVReturnSuccess,
              let buffer = made, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
              let format = MTLPixelFormat(rawValue: view.format)
        else { throw TestFailure("no \(view.key) surface") }
        IOSurfaceLock(surface, [], nil)
        let base = IOSurfaceGetBaseAddress(surface).bindMemory(to: UInt8.self, capacity: IOSurfaceGetAllocSize(surface))
        for at in 0 ..< IOSurfaceGetAllocSize(surface) { base[at] = UInt8(truncatingIfNeeded: at &* 131 &+ 17) }
        IOSurfaceUnlock(surface, [], nil)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: view.width, height: view.height, mipmapped: false
        )
        descriptor.usage = view.usage
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no \(view.key) texture")
        }
        let before = surfaceBytes(surface)
        let read = try await gpu.texels(texture)
        let writer = try gpu.compute("write_converted_texture")
        var result = Result()
        result.paths.append(
            try await gpu.run(tolerateError: true) { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(writer)
                encoder.setTexture(texture, index: 0)
                encoder.dispatchThreads(
                    MTLSize(width: view.width, height: view.height, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
                )
                encoder.endEncoding()
            }.path
        )
        result.bytesChanged = zip(surfaceBytes(surface), before).filter { $0 != $1 }.count
        result.readsChanged = zip(try await gpu.texels(texture), read).filter { $0 != $1 }.count
        let reader = try gpu.compute("read_bound_texture")
        let out = try gpu.buffer([UInt8](repeating: 0x5a, count: 16 * 16))
        result.paths.append(
            try await gpu.run(tolerateError: true) { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(reader)
                encoder.setTexture(nil, index: 0)
                encoder.setBuffer(out, offset: 0, index: 0)
                encoder.dispatchThreads(
                    MTLSize(width: 16, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 16, height: 1, depth: 1)
                )
                encoder.endEncoding()
            }.path
        )
        result.nullNonZero = UnsafeRawBufferPointer(start: out.contents(), count: 16 * 16).filter { $0 != 0 }.count
        return result
    }
}
