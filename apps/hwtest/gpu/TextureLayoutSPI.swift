import Darwin
import Foundation
import HWTestCore
import Metal

/// Metal's texture layout SPI, as Core Animation and CoreUI use it for compressed images: the device's layout object
/// (newTextureLayoutWithDescriptor:isHeapOrBufferBacked:) sizes the memory and lays the blocks out in it, and
/// newTextureWithBytesNoCopy:length:descriptor:deallocator: wraps that memory as a texture with no copy
/// (drivers/TegraGM20BMetal/raw/runtime/RawMetalNoCopy.mm on the Switch).
public struct TextureLayoutSPI {
    typealias NewLayout = @convention(c) (AnyObject, Selector, MTLTextureDescriptor, Bool) -> Unmanaged<AnyObject>?
    typealias Count = @convention(c) (AnyObject, Selector) -> Int
    typealias CopyIn =
        @convention(c) (
            AnyObject, Selector, UnsafeRawPointer, Int, Int, Int, UnsafeMutableRawPointer, Int, Int, MTLRegion
        ) -> Void
    public typealias Deallocator = @convention(block) (UnsafeMutableRawPointer, Int) -> Void
    typealias NewNoCopy =
        @convention(c) (AnyObject, Selector, UnsafeMutableRawPointer, Int, MTLTextureDescriptor, Deallocator?) ->
        Unmanaged<AnyObject>?

    static let newLayout = NSSelectorFromString("newTextureLayoutWithDescriptor:isHeapOrBufferBacked:")
    static let newNoCopy = NSSelectorFromString("newTextureWithBytesNoCopy:length:descriptor:deallocator:")
    static let copyIn = NSSelectorFromString(
        "copyFromLinearBytes:linearOffset:linearBytesPerRow:linearBytesPerImage:toTextureMemory:textureSlice:"
            + "textureLevel:textureRegion:")

    public let layout: NSObject
    public let size: Int
    public let alignment: Int
    public let className: String

    /// The device's layout of `descriptor` for memory the CPU fills; nil where the device has no such SPI or refuses.
    public init?(_ device: MTLDevice, _ descriptor: MTLTextureDescriptor) {
        guard let object = device as? NSObject, object.responds(to: Self.newLayout),
            let made = unsafeBitCast(object.method(for: Self.newLayout), to: NewLayout.self)(
                object, Self.newLayout, descriptor, false)?.takeRetainedValue() as? NSObject,
            made.responds(to: Self.copyIn)
        else { return nil }
        layout = made
        className = NSStringFromClass(type(of: made))
        let count = { (name: String) -> Int in
            let selector = NSSelectorFromString(name)
            return unsafeBitCast(made.method(for: selector), to: Count.self)(made, selector)
        }
        size = count("size")
        alignment = count("alignment")
    }

    /// Lays `region`'s blocks (rows `rowBytes` apart in `bytes`) out in `memory`, in layer `slice`.
    public func copy(
        _ bytes: [UInt8], rowBytes: Int, region: MTLRegion, slice: Int = 0, to memory: UnsafeMutableRawPointer
    ) {
        bytes.withUnsafeBytes {
            unsafeBitCast(layout.method(for: Self.copyIn), to: CopyIn.self)(
                layout, Self.copyIn, $0.baseAddress!, 0, rowBytes, 0, memory, slice, 0, region)
        }
    }

    /// A texture over `length` bytes at `memory` with no copy; nil where the device has no such SPI or refuses.
    public static func noCopyTexture(
        _ device: MTLDevice, _ memory: UnsafeMutableRawPointer, length: Int, _ descriptor: MTLTextureDescriptor,
        deallocator: Deallocator?
    ) -> MTLTexture? {
        guard let object = device as? NSObject, object.responds(to: newNoCopy) else { return nil }
        let made = unsafeBitCast(object.method(for: newNoCopy), to: NewNoCopy.self)(
            object, newNoCopy, memory, length, descriptor, deallocator)
        return made?.takeRetainedValue() as? MTLTexture
    }
}

/// Zeroed memory `bytes` long from a 16 KiB page boundary, freed by the texture made over it (or by `release` if none is).
public struct PageAlignedMemory {
    public let base: UnsafeMutableRawPointer
    public let bytes: Int

    public init(bytes: Int) throws {
        var allocation: UnsafeMutableRawPointer?
        guard posix_memalign(&allocation, 16384, bytes) == 0, let memory = allocation else {
            throw TestFailure("posix_memalign(16384, \(bytes)) failed")
        }
        memory.initializeMemory(as: UInt8.self, repeating: 0, count: bytes)
        base = memory
        self.bytes = bytes
    }

    public var deallocator: TextureLayoutSPI.Deallocator {
        let base = self.base
        return { _, _ in free(base) }
    }

    public func release() { free(base) }
}
