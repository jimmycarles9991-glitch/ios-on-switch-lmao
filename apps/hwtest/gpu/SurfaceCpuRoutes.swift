import Darwin
import Foundation
import HWTestCore
import IOSurface
import Metal

/// gpu.gm20b.readback_routes's surface arms: a 260x260 'BGRA' IOSurface the GPU drew, asked for buffers over its pages
/// and read and written by the CPU routes, with and without an IOSurface lock, through its texture and a view of it
public enum SurfaceCpuRoutes {
    public static let side = 260, row = side * 4

    public struct Result {
        /// Whether a no-copy buffer was made over the surface's pages, over one page inside them, and over pages of the
        /// process's own
        public var surfaceNoCopy = false, pageNoCopy = false, ownNoCopy = false
        /// The no-copy buffer over the surface's pages read as rows, where one was made
        public var noCopyRows: [UInt8]?
        /// newBufferWithIOSurface: (SPI) asked twice: the buffers made; nil where the device has no such method
        public var surfaceBuffers: Int?
        /// The rows each route read: getbytes, view_getbytes (no lock), after_nocopy, locked_getbytes,
        /// view_locked_getbytes (a read-only lock held), view_locked_replace (the texture's read after the view's
        /// replaceRegion under a lock)
        public var rows: [String: [UInt8]] = [:]
    }

    /// The surface, rows of 1040 bytes, and a texture over it a view may reinterpret
    public static func surfaceTexture(_ device: MTLDevice) throws -> (IOSurface, MTLTexture) {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: side, .height: side, .bytesPerElement: 4, .bytesPerRow: row, .pixelFormat: UInt32(0x4247_5241),
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface not made") }
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false
        )
        d.usage = [.renderTarget, .shaderRead, .pixelFormatView]
        d.storageMode = .shared
        guard
            let texture = device.makeTexture(
                descriptor: d, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0
            )
        else { throw TestFailure("no texture over the IOSurface") }
        return (surface, texture)
    }

    /// The routes over `texture`, which holds what the GPU drew; `replaced` is what the view's replaceRegion writes
    public static func run(
        _ device: MTLDevice, _ surface: IOSurface, _ texture: MTLTexture, replaced: [UInt8]
    ) throws -> Result {
        guard let view = texture.makeTextureView(pixelFormat: .bgra8Unorm_srgb) else {
            throw TestFailure("no BGRA8Unorm_sRGB view")
        }
        var out = Result()
        out.rows["getbytes"] = getBytes(texture)
        out.rows["view_getbytes"] = getBytes(view)
        let page = Int(vm_page_size)
        let pages = (surface.allocationSize + page - 1) / page * page
        if let whole = device.makeBuffer(
            bytesNoCopy: surface.baseAddress, length: pages, options: .storageModeShared, deallocator: nil
        )
        {
            out.surfaceNoCopy = true
            out.noCopyRows = rows(UnsafeRawPointer(whole.contents()), pitch: surface.bytesPerRow)
        }
        out.pageNoCopy =
            device.makeBuffer(
                bytesNoCopy: surface.baseAddress + page, length: page, options: .storageModeShared, deallocator: nil
            )
            != nil
        var own: vm_address_t = 0
        if vm_allocate(mach_task_self_, &own, vm_size_t(4 * page), VM_FLAGS_ANYWHERE) == KERN_SUCCESS {
            out.ownNoCopy =
                device.makeBuffer(
                    bytesNoCopy: UnsafeMutableRawPointer(bitPattern: own)!, length: 4 * page,
                    options: .storageModeShared, deallocator: nil
                ) != nil
            vm_deallocate(mach_task_self_, own, vm_size_t(4 * page))
        }
        let spi = NSSelectorFromString("newBufferWithIOSurface:")
        if (device as AnyObject).responds(to: spi) {
            out.surfaceBuffers = (0 ..< 2).filter { _ in
                (device as AnyObject).perform(spi, with: surface)?.takeRetainedValue() is MTLBuffer
            }.count
        }
        out.rows["after_nocopy"] = getBytes(texture)
        surface.lock(options: .readOnly, seed: nil)
        out.rows["view_locked_getbytes"] = getBytes(view)
        out.rows["locked_getbytes"] = getBytes(texture)
        surface.unlock(options: .readOnly, seed: nil)
        surface.lock(options: [], seed: nil)
        replaced.withUnsafeBytes {
            view.replace(
                region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: row
            )
        }
        surface.unlock(options: [], seed: nil)
        out.rows["view_locked_replace"] = getBytes(texture)
        return out
    }

    public static func getBytes(_ texture: MTLTexture) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: row * side)
        out.withUnsafeMutableBytes {
            texture.getBytes(
                $0.baseAddress!, bytesPerRow: row, from: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0
            )
        }
        return out
    }

    static func rows(_ base: UnsafeRawPointer, pitch: Int) -> [UInt8] {
        (0 ..< side).flatMap { [UInt8](UnsafeRawBufferPointer(start: base + $0 * pitch, count: row)) }
    }
}
