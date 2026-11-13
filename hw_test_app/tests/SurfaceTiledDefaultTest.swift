import CoreVideo
import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A fresh IOSurface the GPU renders first is block-linear (drivers/TegraGM20B/TegraGM20BTiled.cpp: its first GPU import
/// that asks, writing, while no other import holds it). The raw facade's import asks when the texture is made; after the
/// render, an import of the test's own connection asks too and gets that decision back. Its ask comes last because a
/// first ask that does not write decides pitch for good. One surface per shape: a square, and Core Animation's 720x1280
/// framebuffer with 3072-byte rows.
struct SurfaceTiledDefaultTest: HWTest {
    let id = "gpu.surface.tiled_default"
    let name = "A fresh IOSurface the GPU renders first is block-linear"
    let subsystem = Subsystem.metal

    static let shapes = [(256, 256, 1024), (720, 1280, 3072)]
    static let clearSeen: [UInt8] = [153, 102, 51, 255]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (width, height, rowBytes) in Self.shapes {
            let label = "\(width)x\(height)"
            let (surface, texture) = try Self.target(gpu, width, height, rowBytes)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            let (kr, layout) = connection.surfaceLayout(surface)
            let answer = kr == KERN_SUCCESS ? String(layout) : GPUConnection.hex(kr)
            metrics.set("layout_\(label)", answer)
            checks.append(("\(label) block-linear (kernel said \(answer))", answer == "1"))
            surface.lock(options: .readOnly, seed: nil)
            let seen = Array(
                UnsafeRawBufferPointer(start: surface.baseAddress + (height / 2) * rowBytes + width * 2, count: 4))
            surface.unlock(options: .readOnly, seed: nil)
            checks.append(("\(label) the clear reads back as rows", seen == Self.clearSeen))
            withExtendedLifetime(texture) {}
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func target(_ gpu: GPU, _ width: Int, _ height: Int, _ rowBytes: Int) throws -> (IOSurface, MTLTexture) {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .bytesPerElement: 4, .bytesPerRow: rowBytes,
            .allocSize: rowBytes * height, .pixelFormat: kCVPixelFormatType_32BGRA,
        ]
        guard let surface = IOSurface(properties: properties) else {
            throw TestFailure("IOSurface \(width)x\(height) not made")
        }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard
            let texture = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { throw TestFailure("makeTexture(iosurface:) \(width)x\(height) returned nil") }
        return (surface, texture)
    }
}

extension GPUConnection {
    static let boImportMethod: UInt32 = 3
    static let boFreeMethod: UInt32 = 4
    static let surfaceLayoutInfoMethod: UInt32 = 24
    static let importGpuReads: UInt32 = 1

    /// The kernel's layout of `surface` (TegraGM20BAbiSurfaceLayoutInfo.layout: 0 pitch, 1 block-linear, 2 block-linear
    /// at another shape), asked without writes through an import of this connection's own, freed after.
    func surfaceLayout(_ surface: IOSurface) -> (kern_return_t, UInt32) {
        let page = Int(getpagesize())
        let address = UInt64(UInt(bitPattern: surface.baseAddress))
        let bytes = UInt64((surface.allocationSize + page - 1) / page * page)
        let id = IOSurfaceGetID(unsafeBitCast(surface, to: IOSurfaceRef.self))
        let (imported, reply) = call(
            Self.boImportMethod, Self.words(split(address) + split(bytes) + [id, Self.importGpuReads]), outputSize: 16)
        guard imported == KERN_SUCCESS else { return (imported, 0) }
        let handle = Self.uint64(reply, 0)
        defer { _ = call(Self.boFreeMethod, Self.words(split(handle)), outputSize: 0) }
        let shape = [surface.width, surface.height, surface.bytesPerElement, surface.bytesPerRow].map { UInt32($0) }
        let (asked, info) = call(
            Self.surfaceLayoutInfoMethod, Self.words(split(handle) + shape + [0, 0]), outputSize: 48)
        return (asked, asked == KERN_SUCCESS ? Self.uint32(info, 0) : 0)
    }

    private func split(_ value: UInt64) -> [UInt32] { [UInt32(truncatingIfNeeded: value), UInt32(value >> 32)] }
}
