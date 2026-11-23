import CoreVideo
import Foundation
import VideoToolbox

/// Which unit a VTPixelTransferSession took for a pair: kVTPixelTransferPropertyKey_MostRecentConversionType, exported
/// by VideoToolbox in 21H461 and 24B119 though no SDK header declares it (image.pixel_transfer_route).
public enum PixelTransferRoutes {
    public struct Side: Sendable {
        let width, height: Int
        let format: OSType
        var tags: [String: String] = [:]
    }

    public struct Pair: Sendable {
        public let key: String
        let source: Side
        let destinations: [Side]
        /// The route VideoToolbox's registrations give the pair on the device: hardware where the scaler (VIC) is
        /// registered for it
        public let expected: String
        var ramp = false
    }

    static let matrix = kCVImageBufferYCbCrMatrixKey as String, function = kCVImageBufferTransferFunctionKey as String
    static let primaries = kCVImageBufferColorPrimariesKey as String
    static let l008 = kCVPixelFormatType_OneComponent8, bgra = kCVPixelFormatType_32BGRA
    static let p422 = OSType(0x7034_3232), p444 = OSType(0x7034_3434), x422 = OSType(0x7834_3232)
    static let x444 = OSType(0x7834_3434)
    static let srgb709 = [
        function: kCVImageBufferTransferFunction_sRGB as String,
        primaries: kCVImageBufferColorPrimaries_ITU_R_709_2 as String,
    ]

    public static let pairs: [Pair] = [
        Pair(
            key: "420f_bgra",
            source: Side(
                width: 1920, height: 1080, format: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                tags: [matrix: kCVImageBufferYCbCrMatrix_ITU_R_601_4 as String]
            ),
            destinations: [Side(width: 1280, height: 720, format: bgra)], expected: "hardware"
        ),
        Pair(
            key: "420v_bgra",
            source: Side(
                width: 1920, height: 1080, format: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                tags: [matrix: kCVImageBufferYCbCrMatrix_ITU_R_709_2 as String]
            ),
            destinations: [Side(width: 1280, height: 720, format: bgra)], expected: "hardware"
        ),
        Pair(
            key: "bgra_bgra", source: Side(width: 1920, height: 1080, format: bgra, tags: srgb709),
            destinations: [
                Side(width: 1920, height: 1080, format: bgra, tags: srgb709),
                Side(width: 960, height: 540, format: bgra, tags: srgb709),
            ], expected: "hardware"
        ),
        Pair(
            key: "l008_bgra", source: Side(width: 640, height: 480, format: l008),
            destinations: [Side(width: 640, height: 480, format: bgra)], expected: "hardware", ramp: true
        ),
        Pair(
            key: "bgra_l008", source: Side(width: 640, height: 480, format: bgra),
            destinations: [Side(width: 640, height: 480, format: l008)], expected: "metal"
        ),
        Pair(
            key: "420f_bgra_cm",
            source: Side(
                width: 1920, height: 1080, format: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                tags: [
                    function: kCVImageBufferTransferFunction_ITU_R_709_2 as String,
                    primaries: kCVImageBufferColorPrimaries_ITU_R_709_2 as String,
                ]
            ),
            destinations: [
                Side(
                    width: 1920, height: 1080, format: bgra,
                    tags: [
                        function: kCVImageBufferTransferFunction_sRGB as String,
                        primaries: kCVImageBufferColorPrimaries_P3_D65 as String,
                    ]
                ),
            ], expected: "metal"
        ),
        Pair(
            key: "p422_x422", source: Side(width: 640, height: 480, format: p422),
            destinations: [Side(width: 640, height: 480, format: x422)], expected: "metal"
        ),
        Pair(
            key: "p444_x444", source: Side(width: 640, height: 480, format: p444),
            destinations: [Side(width: 640, height: 480, format: x444)], expected: "metal"
        ),
    ]

    public struct Reading: Sendable {
        /// hardware, metal, software, both; none: no type after the transfers; unexported: no such symbol;
        /// unsupported: the session answers kVTPropertyNotSupportedErr (macOS 27's does)
        public var route = "none"
        public var status: OSStatus = 0
        /// The session's chain description after the last transfer ("-": none)
        public var chain = "-"
        /// l008_bgra: max |R - L|, |G - L|, |B - L| over the destination
        public var grayError: Int?
    }

    static func symbol(_ name: String) -> CFString? {
        guard let address = dlsym(UnsafeMutableRawPointer(bitPattern: -2), name) else { return nil }
        return address.assumingMemoryBound(to: CFString.self).pointee
    }

