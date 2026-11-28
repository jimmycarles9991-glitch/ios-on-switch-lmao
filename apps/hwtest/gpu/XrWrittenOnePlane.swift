import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// RGB10A8_2P_XR10 (550) and its sRGB twin (551) render targets over one plane alone, which holds no alpha: a 'w30r' of
/// 64x64 words (rows 256 bytes, 4 KiB more after them) and plane 1 of a 'b3a8' (alpha bytes, rows 64), every byte 0xff
/// first (gpu.gm20b.xr_written_one_plane).
public enum XrWrittenOnePlane {
    public enum Arm: String, Sendable, CaseIterable {
        /// a Clear (0.5, 0.25, -0.25, 1) and Store pass
        case clear550 = "clear_550", clear551 = "clear_551"
        /// a Load pass drawing that colour with source factor OneMinusDestinationAlpha, destination factor Zero
        case blend550 = "blend_550"
        /// the cleared target's own texels read by a draw into RGBA16Float
        case read550 = "read_550"
        /// a 16x16 550 over plane 1 of a 64x64 'b3a8', cleared
        case plane1 = "plane1_550"
    }

    public struct Reading: Sendable {
        public var made = false
        /// The first word of the target and the words not that one (-1: no word read)
        public var word: UInt32 = 0
        public var wordsOff = 0
        /// Bytes after the target's rows that changed
        public var pastChanged = 0
        /// read_550: texels more than one half step off the word's colour with alpha 0
        public var texelsOff = 0
        public var paths: [String] = []
    }

    static let colour = MTLClearColor(red: 0.5, green: 0.25, blue: -0.25, alpha: 1)

    static func surface(_ arm: Arm) -> IOSurfaceRef? {
        let properties: [CFString: Any]
        if arm == .plane1 {
            let planes: [[CFString: Any]] = [
                [
                    kIOSurfacePlaneWidth: 64, kIOSurfacePlaneHeight: 64, kIOSurfacePlaneBytesPerElement: 4,
                    kIOSurfacePlaneBytesPerRow: 256, kIOSurfacePlaneOffset: 0, kIOSurfacePlaneSize: 16384,
                ],
                [
                    kIOSurfacePlaneWidth: 64, kIOSurfacePlaneHeight: 64, kIOSurfacePlaneBytesPerElement: 1,
                    kIOSurfacePlaneBytesPerRow: 64, kIOSurfacePlaneOffset: 16384, kIOSurfacePlaneSize: 4096,
                ],
            ]
            properties = [
                kIOSurfaceWidth: 64, kIOSurfaceHeight: 64, kIOSurfacePixelFormat: OSType(0x6233_6138),
                kIOSurfaceAllocSize: 24576, kIOSurfacePlaneInfo: planes,
            ]
        } else {
            properties = [
                kIOSurfaceWidth: 64, kIOSurfaceHeight: 64, kIOSurfacePixelFormat: OSType(0x7733_3072),
                kIOSurfaceBytesPerElement: 4, kIOSurfaceBytesPerRow: 256, kIOSurfaceAllocSize: 256 * 64 + 4096,
            ]
        }
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { return nil }
        IOSurfaceLock(surface, [], nil)
        memset(IOSurfaceGetBaseAddress(surface), 0xff, IOSurfaceGetAllocSize(surface))
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    static func pass(_ gpu: GPU, _ texture: MTLTexture, load: Bool, _ draw: ((MTLRenderCommandEncoder) throws -> Void)?)
        async throws -> GPU.Timing
    {
        try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = load ? .load : .clear
            pass.colorAttachments[0].clearColor = colour
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            try draw?(encoder)
            encoder.endEncoding()
        }
    }

    public static func observe(_ gpu: GPU, _ arm: Arm) async throws -> Reading {
        var reading = Reading()
        guard let surface = surface(arm) else { throw TestFailure("no surface for \(arm.rawValue)") }
        let format = MTLPixelFormat(rawValue: arm == .clear551 ? 551 : 550)!, size = arm == .plane1 ? 16 : 64
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format,
            width: size,
            height: size,
            mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(
            descriptor: descriptor,
            iosurface: surface,
            plane: arm == .plane1 ? 1 : 0
        )
        else { return reading }
        reading.made = true
        reading.paths.append(try await pass(gpu, texture, load: false, nil).facadePath)
        if arm == .blend550 {
            IOSurfaceLock(surface, [], nil)
            memset(IOSurfaceGetBaseAddress(surface), 0, 256 * 64)
            IOSurfaceUnlock(surface, [], nil)
            let state = try gpu.render(fragment: "solid_fragment", format: format) { d in
                d.colorAttachments[0].isBlendingEnabled = true
                d.colorAttachments[0].sourceRGBBlendFactor = .oneMinusDestinationAlpha
                d.colorAttachments[0].destinationRGBBlendFactor = .zero
            }
            reading.paths.append(try await pass(gpu, texture, load: true) { encoder in
                encoder.setRenderPipelineState(state)
                GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0.5, 0.25, -0.25, 1))
            }.facadePath)
        }
        let (rowBytes, at) = arm == .plane1 ? (64, 16384) : (256, 0)
        IOSurfaceLock(surface, .readOnly, nil)
        let base = IOSurfaceGetBaseAddress(surface)
        reading.word = base.load(fromByteOffset: at, as: UInt32.self)
        for y in 0 ..< size {
            for x in 0 ..< size
                where base.load(fromByteOffset: at + y * rowBytes + 4 * x, as: UInt32.self) != reading.word {
                reading.wordsOff += 1
            }
        }
        let end = at + rowBytes * (size - 1) + 4 * size
        reading.pastChanged = (end ..< IOSurfaceGetAllocSize(surface)).filter { base.load(
            fromByteOffset: $0,
            as: UInt8.self
        ) != 0xff }.count
        IOSurfaceUnlock(surface, .readOnly, nil)
        if arm == .read550 {
            let target = try gpu.texture(.rgba16Float, size, size)
            let state = try gpu.render(fragment: "read_texel_fragment", format: .rgba16Float)
            let timing = try await gpu.run(tolerateError: true) { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                var corners = GPU.fullScreen
                encoder.setRenderPipelineState(state)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentTexture(texture, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            reading.paths.append(timing.facadePath)
            let bytes = try await gpu.read(target, bytesPerPixel: 8), want = XrPlanesRows.texel(reading.word, 0)
            reading.texelsOff = (0 ..< (size * size)).filter { i in
                let got = bytes.loaded(as: SIMD4<Float16>.self, at: i)
                return (0 ..< 4)
                    .contains {
                        abs(Int(Int16(bitPattern: got[$0].bitPattern)) - Int(Int16(bitPattern: want[$0].bitPattern))) >
                            1 }
            }.count
        }
        return reading
    }
}
