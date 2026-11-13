import CoreMedia
import CoreVideo
import Darwin
import Foundation
import HWTestCore
import VideoToolbox

/// A picture through VideoToolbox's tile compression session, the SPI CMPhoto encodes HEIC grids with (24B119
/// VTTileCompressionPluginClass_prepare 0x1a6bb0c54 and _encode 0x1a6bb353c, the same properties): the encoder
/// ImageIO's HEIC reaches, with the session in hand to ask it how the tiles were coded. Every tile sample is decoded
/// back on the hardware decoder and its luma compared with the source's.
public enum TileCompressor {
    public struct Result {
        public let expected: Int
        public let tiles: Int
        public let failed: Int
        public let usingHardware: Bool
        /// SwitchEncoderRoutes {pictures, tiles, errors, input_vic, input_bounced, input_cpu_copies, input_refused}
        /// (debug builds of the AVE plug-in): nil from any other encoder.
        public let routes: [String: Int]?
        public let bytes: Int
        public let decoded: Int
        /// Mean absolute luma difference over a tile's region of the source, the worst tile's.
        public let worstLumaError: Double
        public let seconds: Double
    }

    typealias Output =
        @convention(c) (
            UnsafeMutableRawPointer?, UnsafeMutableRawPointer?, UInt64, UInt64, OSStatus, UInt32,
            UnsafeMutableRawPointer?
        ) -> Void
    typealias Create =
        @convention(c) (
            CFAllocator?, UInt64, CMVideoCodecType, CFDictionary?, CFDictionary?, CFAllocator?, Output,
            UnsafeMutableRawPointer?, UnsafeMutablePointer<UnsafeMutableRawPointer?>
        ) -> OSStatus
    typealias Prepare = @convention(c) (UnsafeMutableRawPointer, UInt8, UnsafeMutablePointer<UInt64>?) -> OSStatus
    typealias Encode =
        @convention(c) (
            UnsafeMutableRawPointer, CVPixelBuffer, UInt64, UInt64, CFDictionary?, UnsafeMutableRawPointer?,
            UnsafeMutablePointer<UInt32>?
        ) -> OSStatus
    typealias Complete = @convention(c) (UnsafeMutableRawPointer) -> OSStatus
    typealias Copy =
        @convention(c) (UnsafeMutableRawPointer, CFString, CFAllocator?, UnsafeMutablePointer<Unmanaged<CFTypeRef>?>)
        -> OSStatus
    typealias Invalidate = @convention(c) (UnsafeMutableRawPointer) -> Void

    struct Calls {
        let create: Create
        let prepare: Prepare
        let encode: Encode
        let complete: Complete
        let copy: Copy
        let invalidate: Invalidate

