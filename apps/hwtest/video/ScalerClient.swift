import CoreVideo
import Foundation
import IOSurface

/// IOSurfaceAccelerator's own client of the scaler (VIC here, the M-series scaler on a Mac), through dlsym: no SDK
/// header declares it. TransformSurface with no completion is synchronous; its IOReturn comes back, and the arguments
/// after the options are VideoToolbox's (iOS 27's VideoToolbox passes a completion pointer, then three zeros).
public final class ScalerClient {
    typealias Create = @convention(c) (
        CFAllocator?, CFDictionary?, UnsafeMutablePointer<UnsafeMutableRawPointer?>
    ) -> Int32
    typealias Transform = @convention(c) (
        UnsafeMutableRawPointer, IOSurfaceRef, IOSurfaceRef, CFDictionary?, UnsafeMutableRawPointer?,
        UnsafeMutableRawPointer?, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?
    ) -> Int32

    static let path = "/System/Library/PrivateFrameworks/IOSurfaceAccelerator.framework/IOSurfaceAccelerator"
    let accelerator: UnsafeMutableRawPointer
    let transformSurface: Transform

    /// nil with why there is none
    public static func open() -> Result<ScalerClient, ScalerError> {
        guard let framework = dlopen(path, RTLD_NOW), let create = dlsym(framework, "IOSurfaceAcceleratorCreate"),
              let transform = dlsym(framework, "IOSurfaceAcceleratorTransformSurface")
        else { return .failure(ScalerError("no IOSurfaceAccelerator")) }
        var accelerator: UnsafeMutableRawPointer?
        let status = unsafeBitCast(create, to: Create.self)(nil, nil, &accelerator)
        guard status == 0, let accelerator else {
            return .failure(
                ScalerError("IOSurfaceAcceleratorCreate 0x\(String(UInt32(bitPattern: status), radix: 16))")
            )
        }
        return .success(ScalerClient(accelerator, unsafeBitCast(transform, to: Transform.self)))
    }

    init(_ accelerator: UnsafeMutableRawPointer, _ transform: @escaping Transform) {
        self.accelerator = accelerator
        transformSurface = transform
    }

    deinit { Unmanaged<AnyObject>.fromOpaque(accelerator).release() }

    /// The whole of `source` into the whole of `destination`, waited for: the IOReturn
    public func transform(_ source: CVPixelBuffer, _ destination: CVPixelBuffer) -> Int32 {
        guard let s = CVPixelBufferGetIOSurface(source)?.takeUnretainedValue(),
              let d = CVPixelBufferGetIOSurface(destination)?.takeUnretainedValue()
        else { return -1 }
        return transformSurface(accelerator, s, d, nil, nil, nil, nil, nil)
    }

    /// An IOSurface-backed buffer with rows and planes on 256 bytes (VIC's source row rule), tagged as CoreVideo tags
    /// one (its attachments propagate to the surface, whose bulk attachments the kernel reads)
    public static func buffer(
        _ width: Int, _ height: Int, _ format: OSType, tags: [String: String] = [:]
    ) throws -> CVPixelBuffer {
        let attributes: [String: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            kCVPixelBufferBytesPerRowAlignmentKey as String: 256, kCVPixelBufferPlaneAlignmentKey as String: 256,
        ]
        var buffer: CVPixelBuffer?
        let status = CVPixelBufferCreate(nil, width, height, format, attributes as CFDictionary, &buffer)
        guard status == kCVReturnSuccess, let buffer else {
            throw ScalerError("no \(width)x\(height) '\(String(format: "%08x", format))' buffer (\(status))")
        }
        for (key, value) in tags { CVBufferSetAttachment(buffer, key as CFString, value as CFString, .shouldPropagate) }
        return buffer
    }

    /// `body` over plane `plane`'s bytes and row pitch, locked for writing
    public static func edit(
        _ buffer: CVPixelBuffer,
        plane: Int = 0,
        _ body: (UnsafeMutablePointer<UInt8>, Int) -> Void
    ) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let planar = CVPixelBufferIsPlanar(buffer)
        let base = planar ? CVPixelBufferGetBaseAddressOfPlane(buffer, plane) : CVPixelBufferGetBaseAddress(buffer)
        let pitch = planar ? CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) : CVPixelBufferGetBytesPerRow(buffer)
        body(base!.assumingMemoryBound(to: UInt8.self), pitch)
    }
}

public struct ScalerError: Error, CustomStringConvertible {
    public let description: String

    init(_ description: String) { self.description = description }
}
