import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import MachO
import Metal

/// What a widget needs from the system beyond its own drawing (boot 15: GameCenter's widget never archived, and
/// TVWidgetExtension was jetsammed at 30 MB half a second after its first draw).
/// - WidgetKit's archiver sizes its image limit for a 2x screen: the limit's helper reads no -scale
///   (switch_ui_scale_sites_24B119.h). At the display's 1.25 it refused GameCenter's 310 x 310 artwork.
/// - A widget's draw, a 305 x 305 target cleared and drawn once, runs on raw, and a chunk the raw device makes for it
///   is no larger than what the device already held, its first size (a sixteenth of the render server's 4 MiB) or the
///   target (raw_gpu.cpp chunkSize). A fixed 4 MiB chunk breaks that while the app holds less; once it holds 4 MiB the
///   bound holds either way, so the boot's jetsam lines for widget extensions remain the check of what they hold.
struct WidgetsRenderTest: HWTest {
    let id = "system.widgets_render"
    let name = "Widgets archive 2x images and draw on raw in proportion"
    let subsystem = Subsystem.system

    static let side = 305
    static let firstChunk = 256 << 10
    /// The target's bytes, twice over for a block-linear image's padding, in 16 KiB pages
    static let targetBound = (2 * side * side * 4 + 0x3fff) & ~0x3fff

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var failures: [String] = []
        // the site is 24B119's; 21H461's WidgetKit is not patched
        let twentySeven = ProcessInfo.processInfo.isOperatingSystemAtLeast(
            OperatingSystemVersion(majorVersion: 27, minorVersion: 0, patchVersion: 0))
        if twentySeven, let limit = Self.archiveImageLimit() {
            metrics.set("limit_stock", limit.stock)
            metrics.set("limit_2x", limit.patched)
            if limit.stock > 0 {
                failures.append("WidgetKit's archive image limit still reads the display's scale")
            } else if limit.patched != 1 {
                failures.append("WidgetKit's archive image limit was not found once (\(limit.patched))")
            }
        } else {
            metrics.set("limit_stock", "-")
        }

        let gpu = try GPU.make()
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let held = RawFacade.memory(of: gpu.device)
        let before = Self.footprint()
        let target = try gpu.texture(.bgra8Unorm, Self.side, Self.side)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            GPU.draw(
                encoder, corners: GPU.quad(x0: -0.5, y0: -0.5, x1: 0.5, y1: 0.5, z: 0), color: SIMD4(1, 0.5, 0, 1))
            encoder.endEncoding()
        }
        let grown = Self.footprint() &- before
        metrics.set("dev", RawFacade.kind(of: gpu.device))
        metrics.set("path", timing.path)
        metrics.set("grown_kb", Int(Int64(bitPattern: grown) >> 10))
        if RawFacade.kind(of: gpu.device) == "raw" && timing.path != "raw" {
            failures.append("the widget's draw went \(timing.path)")
        }
        if let pool = held["pool"], let now = RawFacade.memory(of: gpu.device)["pool"] {
            let bound = max(Self.firstChunk, pool, Self.targetBound)
            metrics.set("pool_kb", now >> 10)
            metrics.set("pool_grown_kb", (now - pool) >> 10)
            if now - pool > bound {
                failures.append("the raw pool grew by \((now - pool) >> 10) KiB for the widget, over \(bound >> 10)")
            }
        }
        let drawn = try await gpu.raster(target, .bgra8Unorm)
        let centre = drawn.pixel(Self.side / 2, Self.side / 2), corner = drawn.pixel(0, 0)
        metrics.set("centre", drawn.shown(centre))
        if abs(centre.reduce(0, +) - 2.5) > 0.02 {
            failures.append("the quad's centre is \(drawn.shown(centre)), not the solid colour")
        }
        if corner.reduce(0, +) != 0 { failures.append("the cleared corner is \(drawn.shown(corner))") }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.joined(separator: "; "), metrics)
    }

    static func footprint() -> UInt64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return status == KERN_SUCCESS ? info.phys_footprint : 0
    }

    /// The limit helper's words in WidgetKit's text: bl size; mov v10.16b, v0.16b; mov v11.16b, v1.16b; mov x0, x20;
    /// then a bl to -scale (stock) or fmov d0, #2.0 (the site), fmul d10, d10, d0, mov x0, x20 and again. Nil where
    /// WidgetKit is not loaded or holds no such helper.
    static func archiveImageLimit() -> (stock: Int, patched: Int)? {
        guard dlopen("/System/Library/Frameworks/WidgetKit.framework/WidgetKit", RTLD_NOW) != nil else { return nil }
        for image in 0..<_dyld_image_count() {
            guard let name = _dyld_get_image_name(image),
                String(cString: name).hasSuffix("/WidgetKit.framework/WidgetKit"),
                let header = _dyld_get_image_header(image)
            else { continue }
            var size: UInt = 0
            let text = header.withMemoryRebound(to: mach_header_64.self, capacity: 1) {
                getsectiondata($0, "__TEXT", "__text", &size)
            }
            guard let text else { return nil }
            let words = UnsafeBufferPointer(
                start: UnsafeRawPointer(text).assumingMemoryBound(to: UInt32.self), count: Int(size) / 4)
            let found = count(words)
            return found.stock + found.patched > 0 ? found : nil
        }
        return nil
    }

    static func count(_ w: UnsafeBufferPointer<UInt32>) -> (stock: Int, patched: Int) {
        let fmovTwo: UInt32 = 0x1e60_1000
        let isBl = { (word: UInt32) in word >> 26 == 0x25 }
        var stock = 0
        var patched = 0
        guard w.count > 8 else { return (0, 0) }
        for i in 1..<(w.count - 6)
        where w[i] == 0x4ea0_1c0a && w[i + 1] == 0x4ea1_1c2b && w[i + 2] == 0xaa14_03e0 && w[i + 4] == 0x1e60_094a
            && w[i + 5] == 0xaa14_03e0 && isBl(w[i - 1])
        {
            if isBl(w[i + 3]) && isBl(w[i + 6]) {
                stock += 1
            } else if w[i + 3] == fmovTwo && w[i + 6] == fmovTwo {
                patched += 1
            }
        }
        return (stock, patched)
    }
}
