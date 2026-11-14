import CoreGraphics
import Darwin
import Foundation
import HWTestCore
import HWTestGPU

/// gpu.raw.icon_corners' IconServices arms: Settings' icon as iconservicesagent draws it at the App Library's 27 and
/// 68 points, scale 2, and the agent's rendered-icon cache.
///
/// The agent keeps its renders in systemgroup.com.apple.lsd.iconscache's Library/Caches/com.apple.IconsCache and resets
/// them at launch unless __cache_info__ holds the system's ProductBuildVersion (21H461's and 24B119's
/// -[ISMutableIconCache initWithURL:]); a deploy that stages another GPU build removes that value
/// (tools/deploy/icon_cache.py). `icon_cache_build`: the system's ProductBuildVersion, the key; `icon_cache_info_build`
/// and `icon_cache_reset_this_boot`: what __cache_info__ holds and whether the agent wrote it since this boot (a reset),
/// or why the app cannot read it. The cached images are read before anything renders uncached (an uncached render can
/// be stored): `icon<pt>_cached_fresh_diff` counts the pixels where the cached image is more than `codes` off an
/// uncached render by this GPU build, 0 when the cache holds this build's icons. `icon27_corner_beyond` and
/// `icon27_cached_corner_beyond`: the uncached and cached small icon's corners against the uncached large one reduced
/// to its size. FAIL: the uncached small icon's corners beyond RenderBoxCorner.tolerance, a cached image that is not
/// this build's, or IconServices' calls absent (`icon_api=`).
enum IconServicesCache {
    static let group = "systemgroup.com.apple.lsd.iconscache"
    static let info = "Library/Caches/com.apple.IconsCache/__cache_info__"
    static let codes = 2
    static let iconRadius = 0.2237

    struct Image {
        let rgba: [UInt8]
        let side: Int

        var alpha: [Double] { stride(from: 3, to: rgba.count, by: 4).map { Double(rgba[$0]) / 255 } }
    }

    static func measure(_ metrics: inout Metrics, _ wrong: inout [String]) {
        metrics.set("icon_cache_build", systemBuild() ?? "unreadable")
        let (stored, resetThisBoot) = cacheInfo()
        metrics.set("icon_cache_info_build", stored)
        if let resetThisBoot { metrics.set("icon_cache_reset_this_boot", resetThisBoot) }
        let cached = [27: icon(points: 27, ignoreCache: false), 68: icon(points: 68, ignoreCache: false)]
        guard let large = icon(points: 68, ignoreCache: true), let small = icon(points: 27, ignoreCache: true) else {
            metrics.set("icon_api", "absent")
            wrong.append("icon_api")
            return
        }
        for (points, fresh) in [(27, small), (68, large)] {
            guard let image = cached[points] ?? nil, image.side == fresh.side else {
                metrics.set("icon\(points)_cached_fresh_diff", "unmatched")
                wrong.append("icon\(points)_cached")
                continue
            }
            let diff = differing(image, fresh)
            metrics.set("icon\(points)_cached_fresh_diff", diff)
            if diff > 0 { wrong.append("icon\(points)_cached") }
        }
        metrics.set("icon68_side", large.side)
        for (name, image) in [("icon27", small), ("icon27_cached", cached[27] ?? nil)] {
            guard let image else { continue }
            metrics.set("\(name)_side", image.side)
            let corner = judge(image, against: large)
            metrics.set("\(name)_corner_beyond", corner.beyond)
            metrics.set("\(name)_corner_max_diff", corner.maxDiff)
            metrics.set("\(name)_partial", corner.partial)
            metrics.set("\(name)_ref_partial", corner.referencePartial)
            if name == "icon27", corner.beyond > 0 { wrong.append(name) }
        }
    }

    static func differing(_ a: Image, _ b: Image) -> Int {
        (0..<(a.rgba.count / 4)).filter { p in
            (0..<4).contains { abs(Int(a.rgba[p * 4 + $0]) - Int(b.rgba[p * 4 + $0])) > codes }
        }.count
    }

    static func judge(_ small: Image, against large: Image) -> RenderBoxCorner.Cell {
        let reference = RenderBoxCorner.reduce(large.alpha, from: large.side, to: small.side)
        let toCodes = { (alpha: [Double]) in alpha.map { UInt8(($0 * 255).rounded()) } }
        return RenderBoxCorner.judge(
            toCodes(small.alpha), toCodes(reference), side: small.side, radius: Double(small.side) * iconRadius)
    }