    static let routeNames = [
        ("Hardware", "hardware"), ("Metal", "metal"), ("Software", "software"), ("HardwareAndSoftware", "both"),
    ]

    /// The property's value; nil with "unexported" or "unsupported" where the session has none
    static func property(_ session: VTPixelTransferSession, _ name: String) -> (CFTypeRef?, String?) {
        guard let key = symbol(name) else { return (nil, "unexported") }
        var value: CFTypeRef?
        let status = withUnsafeMutablePointer(to: &value) {
            VTSessionCopyProperty(session, key: key, allocator: nil, valueOut: $0)
        }
        return (value, status == kVTPropertyNotSupportedErr ? "unsupported" : nil)
    }

    static func route(_ session: VTPixelTransferSession) -> String {
        let (value, missing) = property(session, "kVTPixelTransferPropertyKey_MostRecentConversionType")
        if let missing { return missing }
        guard let value = value as? String else { return "none" }
        let known = routeNames.first { symbol("kVTPixelTransferMostRecentConversionValue_" + $0.0) as String? == value }
        return known?.1 ?? value
    }

    static func buffer(_ side: Side) throws -> CVPixelBuffer {
        var buffer: CVPixelBuffer?
        let attributes: [String: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            kCVPixelBufferMetalCompatibilityKey as String: true,
        ]
        let status = CVPixelBufferCreate(nil, side.width, side.height, side.format, attributes as CFDictionary, &buffer)
        guard status == kCVReturnSuccess, let buffer else { throw PixelTransferRouteError(side: side, status: status) }
        for (key, value) in side.tags { CVBufferSetAttachment(
            buffer,
            key as CFString,
            value as CFString,
            .shouldPropagate
        ) }
        return buffer
    }

    /// Each L008 byte (x + 3y) & 255
    static func ramp(_ buffer: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let base = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
        let pitch = CVPixelBufferGetBytesPerRow(buffer)
        for y in 0 ..< CVPixelBufferGetHeight(buffer) {
            for x in 0 ..< CVPixelBufferGetWidth(buffer) { base[y * pitch + x] = UInt8((x + 3 * y) & 255) }
        }
    }

    static func grayError(_ gray: CVPixelBuffer, _ bgra: CVPixelBuffer) -> Int {
        CVPixelBufferLockBaseAddress(gray, .readOnly)
        CVPixelBufferLockBaseAddress(bgra, .readOnly)
        defer {
            CVPixelBufferUnlockBaseAddress(gray, .readOnly)
            CVPixelBufferUnlockBaseAddress(bgra, .readOnly)
        }
        let l = CVPixelBufferGetBaseAddress(gray)!.assumingMemoryBound(to: UInt8.self)
        let c = CVPixelBufferGetBaseAddress(bgra)!.assumingMemoryBound(to: UInt8.self)
        let lp = CVPixelBufferGetBytesPerRow(gray), cp = CVPixelBufferGetBytesPerRow(bgra)
        var worst = 0
        for y in 0 ..< CVPixelBufferGetHeight(bgra) {
            for x in 0 ..< CVPixelBufferGetWidth(bgra) {
                let luma = Int(l[y * lp + x])
                for channel in 0 ..< 3 { worst = max(worst, abs(Int(c[y * cp + 4 * x + channel]) - luma)) }
            }
        }
        return worst
    }

    /// The pair's transfers, each `runs` times on one session; the route after the last
    public static func run(_ pair: Pair, runs: Int = 5) throws -> Reading {
        var session: VTPixelTransferSession?
        guard VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session) == noErr,
              let session else {
            throw PixelTransferRouteError(side: pair.source, status: -1)
        }
        defer { VTPixelTransferSessionInvalidate(session) }
        let source = try buffer(pair.source)
        if pair.ramp { ramp(source) }
        var reading = Reading()
        var routes: [String] = []
        for side in pair.destinations {
            let destination = try buffer(side)
            for _ in 0 ..< runs where reading.status == noErr {
                reading.status = VTPixelTransferSessionTransferImage(session, from: source, to: destination)
            }
            routes.append(route(session))
            if pair.ramp { reading.grayError = grayError(source, destination) }
        }
        reading.route = Set(routes).count == 1 ? routes[0] : routes.joined(separator: "/")
        let (chain, _) = property(session, "kVTPixelTransferPropertyKey_MostRecentChainDescription")
        if let chain { reading.chain = String(describing: chain) }
        return reading
    }
}

public struct PixelTransferRouteError: Error, CustomStringConvertible {
    let side: PixelTransferRoutes.Side
    let status: Int32

    public var description: String {
        "no \(side.width)x\(side.height) '\(String(format: "%08x", side.format))' buffer or session (\(status))"
    }
}
