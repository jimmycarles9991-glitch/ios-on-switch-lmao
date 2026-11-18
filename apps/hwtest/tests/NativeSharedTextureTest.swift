import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// The other end of the XPC round trip, an anonymous listener in this process: its connection decodes each handle it is
/// sent and encodes it back. An IOSurface encodes only with an NSXPCCoder, so XPC is how a handle crosses processes.
@objc protocol SharedTextureCarrier {
    func carry(_ handle: MTLSharedTextureHandle, reply: @escaping (MTLSharedTextureHandle?) -> Void)
}

final class SharedTextureEcho: NSObject, NSXPCListenerDelegate, SharedTextureCarrier {
    static func interface() -> NSXPCInterface {
        let interface = NSXPCInterface(with: SharedTextureCarrier.self)
        let carry = #selector(SharedTextureCarrier.carry(_:reply:))
        let classes = NSSet(object: MTLSharedTextureHandle.self) as! Set<AnyHashable>
        interface.setClasses(classes, for: carry, argumentIndex: 0, ofReply: false)
        interface.setClasses(classes, for: carry, argumentIndex: 0, ofReply: true)
        return interface
    }

    func listener(_ listener: NSXPCListener, shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        connection.exportedInterface = Self.interface()
        connection.exportedObject = self
        connection.resume()
        return true
    }

    func carry(_ handle: MTLSharedTextureHandle, reply: @escaping (MTLSharedTextureHandle?) -> Void) { reply(handle) }
}

/// Shared texture handles, as the identity's AGX driver makes them: a 100x70 render target (the kernel lays its surface
/// out block-linear at its first import, which renders it) drawn in four colours and a 64x48 sampled texture written by
/// a blit (pitch), both shareable. The rendered one's handle goes through XPC and through a mach port
/// (createMachPort, initWithMachPort:), and each copy opens a texture of the same surface whose texels match the
/// CPU's. A plain texture and a plain IOSurface texture are not shareable and make no handle. The device's registry ID,
/// which a handle finds its device by in every process, is the accelerator service's IORegistry entry ID.
struct NativeSharedTextureTest: NativeObjectTest {
    let id = "metal.native.shared_texture"
    let name = "Shared texture handles: through XPC and a mach port, opened with the texels the GPU wrote"

    static func quarter(_ x: Int, _ y: Int, width: Int, height: Int) -> [UInt8] {
        switch (x < width / 2, y < height / 2) {
        case (true, true): return [255, 0, 0, 255]
        case (false, true): return [0, 255, 0, 255]
        case (true, false): return [0, 0, 255, 255]
        case (false, false): return [255, 255, 255, 255]
        }
    }

    static func gradient(_ x: Int, _ y: Int) -> [UInt8] { [UInt8(x * 4), UInt8(y * 5), UInt8((x ^ y) & 0xff), 255] }

    static func wrong(_ pixels: [UInt8], width: Int, height: Int, _ want: (Int, Int) -> [UInt8]) -> Int {
        (0..<width * height).filter { i in
            Array(pixels.pixel(i % width, i / width, width: width)) != want(i % width, i / width)
        }.count
    }