    static func systemBuild() -> String? {
        let plist = NSDictionary(contentsOfFile: "/System/Library/CoreServices/SystemVersion.plist")
        return plist?["ProductBuildVersion"] as? String
    }

    typealias GroupPath =
        @convention(c) (UnsafeRawPointer?, UnsafePointer<CChar>, UnsafeMutablePointer<UInt64>) ->
        UnsafeMutablePointer<CChar>?

    /// __cache_info__'s ProductBuildVersion ("absent" without one) and whether it was written since this boot, or why
    /// it cannot be read
    static func cacheInfo() -> (String, Bool?) {
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "container_system_group_path_for_identifier")
        else { return ("unreadable:no_container_lookup", nil) }
        var error: UInt64 = 1
        guard let path = unsafeBitCast(symbol, to: GroupPath.self)(nil, group, &error) else {
            return ("unreadable:container_error_\(error)", nil)
        }
        let url = URL(fileURLWithPath: String(cString: path)).appendingPathComponent(info)
        free(path)
        let values: [String: Any]
        let written: Date
        do {
            let data = try Data(contentsOf: url)
            values = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] ?? [:]
            written =
                try url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate ?? .distantPast
        } catch {
            return ("unreadable:" + "\((error as NSError).domain)_\((error as NSError).code)", nil)
        }
        let boot = Date(timeIntervalSinceNow: -ProcessInfo.processInfo.systemUptime)
        return (values["ProductBuildVersion"] as? String ?? "absent", written > boot)
    }

    typealias InitSizeScale = @convention(c) (AnyObject, Selector, CGSize, CGFloat) -> AnyObject
    typealias SetBool = @convention(c) (AnyObject, Selector, Bool) -> Void
    typealias ImageFor = @convention(c) (AnyObject, Selector, AnyObject) -> Unmanaged<CGImage>?

    /// Settings' icon as iconservicesagent draws it at `points` and scale 2, nil where IconServices lacks what this asks
    static func icon(points: CGFloat, ignoreCache: Bool) -> Image? {
        guard let iconType = NSClassFromString("ISIcon") as? NSObject.Type,
            let descriptorType = NSClassFromString("ISImageDescriptor") as? NSObject.Type
        else { return nil }
        let initSize = NSSelectorFromString("initWithSize:scale:"), ignore = NSSelectorFromString("setIgnoreCache:")
        let prepare = NSSelectorFromString("prepareImageForDescriptor:")
        let cgImage = NSSelectorFromString("CGImageForDescriptor:")
        guard descriptorType.instancesRespond(to: initSize), descriptorType.instancesRespond(to: ignore),
            iconType.instancesRespond(to: prepare), iconType.instancesRespond(to: cgImage),
            let made = iconType.perform(NSSelectorFromString("alloc"))?.takeUnretainedValue() as? NSObject,
            let icon = made.perform(
                NSSelectorFromString("initWithBundleIdentifier:"), with: "com.apple.Preferences")?
                .takeUnretainedValue() as? NSObject,
            let alloc = descriptorType.perform(NSSelectorFromString("alloc"))?.takeUnretainedValue()
        else { return nil }
        let descriptor = unsafeBitCast(descriptorType.instanceMethod(for: initSize), to: InitSizeScale.self)(
            alloc, initSize, CGSize(width: points, height: points), 2)
        unsafeBitCast(descriptorType.instanceMethod(for: ignore), to: SetBool.self)(descriptor, ignore, ignoreCache)
        _ = icon.perform(prepare, with: descriptor)
        guard
            let image = unsafeBitCast(iconType.instanceMethod(for: cgImage), to: ImageFor.self)(
                icon, cgImage, descriptor)?
                .takeUnretainedValue(), image.width == image.height
        else { return nil }
        return Image(rgba: rgba(of: image), side: image.width)
    }

    static func rgba(of image: CGImage) -> [UInt8] {
        let w = image.width, h = image.height
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        bytes.withUnsafeMutableBytes { raw in
            guard
                let context = CGContext(
                    data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                    space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return }
            context.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        }
        return bytes
    }
}
