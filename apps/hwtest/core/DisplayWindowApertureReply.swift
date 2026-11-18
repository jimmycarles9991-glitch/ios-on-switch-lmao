import Foundation

/// The display's window A aperture check (drivers/NativeMobileDisplay/DisplayWindowProbeABI.h op 3, SwitchWindowAperture.h),
/// little-endian: which of the words window A's completion flip writes read back the same through window A's own
/// aperture and the WINDOW_HEADER copy, both ways, and went back to their values.
public struct DisplayWindowApertureReply: Sendable, Equatable {
    public static let op: UInt32 = 3
    public static let size = 24

    public let error: Int32
    public let words: UInt32
    public let aliased: UInt32
    public let restored: UInt32

    public init?(_ bytes: [UInt8]) {
        guard bytes.count == Self.size, DisplayWindowProbeReply.word(bytes, 0) == DisplayWindowProbeReply.magic else {
            return nil
        }
        error = Int32(bitPattern: DisplayWindowProbeReply.word(bytes, 4))
        words = DisplayWindowProbeReply.word(bytes, 8)
        aliased = DisplayWindowProbeReply.word(bytes, 12)
        restored = DisplayWindowProbeReply.word(bytes, 16)
    }

    /// Every word aliased and put back.
    public var alias: Bool { error == 0 && words != 0 && aliased == words && restored == words }
}
