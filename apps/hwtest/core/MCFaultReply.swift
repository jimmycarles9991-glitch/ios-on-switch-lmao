import Foundation

/// TegraVIC's MC fault probe reply (drivers/TegraVIC/SwitchVICMcFaultProbe.h, little-endian, 32 bytes): a VIC copy read
/// a page VIC's directory leaves empty, and the MC error interrupt was to deliver its SMMU faults.
public struct MCFaultReply: Sendable, Equatable {
    public static let magic: UInt32 = 0x564D_4346 // 'VMCF'
    public static let size = 32
    static let decerrEmem: UInt32 = 1 << 6
    static let smmuFault: UInt32 = 1 << 10
    static let smmuErrType: UInt32 = 6
    static let vicRead: UInt32 = 0x6C
    static let expectedBytes: UInt32 = 0x10000

    public let status: Int32
    public let iova: UInt32
    public let attached: Bool
    public let taken: Int
    public let intstatus: UInt32
    public let errStatus: UInt32
    public let address: UInt32

    /// The request: magic, 0.
    public static let request: [UInt8] = [magic, 0].flatMap { word in
        (0 ..< 4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) }
    }

    public init?(_ bytes: [UInt8]) {
        guard bytes.count == Self.size, Self.word(bytes, 0) == Self.magic else { return nil }
        status = Int32(bitPattern: Self.word(bytes, 4))
        iova = Self.word(bytes, 8)
        attached = Self.word(bytes, 12) != 0
        taken = Int(Self.word(bytes, 16))
        intstatus = Self.word(bytes, 20)
        errStatus = Self.word(bytes, 24)
        address = Self.word(bytes, 28)
    }

    /// Why the interrupt is not proven to deliver, or nil: one SMMU fault at least, the first by csr_vicsrd at the page
    /// the copy read.
    public var failure: String? {
        if status == -95 { return "tegra_vic_build refused the copy" }
        if !attached { return "the MC error interrupt was never attached" }
        if taken == 0 { return "the copy raised no SMMU fault (nothing latched)" }
        let inRange = address >= iova && address - iova < Self.expectedBytes
        let kindOk = intstatus == Self.smmuFault || intstatus == (Self.smmuFault | Self.decerrEmem)
        let errTypeOk = ((errStatus >> 28) & 7) == Self.smmuErrType
        guard kindOk, errTypeOk, errStatus & 0xFF == Self.vicRead, inRange else {
            return "the first delivered error is not the copy's fault"
        }
        return nil
    }

    static func word(_ bytes: [UInt8], _ at: Int) -> UInt32 {
        (0 ..< 4).reduce(0) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }
    }
}
