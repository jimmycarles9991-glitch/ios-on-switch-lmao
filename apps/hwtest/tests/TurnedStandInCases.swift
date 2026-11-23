import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

enum TurnedStandInCases {
    enum Source {
        case plain(width: Int, height: Int, bytes: Int, fourcc: OSType)
        case pixelBuffer(OSType, width: Int, height: Int)
    }

    struct Arm {
        let key: String
        let format: UInt
        let turn: Int
        let source: Source
        let plane: Int
        let indexed: Bool
    }

    static let arms = [
        Arm(
            key: "rot1_rg8srgb", format: 31, turn: 1,
            source: .plain(width: 32, height: 16, bytes: 2, fourcc: kCVPixelFormatType_TwoComponent8),
            plane: 0, indexed: false
        ),
        Arm(
            key: "rot3_bgra10xrsrgb", format: 553, turn: 3,
            source: .plain(width: 32, height: 16, bytes: 8, fourcc: kCVPixelFormatType_64RGBAHalf), plane: 0,
            indexed: false
        ),
        Arm(
            key: "rot2_bgr10xr", format: 554, turn: 2,
            source: .pixelBuffer(kCVPixelFormatType_30RGBLEPackedWideGamut, width: 32, height: 16),
            plane: 0, indexed: false
        ),
        Arm(
            key: "rot1_r8_plane0", format: 10, turn: 1,
            source: .pixelBuffer(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, width: 32, height: 8), plane: 0,
            indexed: true
        ),
        Arm(
            key: "rot3_rg8_plane1", format: 30, turn: 3,
            source: .pixelBuffer(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, width: 64, height: 32), plane: 1,
            indexed: false
        ),
    ]

    struct Plane {
        let surface: IOSurfaceRef
        let owner: CVPixelBuffer?
        let index: Int
        let width, height: Int

        var planar: Bool { IOSurfaceGetPlaneCount(surface) > 0 }
        var rowBytes: Int { planar ? IOSurfaceGetBytesPerRowOfPlane(surface, index) : IOSurfaceGetBytesPerRow(surface) }
        var base: UnsafeMutableRawPointer {
            planar ? IOSurfaceGetBaseAddressOfPlane(surface, index) : IOSurfaceGetBaseAddress(surface)
        }
    }

    struct Result {
        let off: Int
        let looks: String
    }

    static func frame(_ arm: Arm, _ plane: Plane) -> (width: Int, height: Int) {
        arm.turn == 2 ? (plane.width, plane.height) : (plane.height, plane.width)
    }

