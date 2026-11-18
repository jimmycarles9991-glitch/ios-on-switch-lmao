import CoreGraphics
import Darwin
import Foundation
import HWTestCore
import HWTestGPU

/// gpu.raw.icon_corners' IconServices arms: Settings' icon as iconservicesagent draws it at the App Library's 27 and
/// 68 points, scale 2, and the agent's rendered-icon cache; and the requests the icon_* cases make.
///
/// The agent keeps its renders in systemgroup.com.apple.lsd.iconscache's Library/Caches/com.apple.IconsCache, 24B119's
/// in a salt folder under it (-[ISDefaults cacheURL]: a UUID of IconServices' build time, the BuildVersion and the
/// UniqueDeviceID), and resets them at launch unless __cache_info__ holds the system's ProductBuildVersion (21H461's and
/// 24B119's -[ISMutableIconCache initWithURL:]); after a deploy that stages another GPU build the kernel removes
/// __cache_info__ at boot (kernel/switch/boot/icon_cache.c). `icon_cache_build`: the system's ProductBuildVersion, the key; `icon_cache_info_build`
/// and `icon_cache_reset_this_boot`: what __cache_info__ holds and whether the agent wrote it since this boot (a reset),
/// or why the app cannot read it; `icon_cache_info_at`: top, salt or none. The cached images are read before anything
/// renders uncached (an uncached render can be stored): `icon<pt>_cached_fresh_diff` counts the pixels where the cached
/// image is more than `codes` off an uncached render by this GPU build, 0 when the cache holds this build's icons.
/// `icon_placeholder`: the uncached render is the placeholder IconServices makes when the agent refuses a request
/// (`Rejecting generation request from <pid>`), so it is no render of Settings at all. `icon27_corner_beyond` and
/// `icon27_cached_corner_beyond`: the uncached and cached small icon's corners against the uncached large one reduced
/// to its size. FAIL: the uncached small icon's corners beyond RenderBoxCorner.tolerance, a cached image that is not
/// this build's, a placeholder, or IconServices' calls absent (`icon_api=`).
enum IconServicesCache {
    static let group = "systemgroup.com.apple.lsd.iconscache"
    static let cache = "Library/Caches/com.apple.IconsCache"
    static let info = "__cache_info__"
    static let codes = 2
    static let iconRadius = 0.2237
    static let settings = IconSource.bundle("com.apple.Preferences")
    static let missingApp = IconSource.bundle("com.brunostjohn.switch.no-such-app")
    static let missingType = IconSource.type("com.brunostjohn.switch.no-such-type")

    enum IconSource {
        case bundle(String)
        case type(String)

        /// What IconServices draws for a request of this kind it cannot or will not serve
        var missing: IconSource {
            switch self {
            case .bundle: IconServicesCache.missingApp
            case .type: IconServicesCache.missingType
            }
        }
    }

    struct Image {
        let rgba: [UInt8]
        let side: Int

        var alpha: [Double] { stride(from: 3, to: rgba.count, by: 4).map { Double(rgba[$0]) / 255 } }
    }

    struct CacheInfo {
        var build = "absent", at = "none", container = "", resetThisBoot: Bool?
        var folders: [String] = []
        var store: URL?
    }

