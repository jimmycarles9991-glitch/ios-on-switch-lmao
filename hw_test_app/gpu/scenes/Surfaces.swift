import CoreVideo
import Foundation
import HWTestCore
import IOSurface
import Metal

/// An IOSurface and a Metal texture over it, with the CPU's view of its texels through IOSurface's lock.
struct SurfaceTarget {
    let surface: IOSurface
    let texture: MTLTexture
    let format: TexelFormat
    let width: Int
    let height: Int

    static func pixelFormat(_ format: TexelFormat) -> OSType {
        switch format {
        case .r8Unorm: return kCVPixelFormatType_OneComponent8
        case .rgba16Float: return kCVPixelFormatType_64RGBAHalf
        default: return kCVPixelFormatType_32BGRA
        }
    }

    /// `rowBytes` 0: the texels' bytes rounded up to 64, as Core Animation lays out its surfaces.
    static func make(
        _ gpu: GPU, _ format: TexelFormat, _ width: Int, _ height: Int, rowBytes: Int = 0, usage: MTLTextureUsage
    ) throws -> SurfaceTarget {
        let stride = rowBytes > 0 ? rowBytes : (width * format.bytesPerTexel + 63) / 64 * 64
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .bytesPerElement: format.bytesPerTexel, .bytesPerRow: stride,
            .allocSize: stride * height, .pixelFormat: pixelFormat(format),
        ]
        guard let surface = IOSurface(properties: properties) else {
            throw TestFailure("IOSurface \(format.rawValue) \(width)x\(height) row \(stride) not made")
        }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format.metal, width: width, height: height, mipmapped: false)
        descriptor.usage = usage
        descriptor.storageMode = .shared
        guard
            let texture = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { throw TestFailure("makeTexture(iosurface:) \(format.rawValue) \(width)x\(height) returned nil") }
        return SurfaceTarget(surface: surface, texture: texture, format: format, width: width, height: height)
    }

    /// The texels through a read-only lock, rows packed.
    func cpuRead() throws -> [UInt8] {
        try lock(.readOnly) {
            let row = width * format.bytesPerTexel
            return (0..<height).flatMap { y in
                Array(UnsafeRawBufferPointer(start: surface.baseAddress + y * surface.bytesPerRow, count: row))
            }
        }
    }

    func cpuWrite(_ rect: UploadRect, _ bytes: [UInt8]) throws {
        try lock([]) {
            let texel = format.bytesPerTexel
            for row in 0..<rect.height {
                let to = surface.baseAddress + (rect.y + row) * surface.bytesPerRow + rect.x * texel
                bytes.withUnsafeBytes {
                    to.copyMemory(from: $0.baseAddress! + row * rect.width * texel, byteCount: rect.width * texel)
                }
            }
        }
    }

    func lock<T>(_ options: IOSurfaceLockOptions, _ body: () -> T) throws -> T {
        let status = surface.lock(options: options, seed: nil)
        guard status == 0 else {
            throw TestFailure(
                "IOSurface lock \(options.rawValue) failed: 0x\(String(UInt32(bitPattern: status), radix: 16))")
        }
        defer { surface.unlock(options: options, seed: nil) }
        return body()
    }
}

extension GPU {
    /// One pass into `target`: a load-action clear (the raw path's own on the facade) and, when `rect` is given, a
    /// solid quad over it drawn by the app's pipeline (m2v on the facade).
    func clearAndFill(
        _ target: MTLTexture, clear: [Float], rect: UploadRect?, color: [Float], pipeline: MTLRenderPipelineState?
    ) throws -> MTLCommandBuffer {
        try commit { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(clear[0]), green: Double(clear[1]), blue: Double(clear[2]), alpha: Double(clear[3]))
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let rect, let pipeline {
                encoder.setRenderPipelineState(pipeline)
                GPU.draw(
                    encoder,
                    corners: GPU.pixelQuad(
                        rect.x, rect.y, rect.x + rect.width, rect.y + rect.height, width: target.width,
                        height: target.height),
                    color: SIMD4(color[0], color[1], color[2], color[3]))
            }
            encoder.endEncoding()
        }
    }
}

extension TexelImage {
    /// `color`'s channels the format keeps (RGBA order in, the format's channels out), over `rect` or everywhere.
    mutating func fill(_ color: [Float], _ rect: UploadRect? = nil) {
        let texel = format.encode(Array(color.prefix(format.channels)))
        let area = rect ?? UploadRect(x: 0, y: 0, width: width, height: height)
        write(area, Patterns.repeated(texel, count: area.width * area.height))
    }
}
