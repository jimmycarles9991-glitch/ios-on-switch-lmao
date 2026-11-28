import Darwin
import HWTestGPU
import IOSurface

extension GPUConnection {
    public static let boImportMethod: UInt32 = 3
    public static let boFreeMethod: UInt32 = 4
    public static let surfaceLayoutInfoMethod: UInt32 = 24
    public static let importGpuReads: UInt32 = 1

    /// The kernel's layout of `surface` (TegraGM20BAbiSurfaceLayoutInfo.layout: 0 pitch, 1 block-linear, 2 block-linear
    /// at another shape), asked without writes through an import of this connection's own, freed after.
    public func surfaceLayout(_ surface: IOSurface) -> (kern_return_t, UInt32) {
        let page = Int(getpagesize())
        let address = UInt64(UInt(bitPattern: surface.baseAddress))
        let bytes = UInt64((surface.allocationSize + page - 1) / page * page)
        let id = IOSurfaceGetID(unsafeBitCast(surface, to: IOSurfaceRef.self))
        let (imported, reply) = call(
            Self.boImportMethod, Self.words(split(address) + split(bytes) + [id, Self.importGpuReads]), outputSize: 16
        )
        guard imported == KERN_SUCCESS else { return (imported, 0) }
        let handle = Self.uint64(reply, 0)
        defer { _ = call(Self.boFreeMethod, Self.words(split(handle)), outputSize: 0) }
        let shape = [surface.width, surface.height, surface.bytesPerElement, surface.bytesPerRow].map { UInt32($0) }
        let (asked, info) = call(
            Self.surfaceLayoutInfoMethod, Self.words(split(handle) + shape + [0, 0]), outputSize: 48
        )
        return (asked, asked == KERN_SUCCESS ? Self.uint32(info, 0) : 0)
    }

    private func split(_ value: UInt64) -> [UInt32] { [UInt32(truncatingIfNeeded: value), UInt32(value >> 32)] }
}