    static func measure(_ metrics: inout Metrics, _ wrong: inout [String]) {
        metrics.set("icon_cache_build", systemBuild() ?? "unreadable")
        let found = cacheInfo()
        metrics.set("icon_cache_info_build", found.build)
        metrics.set("icon_cache_info_at", found.at)
        if let resetThisBoot = found.resetThisBoot { metrics.set("icon_cache_reset_this_boot", resetThisBoot) }
        let cached = [
            27: icon(settings, points: 27, scale: 2, ignoreCache: false),
            68: icon(settings, points: 68, scale: 2, ignoreCache: false),
        ]
        guard let large = icon(settings, points: 68, scale: 2, ignoreCache: true),
            let small = icon(settings, points: 27, scale: 2, ignoreCache: true)
        else {
            metrics.set("icon_api", "absent")
            wrong.append("icon_api")
            return
        }
        if let placeholder = icon(missingApp, points: 68, scale: 2, ignoreCache: true),
            differing(placeholder, large) == 0
        {
            metrics.set("icon_placeholder", true)
            wrong.append("icon_placeholder")
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
        guard a.side == b.side else { return a.side * a.side }
        return (0..<(a.rgba.count / 4)).filter { p in
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

    /// The cache folder in the group container, or why the app cannot name it
    static func cacheDirectory() -> (URL?, String) {
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "container_system_group_path_for_identifier")
        else { return (nil, "unreadable:no_container_lookup") }
        var error: UInt64 = 1
        guard let path = unsafeBitCast(symbol, to: GroupPath.self)(nil, group, &error) else {
            return (nil, "unreadable:container_error_\(error)")
        }
        defer { free(path) }
        return (URL(fileURLWithPath: String(cString: path)).appendingPathComponent(cache), "")
    }

    /// __cache_info__'s ProductBuildVersion ("absent" without one), where it is, whether it was written since this boot,
    /// the container's folder name, the cache's subfolders and the store index beside the info, or why the app cannot
    /// read them
    static func cacheInfo() -> CacheInfo {
        var found = CacheInfo()
        let (located, why) = cacheDirectory()
        guard let directory = located else {
            found.build = why
            return found
        }
        found.container =
            directory.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .lastPathComponent
        let names: [String]
        do {
            names = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        } catch {
            found.build = "unreadable:" + "\((error as NSError).domain)_\((error as NSError).code)"
            return found
        }
        found.folders = names.filter { name in
            var folder: ObjCBool = false
            return FileManager.default.fileExists(
                atPath: directory.appendingPathComponent(name).path, isDirectory: &folder)
                && folder.boolValue
        }.sorted()
        let candidates =
            [(directory, "top")]
            + found.folders.map { (directory.appendingPathComponent($0), "salt") }
        for (folder, at) in candidates {
            let url = folder.appendingPathComponent(info)
            guard let data = try? Data(contentsOf: url) else { continue }
            let values = (try? PropertyListSerialization.propertyList(from: data, format: nil)) as? [String: Any]
            let written =
                (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate)
                ?? .distantPast
            found.build = values?["ProductBuildVersion"] as? String ?? "absent"
            found.at = at
            found.resetThisBoot = written > Date(timeIntervalSinceNow: -ProcessInfo.processInfo.systemUptime)
            found.store = folder.appendingPathComponent("store.index")
            return found
        }
        return found
    }

    /// The number of files under `folder` and their bytes, which a stored render changes
    static func stamp(_ folder: URL) -> [Int]? {
        guard
            let files = FileManager.default.enumerator(
                at: folder, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey])
        else { return nil }
        var count = 0, bytes = 0
        for case let url as URL in files {
            guard let values = try? url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey]),
                values.isRegularFile == true
            else { continue }
            count += 1
            bytes += values.fileSize ?? 0
        }
        return [count, bytes]
    }

    typealias InitSizeScale = @convention(c) (AnyObject, Selector, CGSize, CGFloat) -> AnyObject
    typealias SetBool = @convention(c) (AnyObject, Selector, Bool) -> Void
    typealias ImageFor = @convention(c) (AnyObject, Selector, AnyObject) -> Unmanaged<CGImage>?

    /// `source`'s icon as iconservicesagent draws it at `points` and `scale`, nil where IconServices lacks what this asks
    static func icon(_ source: IconSource, points: CGFloat, scale: CGFloat, ignoreCache: Bool) -> Image? {
        guard let iconType = NSClassFromString("ISIcon") as? NSObject.Type,
            let descriptorType = NSClassFromString("ISImageDescriptor") as? NSObject.Type
        else { return nil }
        let initSize = NSSelectorFromString("initWithSize:scale:"), ignore = NSSelectorFromString("setIgnoreCache:")
        let prepare = NSSelectorFromString("prepareImageForDescriptor:")
        let cgImage = NSSelectorFromString("CGImageForDescriptor:")
        let (initIcon, identifier) =
            switch source {
            case .bundle(let id): ("initWithBundleIdentifier:", id)
            case .type(let id): ("initWithType:", id)
            }
        guard descriptorType.instancesRespond(to: initSize), descriptorType.instancesRespond(to: ignore),
            iconType.instancesRespond(to: prepare), iconType.instancesRespond(to: cgImage),
            let made = iconType.perform(NSSelectorFromString("alloc"))?.takeUnretainedValue() as? NSObject,
            let icon = made.perform(NSSelectorFromString(initIcon), with: identifier)?.takeUnretainedValue()
                as? NSObject,
            let alloc = descriptorType.perform(NSSelectorFromString("alloc"))?.takeUnretainedValue()
        else { return nil }
        let descriptor = unsafeBitCast(descriptorType.instanceMethod(for: initSize), to: InitSizeScale.self)(
            alloc, initSize, CGSize(width: points, height: points), scale)
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
