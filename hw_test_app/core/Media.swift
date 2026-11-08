import Foundation

public enum VideoCodec: String, CaseIterable, Sendable {
    case h264
    case hevc
    case hevc10 = "hevc_main10"
    case vp9

    public var bitDepth: Int { self == .hevc10 ? 10 : 8 }
    public var container: String { self == .vp9 ? "ivf" : "mp4" }
}

/// A test clip: the media tool encodes it at build time and the app decodes it, both from this table.
public struct Clip: Equatable, Sendable {
    public let codec: VideoCodec
    public let width: Int
    public let height: Int
    public let fps: Int
    public let frames: Int
    public let bitrate: Int

    public var label: String {
        switch height {
        case 2160: return "4k"
        default: return "\(height)p"
        }
    }

    public var name: String { "\(codec.rawValue)_\(label)" }
    public var file: String { "\(name).\(codec.container)" }

    /// The barcode's CRC salt: a frame from another clip fails its CRC instead of passing as the right number.
    public var clipID: UInt16 {
        UInt16(VideoCodec.allCases.firstIndex(of: codec)! << 8 | height / 8 & 0xff)
    }

    public var pattern: Pattern { Pattern(width: width, height: height, clip: clipID) }

    public static let all: [Clip] = VideoCodec.allCases.flatMap { codec in
        [
            Clip(codec: codec, width: 1280, height: 720, fps: 60, frames: 60, bitrate: 1_000_000),
            Clip(codec: codec, width: 1920, height: 1080, fps: 60, frames: 60, bitrate: 2_000_000),
            Clip(codec: codec, width: 3840, height: 2160, fps: 30, frames: 30, bitrate: 4_000_000),
        ]
    }

    public static func named(_ file: String) -> Clip? { all.first { $0.file == file } }
}

public enum StillFormat: String, Sendable {
    case jpeg420 = "jpeg_420"
    case jpeg422 = "jpeg_422"
    case jpeg444 = "jpeg_444"
    case png
    case heic
    case heicGrid = "heic_grid"

    public var fileExtension: String {
        switch self {
        case .jpeg420, .jpeg422, .jpeg444: return "jpg"
        case .png: return "png"
        case .heic, .heicGrid: return "heic"
        }
    }
}

public struct StillImage: Equatable, Sendable {
    public let format: StillFormat
    public let width: Int
    public let height: Int

    public var file: String { "\(format.rawValue).\(format.fileExtension)" }
    public var pattern: Pattern { Pattern(width: width, height: height, clip: 0x5000 | UInt16(width / 16 & 0xfff)) }
    /// PNG is lossless: every sample block must match exactly (rounding of the mean aside).
    public var tolerance: Double { format == .png ? 0.5 : 12 }

    public static let all: [StillImage] = [
        StillImage(format: .jpeg420, width: 4032, height: 3024),
        StillImage(format: .jpeg422, width: 2048, height: 1536),
        StillImage(format: .jpeg444, width: 2048, height: 1536),
        StillImage(format: .png, width: 1024, height: 768),
        // ImageIO tiles a 1024x768 HEIC into a grid already; at 640x480 it writes a single coded image.
        StillImage(format: .heic, width: 640, height: 480),
        StillImage(format: .heicGrid, width: 4032, height: 3024),
    ]

    public static func named(_ file: String) -> StillImage? { all.first { $0.file == file } }
}