    static func plane(_ source: Source, _ index: Int) throws -> Plane {
        switch source {
        case .plain(let width, let height, let bytes, let fourcc):
            let properties =
                [
                    kIOSurfaceWidth: width, kIOSurfaceHeight: height, kIOSurfaceBytesPerElement: bytes,
                    kIOSurfacePixelFormat: fourcc,
                ] as [CFString: Any] as CFDictionary
            guard let surface = IOSurfaceCreate(properties) else { throw TestFailure("IOSurface creation failed") }
            return Plane(surface: surface, owner: nil, index: 0, width: width, height: height)
        case .pixelBuffer(let fourcc, let width, let height):
            var made: CVPixelBuffer?
            let attributes =
                [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
            guard CVPixelBufferCreate(nil, width, height, fourcc, attributes, &made) == kCVReturnSuccess,
                  let buffer = made, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
            else { throw TestFailure("no pixel buffer of \(fourcc)") }
            let planar = IOSurfaceGetPlaneCount(surface) > 0
            return Plane(
                surface: surface, owner: buffer, index: index,
                width: planar ? IOSurfaceGetWidthOfPlane(surface, index) : IOSurfaceGetWidth(surface),
                height: planar ? IOSurfaceGetHeightOfPlane(surface, index) : IOSurfaceGetHeight(surface)
            )
        }
    }

    static func turned(
        _ gpu: GPU, _ format: UInt, turn: Int, over plane: Plane, frame: (width: Int, height: Int)
    )
        throws -> MTLTexture
    {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: format)!, width: frame.width, height: frame.height, mipmapped: false
        )
        descriptor.usage = .renderTarget
        descriptor.storageMode = .shared
        guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else {
            throw TestFailure("no rotation SPI on this descriptor")
        }
        descriptor.setValue(NSNumber(value: turn), forKey: "rotation")
        descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: plane.surface, plane: plane.index)
        else { throw TestFailure("no \(format) texture of rotation \(turn) over plane \(plane.index)") }
        return texture
    }

    static func draw(_ gpu: GPU, into texture: MTLTexture, indexedWidth: Int?) async throws {
        let state = try gpu.render(
            fragment: indexedWidth == nil ? "turned_where_fragment" : "turned_index_fragment",
            format: texture.pixelFormat
        )
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(Float(indexedWidth ?? 0), 0, 0, 0))
            encoder.endEncoding()
        }
    }

    static func zero(_ plane: Plane) throws {
        guard IOSurfaceLock(plane.surface, [], nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        memset(plane.base, 0, plane.rowBytes * plane.height)
        IOSurfaceUnlock(plane.surface, [], nil)
    }

    static func bytesPerTexel(_ format: UInt) -> Int {
        switch format {
        case 553: return 8
        case 80, 554: return 4
        case 10: return 1
        default: return 2
        }
    }

    static func pixels(_ plane: Plane, format: UInt, indexedWidth: Int?) throws -> [SIMD2<Int>] {
        guard IOSurfaceLock(plane.surface, .readOnly, nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        defer { IOSurfaceUnlock(plane.surface, .readOnly, nil) }
        let bytes = bytesPerTexel(format)
        let step = { (value: Double) in Int((value * 64).rounded()) }
        let unorm = { (byte: UInt8) in step(Double(byte) / 255) }
        let srgb = { (byte: UInt8) -> Int in
            let v = Double(byte) / 255
            return step(v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4))
        }
        let xr = { (code: Int) in step((Double(code) - 384) / 510) }
        return (0 ..< plane.height).flatMap { y in
            (0 ..< plane.width).map { x -> SIMD2<Int> in
                let t = [UInt8](
                    UnsafeRawBufferPointer(start: plane.base + y * plane.rowBytes + x * bytes, count: bytes)
                )
                if let width = indexedWidth {
                    let index = Int(format == 80 ? t[2] : t[0])
                    return SIMD2(index % width, index / width)
                }
                switch format {
                case 80: return SIMD2(unorm(t[2]), unorm(t[1]))
                case 31: return SIMD2(srgb(t[0]), srgb(t[1]))
                case 553:
                    let codes = XrSrgbFormat.bgra.codes(t)[0]
                    let curve = { (code: Int) in step(Double(XrSrgb.decoded(code, alpha: false))) }
                    return SIMD2(curve(codes[0]), curve(codes[1]))
                case 554:
                    let word = Int(t[0]) | Int(t[1]) << 8 | Int(t[2]) << 16 | Int(t[3]) << 24
                    return SIMD2(xr(word >> 20 & 1023), xr(word >> 10 & 1023))
                default: return SIMD2(unorm(t[0]), unorm(t[1]))
                }
            }
        }
    }

    static func looks(
        _ gpu: GPU, _ states: RotatedStates, _ got: [SIMD2<Int>], plane: Plane, frame: (width: Int, height: Int)
    )
        -> String
    {
        let misses = (0 ... 3).map { turn -> Int in
            let cells = RotatedCells(
                gpu: gpu, states: states, turn: max(turn, 1), width: frame.width, height: frame.height, aligned: false
            )
            var missed = 0
            for y in 0 ..< frame.height {
                for x in 0 ..< frame.width {
                    let (px, py) = turn == 0 ? (x, y) : cells.stored(x, y)
                    if px >= plane.width || py >= plane.height || got[py * plane.width + px] != SIMD2(x, y) {
                        missed += 1
                    }
                }
            }
            return missed
        }
        let best = misses.indices.min { misses[$0] < misses[$1] }!
        return misses[best] == 0 ? "r\(best)" : "none_nearest_r\(best)_\(misses[best])"
    }

    static func run(_ gpu: GPU, _ states: RotatedStates, _ arm: Arm) async throws -> Result {
        let plane = try plane(arm.source, arm.plane)
        let frame = frame(arm, plane)
        let texture = try turned(gpu, arm.format, turn: arm.turn, over: plane, frame: frame)
        let cells = RotatedCells(
            gpu: gpu, states: states, turn: arm.turn, width: frame.width, height: frame.height, aligned: false
        )
        guard let made = try cells.makePlane() else { throw TestFailure("no BGRA8 texture of rotation \(arm.turn)") }
        let reference = Plane(surface: made.surface, owner: nil, index: 0, width: made.width, height: made.height)
        let indexedWidth = arm.indexed ? frame.width : nil
        try zero(plane)
        try zero(reference)
        try await draw(gpu, into: texture, indexedWidth: indexedWidth)
        try await draw(gpu, into: made.texture, indexedWidth: indexedWidth)
        let got = try pixels(plane, format: arm.format, indexedWidth: indexedWidth)
        let want = try pixels(reference, format: 80, indexedWidth: indexedWidth)
        let off = zip(got, want).filter { $0 != $1 }.count
        return Result(off: off, looks: off == 0 ? "r\(arm.turn)" : looks(gpu, states, got, plane: plane, frame: frame))
    }
}