        init?() {
            guard let vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW),
                let create = dlsym(vt, "VTTileCompressionSessionCreate"),
                let prepare = dlsym(vt, "VTTileCompressionSessionPrepareToEncodeTiles"),
                let encode = dlsym(vt, "VTTileCompressionSessionEncodeTile"),
                let complete = dlsym(vt, "VTTileCompressionSessionCompleteTiles"),
                let copy = dlsym(vt, "VTTileCompressionSessionCopyProperty"),
                let invalidate = dlsym(vt, "VTTileCompressionSessionInvalidate")
            else { return nil }
            self.create = unsafeBitCast(create, to: Create.self)
            self.prepare = unsafeBitCast(prepare, to: Prepare.self)
            self.encode = unsafeBitCast(encode, to: Encode.self)
            self.complete = unsafeBitCast(complete, to: Complete.self)
            self.copy = unsafeBitCast(copy, to: Copy.self)
            self.invalidate = unsafeBitCast(invalidate, to: Invalidate.self)
        }
    }

    /// VTInt32Point and VTInt32Size travel by value in one register: x (width) low, y (height) high.
    static func pack(_ low: Int, _ high: Int) -> UInt64 {
        UInt64(UInt32(truncatingIfNeeded: low)) | UInt64(UInt32(truncatingIfNeeded: high)) << 32
    }

    final class Collector: @unchecked Sendable {
        private let lock = NSLock()
        private(set) var samples: [Int: CMSampleBuffer] = [:]
        private(set) var failed = 0

        func add(_ index: Int, _ status: OSStatus, _ flags: UInt32, _ sample: CMSampleBuffer?) {
            lock.lock()
            defer { lock.unlock() }
            if status == noErr, flags & VTEncodeInfoFlags.frameDropped.rawValue == 0, let sample {
                samples[index] = sample
            } else {
                failed += 1
            }
        }
    }

    struct Region {
        let x: Int
        let y: Int
        let width: Int
        let height: Int
    }

    static func regions(width: Int, height: Int, tile: Int) -> [Region] {
        stride(from: 0, to: height, by: tile).flatMap { y in
            stride(from: 0, to: width, by: tile).map { x in
                Region(x: x, y: y, width: min(tile, width - x), height: min(tile, height - y))
            }
        }
    }

    public static func encode(_ source: CVPixelBuffer, tile: Int, quality: Double) throws -> Result {
        guard let calls = Calls() else { throw VideoError.message("VideoToolbox has no tile compression session") }
        let collector = Collector()
        let specification = [kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: true] as CFDictionary
        let attributes = [kCVPixelBufferPixelFormatTypeKey: CVPixelBufferGetPixelFormatType(source)] as CFDictionary
        var raw: UnsafeMutableRawPointer?
        let created = calls.create(
            nil, pack(tile, tile), kCMVideoCodecType_HEVC, specification, attributes, nil,
            { refcon, source, _, _, status, flags, sample in
                guard let refcon else { return }
                Unmanaged<Collector>.fromOpaque(refcon).takeUnretainedValue().add(
                    Int(bitPattern: source) - 1, status, flags,
                    sample.map { Unmanaged<CMSampleBuffer>.fromOpaque($0).takeUnretainedValue() })
            }, Unmanaged.passUnretained(collector).toOpaque(), &raw)
        guard created == noErr, let raw else { throw VideoError.status("VTTileCompressionSessionCreate", created) }
        let session = Unmanaged<CFTypeRef>.fromOpaque(raw).takeRetainedValue()
        // Every call after the property batch goes through `raw`: the session must outlive them all.
        defer { withExtendedLifetime(session) { calls.invalidate(raw) } }
        let properties: [CFString: Any] = [
            kVTCompressionPropertyKey_AllowTemporalCompression: false,
            kVTCompressionPropertyKey_AllowFrameReordering: false,
            kVTCompressionPropertyKey_MaxKeyFrameInterval: 1,
            kVTCompressionPropertyKey_ProfileLevel: "HEVC_MainStill_AutoLevel",
            kVTCompressionPropertyKey_Quality: quality,
        ]
        let set = VTSessionSetProperties(session, propertyDictionary: properties as CFDictionary)
        guard set == noErr else { throw VideoError.status("VTSessionSetProperties", set) }
        var prepared: UInt64 = 0
        _ = calls.prepare(raw, 0, &prepared)
        let regions = regions(
            width: CVPixelBufferGetWidth(source), height: CVPixelBufferGetHeight(source), tile: tile)
        let start = CFAbsoluteTimeGetCurrent()
        for (index, region) in regions.enumerated() {
            let status = calls.encode(
                raw, source, pack(region.x, region.y), pack(region.width, region.height), nil,
                UnsafeMutableRawPointer(bitPattern: index + 1), nil)
            guard status == noErr else {
                throw VideoError.status("VTTileCompressionSessionEncodeTile \(index)", status)
            }
        }
        let completed = calls.complete(raw)
        let seconds = CFAbsoluteTimeGetCurrent() - start
        guard completed == noErr else { throw VideoError.status("VTTileCompressionSessionCompleteTiles", completed) }

        func read(_ key: CFString) -> CFTypeRef? {
            var value: Unmanaged<CFTypeRef>?
            guard calls.copy(raw, key, nil, &value) == noErr else { return nil }
            return value?.takeRetainedValue()
        }
        let samples = withExtendedLifetime(collector) { collector.samples }
        let check = try decodeBack(samples, regions: regions, source: source, tile: tile)
        return Result(
            expected: regions.count, tiles: samples.count, failed: collector.failed,
            usingHardware: (read(kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder) as? Bool) ?? false,
            routes: (read("SwitchEncoderRoutes" as CFString) as? [String: NSNumber])?.mapValues(\.intValue),
            bytes: samples.values.reduce(0) { $0 + CMSampleBufferGetTotalSampleSize($1) }, decoded: check.decoded,
            worstLumaError: check.worst, seconds: seconds)
    }

    final class Picture: @unchecked Sendable {
        var image: CVImageBuffer?
    }

    /// Each tile sample decoded on its own, compared with its region of the source.
    static func decodeBack(
        _ samples: [Int: CMSampleBuffer], regions: [Region], source: CVPixelBuffer, tile: Int
    ) throws -> (decoded: Int, worst: Double) {
        guard let first = samples.values.first, let format = CMSampleBufferGetFormatDescription(first) else {
            return (0, 0)
        }
        let session = try Decompressor.makeSession(
            format: format, width: tile, height: tile, bitDepth: 8, requireHardware: true)
        defer { VTDecompressionSessionInvalidate(session) }
        var decoded = 0
        var worst = 0.0
        for (index, region) in regions.enumerated() {
            guard let sample = samples[index] else { continue }
            let box = Picture()
            let status = VTDecompressionSessionDecodeFrame(
                session, sampleBuffer: sample, flags: [], infoFlagsOut: nil
            ) { status, _, image, _, _ in if status == noErr { box.image = image } }
            VTDecompressionSessionWaitForAsynchronousFrames(session)
            guard status == noErr, let picture = box.image else { continue }
            decoded += 1
            worst = max(worst, lumaError(picture, source, region))
        }
        return (decoded, worst)
    }

    static func lumaError(_ decoded: CVPixelBuffer, _ source: CVPixelBuffer, _ region: Region) -> Double {
        CVPixelBufferLockBaseAddress(decoded, .readOnly)
        CVPixelBufferLockBaseAddress(source, .readOnly)
        defer {
            CVPixelBufferUnlockBaseAddress(decoded, .readOnly)
            CVPixelBufferUnlockBaseAddress(source, .readOnly)
        }
        guard let out = CVPixelBufferGetBaseAddressOfPlane(decoded, 0)?.assumingMemoryBound(to: UInt8.self),
            let into = CVPixelBufferGetBaseAddressOfPlane(source, 0)?.assumingMemoryBound(to: UInt8.self)
        else { return .infinity }
        let outRow = CVPixelBufferGetBytesPerRowOfPlane(decoded, 0)
        let inRow = CVPixelBufferGetBytesPerRowOfPlane(source, 0)
        var sum = 0
        for y in 0..<region.height {
            for x in 0..<region.width {
                sum += abs(Int(out[y * outRow + x]) - Int(into[(region.y + y) * inRow + region.x + x]))
            }
        }
        return Double(sum) / Double(region.width * region.height)
    }
}