    /// The accelerator service's IORegistry entry ID (0: IOKit did not answer)
    static func serviceRegistryID() -> UInt64 {
        typealias Matching = @convention(c) (UnsafePointer<CChar>) -> UnsafeMutableRawPointer?
        typealias Service = @convention(c) (mach_port_t, UnsafeMutableRawPointer?) -> mach_port_t
        typealias EntryID = @convention(c) (mach_port_t, UnsafeMutablePointer<UInt64>) -> kern_return_t
        typealias Release = @convention(c) (mach_port_t) -> kern_return_t
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let entryID = dlsym(iokit, "IORegistryEntryGetRegistryEntryID"),
            let release = dlsym(iokit, "IOObjectRelease")
        else { return 0 }
        let accelerator = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("AGXAccelerator"))
        guard accelerator != 0 else { return 0 }
        var id: UInt64 = 0
        let kr = unsafeBitCast(entryID, to: EntryID.self)(accelerator, &id)
        _ = unsafeBitCast(release, to: Release.self)(accelerator)
        return kr == KERN_SUCCESS ? id : 0
    }

    static func overXPC(_ handle: MTLSharedTextureHandle) async -> MTLSharedTextureHandle? {
        let echo = SharedTextureEcho()
        let listener = NSXPCListener.anonymous()
        listener.delegate = echo
        listener.resume()
        let connection = NSXPCConnection(listenerEndpoint: listener.endpoint)
        connection.remoteObjectInterface = SharedTextureEcho.interface()
        connection.resume()
        defer {
            connection.invalidate()
            listener.invalidate()
            withExtendedLifetime(echo) {}
        }
        let back = HandlerValue<MTLSharedTextureHandle?>(nil)
        let done = DispatchSemaphore(value: 0)
        let proxy = connection.remoteObjectProxyWithErrorHandler { _ in done.signal() } as? SharedTextureCarrier
        proxy?.carry(handle) {
            back.value = $0
            done.signal()
        }
        let arrived = await withCheckedContinuation { continuation in
            DispatchQueue.global().async { continuation.resume(returning: done.wait(timeout: .now() + 5) == .success) }
        }
        return arrived ? back.value : nil
    }

    /// Metal's SPI pair: init consumes alloc's object and answers +1 (nil: the port named no surface of a device here)
    static func overMachPort(_ handle: MTLSharedTextureHandle) -> MTLSharedTextureHandle? {
        let create = NSSelectorFromString("createMachPort"), open = NSSelectorFromString("initWithMachPort:")
        let alloc = NSSelectorFromString("alloc")
        guard handle.responds(to: create), MTLSharedTextureHandle.instancesRespond(to: open) else { return nil }
        typealias Create = @convention(c) (AnyObject, Selector) -> mach_port_t
        typealias Alloc = @convention(c) (AnyClass, Selector) -> UnsafeMutableRawPointer?
        typealias Open =
            @convention(c) (UnsafeMutableRawPointer, Selector, mach_port_t) -> Unmanaged<MTLSharedTextureHandle>?
        let port = unsafeBitCast(handle.method(for: create), to: Create.self)(handle, create)
        guard port != 0 else { return nil }
        defer { mach_port_deallocate(mach_task_self_, port) }
        let cls: AnyClass = MTLSharedTextureHandle.self
        guard let memory = unsafeBitCast(MTLSharedTextureHandle.method(for: alloc), to: Alloc.self)(cls, alloc) else {
            return nil
        }
        let made = unsafeBitCast(MTLSharedTextureHandle.instanceMethod(for: open), to: Open.self)(memory, open, port)
        return made?.takeRetainedValue()
    }

    /// The handle through NSKeyedArchiver's secure coding: its outcome, and the handle it decoded if it did
    static func overKeyedArchiver(_ handle: MTLSharedTextureHandle) -> (MTLSharedTextureHandle?, String) {
        do {
            let data = try NSKeyedArchiver.archivedData(withRootObject: handle, requiringSecureCoding: true)
            let back = try NSKeyedUnarchiver.unarchivedObject(ofClass: MTLSharedTextureHandle.self, from: data)
            return (back, back == nil ? "decoded nil" : "ok")
        } catch {
            let error = error as NSError
            return (nil, "\(error.domain)/\(error.code)")
        }
    }

    static func texels(_ gpu: GPU, _ texture: MTLTexture) async throws -> (pixels: [UInt8], path: String) {
        let rowBytes = texture.width * 4
        let out = try gpu.buffer(rowBytes * texture.height)
        let timing = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: texture.width, height: texture.height, depth: 1), to: out,
                destinationOffset: 0, destinationBytesPerRow: rowBytes, destinationBytesPerImage: 0)
            blit.endEncoding()
        }
        return ([UInt8](UnsafeRawBufferPointer(start: out.contents(), count: rowBytes * texture.height)), timing.path)
    }

    static func descriptor(_ width: Int, _ height: Int, _ usage: MTLTextureUsage) -> MTLTextureDescriptor {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
        d.usage = usage
        d.storageMode = .private
        return d
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        if NativeUnsupportedTest.answer(device, "supportsSharedTextureHandles") != 1 {
            work.failures.append("supportsSharedTextureHandles does not answer YES")
        }
        let service = Self.serviceRegistryID()
        metrics.set("registry", String(device.registryID, radix: 16))
        metrics.set("service_registry", String(service, radix: 16))
        if device.registryID == 0 || device.registryID != service {
            work.failures.append("the registry ID is not the accelerator service's IORegistry entry ID")
        }

        let (w, h) = (100, 70)
        guard let rendered = device.makeSharedTexture(descriptor: Self.descriptor(w, h, [.renderTarget, .shaderRead])),
            let blitted = device.makeSharedTexture(descriptor: Self.descriptor(64, 48, .shaderRead))
        else { throw TestFailure("makeSharedTexture returned nil for a one-level 2D RGBA8 texture") }
        rendered.label = "switch.shared.rendered"
        work.objects += [rendered, blitted]
        metrics.set("alloc_rendered", rendered.allocatedSize)
        metrics.set("alloc_blitted", blitted.allocatedSize)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let drawn = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = rendered
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            for (x0, y0) in [(0, 0), (w / 2, 0), (0, h / 2), (w / 2, h / 2)] {
                let c = Self.quarter(x0, y0, width: w, height: h).map { Float($0) / 255 }
                GPU.draw(
                    encoder, corners: GPU.pixelQuad(x0, y0, x0 + w / 2, y0 + h / 2, width: w, height: h),
                    color: SIMD4(c[0], c[1], c[2], c[3]))
            }
            encoder.endEncoding()
        }
        let source = try gpu.buffer((0..<48).flatMap { y in (0..<64).flatMap { x in Self.gradient(x, y) } })
        let copied = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: source, sourceOffset: 0, sourceBytesPerRow: 256, sourceBytesPerImage: 0,
                sourceSize: MTLSize(width: 64, height: 48, depth: 1), to: blitted, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding()
        }
        work.paths += [drawn.path, copied.path]

        guard let handle = rendered.makeSharedTextureHandle(), let blittedHandle = blitted.makeSharedTextureHandle()
        else { throw TestFailure("a shared texture made no handle") }
        if (handle.device as AnyObject?) !== (device as AnyObject) {
            work.failures.append("the handle names another device")
        }
        if handle.label != rendered.label { work.failures.append("the handle's label is \(handle.label ?? "nil")") }
        let (archived, archiver) = Self.overKeyedArchiver(handle)
        metrics.set("keyed_archiver", archiver)
        let xpc = await Self.overXPC(handle)
        let port = Self.overMachPort(handle)
        if xpc == nil { work.failures.append("the handle did not come back through XPC") }
        if port == nil { work.failures.append("no handle from the handle's mach port") }

        let quarters = { (x: Int, y: Int) in Self.quarter(x, y, width: w, height: h) }
        var opened: [(String, MTLSharedTextureHandle?, MTLTexture, (Int, Int) -> [UInt8])] = [
            ("xpc", xpc, rendered, quarters), ("port", port, rendered, quarters),
            ("blitted", blittedHandle, blitted, Self.gradient),
        ]
        if archived != nil { opened.append(("archived", archived, rendered, quarters)) }
        for (what, handle, source, want) in opened {
            guard let handle else { continue }
            guard let texture = device.makeSharedTexture(handle: handle) else {
                work.failures.append("the \(what) handle opened no texture")
                continue
            }
            work.objects.append(texture)
            if !texture.isShareable { work.failures.append("the \(what) texture is not shareable") }
            if texture.label != handle.label { work.failures.append("the \(what) texture's label is not its handle's") }
            if texture.width != source.width || texture.height != source.height || texture.usage != source.usage
                || texture.pixelFormat != source.pixelFormat || texture.storageMode != source.storageMode
            {
                work.failures.append("the \(what) texture's descriptor is not the shared texture's")
            }
            let (pixels, path) = try await Self.texels(gpu, texture)
            work.paths.append(path)
            let bad = Self.wrong(pixels, width: texture.width, height: texture.height, want)
            metrics.set("wrong_\(what)", bad)
            if bad != 0 { work.failures.append("\(bad) texels of the \(what) texture are wrong") }
        }
        if !rendered.isShareable || !blitted.isShareable { work.failures.append("a shared texture is not shareable") }

        let plain = try gpu.texture(.rgba8Unorm, 16, 16)
        let surface = IOSurfaceCreate(
            [
                kIOSurfaceWidth as String: 16, kIOSurfaceHeight as String: 16, kIOSurfaceBytesPerElement as String: 4,
                kIOSurfaceBytesPerRow as String: 128,
            ] as CFDictionary)
        let overSurface = surface.flatMap {
            device.makeTexture(descriptor: Self.descriptor(16, 16, .shaderRead), iosurface: $0, plane: 0)
        }
        let unshared: [(String, MTLTexture?)] = [("plain", plain), ("surface", overSurface)]
        for (what, texture) in unshared {
            guard let texture else { continue }
            if texture.isShareable || texture.makeSharedTextureHandle() != nil {
                work.failures.append("a \(what) texture is shareable")
            }
        }
        let mipmapped = Self.descriptor(64, 64, .shaderRead)
        mipmapped.mipmapLevelCount = 7
        metrics.set("mipmapped", device.makeSharedTexture(descriptor: mipmapped) == nil ? "refused" : "made")
        metrics.set(
            "resource_index",
            (device as AnyObject).responds(to: NSSelectorFromString("newSharedTextureWithHandle:withResourceIndex:")))
        return work
    }
}
