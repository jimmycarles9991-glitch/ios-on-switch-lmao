import Foundation

/// IVF, libvpx's own container: a 32-byte file header, then per frame a 12-byte header (size, pts) and the frame.
/// The VP9 clips use it because AVFoundation has no VP9 demuxer on iOS 17; the app feeds the frames to
/// VideoToolbox itself.
public enum IVF {
    public struct Header: Equatable, Sendable {
        public let fourcc: String
        public let width: Int
        public let height: Int
        public let rate: Int
        public let scale: Int
        public let frameCount: Int

        public init(fourcc: String, width: Int, height: Int, rate: Int, scale: Int, frameCount: Int) {
            self.fourcc = fourcc
            self.width = width
            self.height = height
            self.rate = rate
            self.scale = scale
            self.frameCount = frameCount
        }
    }

    public struct Frame: Equatable, Sendable {
        public let pts: UInt64
        public let data: Data

        public init(pts: UInt64, data: Data) {
            self.pts = pts
            self.data = data
        }
    }

    public enum ParseError: Error, Equatable {
        case notIVF
        case truncated(frame: Int)
    }

    public static func write(_ header: Header, frames: [Frame]) -> Data {
        var out = Data("DKIF".utf8)
        append(&out, UInt16(0))
        append(&out, UInt16(32))
        out += Data(header.fourcc.utf8.prefix(4))
        append(&out, UInt16(header.width))
        append(&out, UInt16(header.height))
        append(&out, UInt32(header.rate))
        append(&out, UInt32(header.scale))
        append(&out, UInt32(frames.count))
        append(&out, UInt32(0))
        for frame in frames {
            append(&out, UInt32(frame.data.count))
            append(&out, frame.pts)
            out += frame.data
        }
        return out
    }

    public static func parse(_ data: Data) throws -> (Header, [Frame]) {
        let bytes = [UInt8](data)
        guard bytes.count >= 32, bytes[0..<4] == [0x44, 0x4B, 0x49, 0x46] else { throw ParseError.notIVF }
        let headerSize = Int(read(bytes, 6, as: UInt16.self))
        let header = Header(
            fourcc: String(decoding: bytes[8..<12], as: UTF8.self),
            width: Int(read(bytes, 12, as: UInt16.self)),
            height: Int(read(bytes, 14, as: UInt16.self)),
            rate: Int(read(bytes, 16, as: UInt32.self)),
            scale: Int(read(bytes, 20, as: UInt32.self)),
            frameCount: Int(read(bytes, 24, as: UInt32.self))
        )
        var frames: [Frame] = []
        var at = headerSize
        while at < bytes.count {
            guard at + 12 <= bytes.count else { throw ParseError.truncated(frame: frames.count) }
            let size = Int(read(bytes, at, as: UInt32.self))
            let pts = read(bytes, at + 4, as: UInt64.self)
            guard at + 12 + size <= bytes.count else { throw ParseError.truncated(frame: frames.count) }
            frames.append(Frame(pts: pts, data: Data(bytes[(at + 12)..<(at + 12 + size)])))
            at += 12 + size
        }
        return (header, frames)
    }

    static func append<T: FixedWidthInteger>(_ data: inout Data, _ value: T) {
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }

    static func read<T: FixedWidthInteger>(_ bytes: [UInt8], _ at: Int, as: T.Type) -> T {
        var value: T = 0
        for index in (0..<MemoryLayout<T>.size).reversed() { value = value << 8 | T(bytes[at + index]) }
        return value
    }
}
