import CoreImage
import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// What the facade answers itself (drivers/TegraGM20BMetal TegraGM20BMetalLibrary.mm,
/// TegraGM20BMetalLibraries.mm, TegraGM20BMetalDevice.mm, TegraGM20BMetalYcbcrTargets.mm): a library's function names, extern names and
/// type from its own metallib; a library Metal would not load (a missing file, bytes that are no metallib) as Metal's
/// error; a buffer the kernel has no memory for, and one over pages it cannot wire, as nil; an uncompressed texture's
/// plain layout (TegraGM20BMetalPlainLayout.mm), none for a heap's; a two-plane YCbCr target's state drawn by raw; and a Core
/// Image filter chain, whose linked program's extern functions the library makes from its own list. No command buffer
/// may fail, Core Image's included (failed_cbs=, failed_why=), nor any call be skipped (skipped_calls=).
struct OwnAnswersTest: HWTest {
    let id = "gpu.gm20b.own_answers"
    let name = "Library names, unloadable libraries, no memory and a YCbCr target state, answered by the facade"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []

        let names = gpu.library.functionNames
        metrics.set("function_names", names.count)
        let externs =
            (gpu.library as AnyObject).perform(NSSelectorFromString("externFunctionNames"))?
                .takeUnretainedValue() as? [String]
        metrics.set("extern_names", externs?.count ?? -1)
        if names.isEmpty || externs == nil || gpu.library.type != .executable { wrong.append("library") }

        let absent = URL(fileURLWithPath: "/private/var/Switch/absent-\(UUID().uuidString).metallib")
        let missing = (try? gpu.device.makeLibrary(URL: absent)) == nil
        let text = "not a metallib".data(using: .utf8)!
        let garbage = text.withUnsafeBytes { (try? gpu.device.makeLibrary(data: DispatchData(bytes: $0))) == nil }
        if !missing { wrong.append("missing_file") }
        if !garbage { wrong.append("garbage_data") }

        let tooLong = gpu.device.makeBuffer(length: gpu.device.maxBufferLength + 4096, options: .storageModeShared)
        if tooLong != nil { wrong.append("no_memory") }
        let page = Int(getpagesize()), length = 4 * page
        let mapped = mmap(nil, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0)
        guard let unmapped = mapped, unmapped != MAP_FAILED else { throw TestFailure("mmap failed") }
        munmap(unmapped, length)
        let unwired = gpu.device.makeBuffer(
            bytesNoCopy: unmapped, length: length, options: .storageModeShared, deallocator: nil
        )
        if unwired != nil { wrong.append("unwired_pages") }

        let layoutSelector = NSSelectorFromString("newTextureLayoutWithDescriptor:isHeapOrBufferBacked:")
        typealias Layout = @convention(c) (AnyObject, Selector, MTLTextureDescriptor, Bool) -> Unmanaged<AnyObject>?
        let layoutOf = unsafeBitCast((gpu.device as AnyObject).method(for: layoutSelector), to: Layout.self)
        let plain = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: 64, height: 32, mipmapped: false
        )
        let layout = layoutOf(gpu.device as AnyObject, layoutSelector, plain, false)?.takeRetainedValue()
        let layoutBytes = (layout?.value(forKey: "size") as? NSNumber)?.intValue ?? -1
        metrics.set("plain_layout_bytes", layoutBytes)
        if layoutBytes != 64 * 32 * 4 { wrong.append("plain_layout") }
        if layoutOf(gpu.device as AnyObject, layoutSelector, plain, true) != nil { wrong.append("backed_layout") }

        let state = try gpu.render(fragment: "texel_values", format: MTLPixelFormat(rawValue: 500)!)
        metrics.set("ycbcr_state", String(describing: type(of: state as AnyObject)))

        let ci = CIContext(mtlDevice: gpu.device, options: [.cacheIntermediates: false])
        let flat = CIImage(color: CIColor(red: 0.2, green: 0.4, blue: 0.6)).cropped(
            to: CGRect(x: 0, y: 0, width: 64, height: 64)
        )
        let chain = flat.applyingFilter("CIColorInvert").applyingGaussianBlur(sigma: 2)
        var pixel = [UInt8](repeating: 0, count: 4)
        ci.render(
            chain.cropped(to: CGRect(x: 32, y: 32, width: 1, height: 1)), toBitmap: &pixel, rowBytes: 4,
            bounds: CGRect(x: 32, y: 32, width: 1, height: 1), format: .RGBA8,
            colorSpace: CGColorSpace(name: CGColorSpace.sRGB)
        )
        metrics.set("core_image_rgb", pixel.prefix(3).map(String.init).joined(separator: ","))
        // inverted in Core Image's linear working space: 1 - linear(0.2, 0.4, 0.6), back to sRGB
        if !zip(pixel.prefix(3), [251, 239, 215]).allSatisfy({ abs(Int($0) - $1) <= 2 }) { wrong.append("core_image") }

        let after = GM20BFacade.counters(of: gpu.device)
        let checks = FailedCounts.judgedFramework(before: before, after: after, own: gpu.paths, &metrics)
        wrong += checks.filter { !$0.1 }.map(\.0)
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
