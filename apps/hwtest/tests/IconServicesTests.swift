import CoreGraphics
import Foundation
import HWTestCore
import HWTestGPU
import UIKit
import UniformTypeIdentifiers

/// One icon as iconservicesagent draws it at one size and scale, as the cache serves it and uncached (setIgnoreCache:),
/// for the icon_* cases. Fields, prefixed `<icon><points>x<scale in hundredths>` (icons: set Settings, pc Passcode, cal
/// Calendar):
///  - `_edge=right/bottom`: IconEdgeBand's asymmetric pixels of the uncached image's last two columns and rows (raw's
///    `edge miss` shape); Apple's GPU draws macOS's icons symmetric (0/0 on an M4 Max, iconpassoracle sizes);
///  - `_cached`: the pixels where the cached request's image is more than IconServicesCache.codes off the uncached one,
///    `unmatched` where the sizes differ: a cache that holds another build's or another day's picture;
///  - `_store`: the cache folder's files around the cached request: `same` (served from the store, or not stored),
///    `grew` (rendered and stored) or `unknown` (no cache folder the app can read);
///  - `_side`, only off its points times scale; `_placeholder`, only when the uncached image is the placeholder
///    IconServices makes when the agent refuses a request (`true`: a missing app's or type's icon at the same size, by
///    the request's kind) or there is none to compare with (`unknown`).
/// A failure names the field.
enum IconArm {
    static func placeholder(
        _ source: IconServicesCache.IconSource, points: CGFloat, scale: CGFloat
    )
        -> IconServicesCache.Image?
    {
        IconServicesCache.icon(source.missing, points: points, scale: scale, ignoreCache: true)
    }

    static func judge(
        _ name: String, _ source: IconServicesCache.IconSource, points: CGFloat, scale: CGFloat,
        placeholder: IconServicesCache.Image?, store: URL?, _ metrics: inout Metrics
    ) -> [String] {
        let prefix = "\(name)\(Int(points))x\(Int((scale * 100).rounded()))"
        let before = store.flatMap(IconServicesCache.stamp)
        let cached = IconServicesCache.icon(source, points: points, scale: scale, ignoreCache: false)
        let after = store.flatMap(IconServicesCache.stamp)
        metrics.set("\(prefix)_store", before == nil || after == nil ? "unknown" : before == after ? "same" : "grew")
        guard let fresh = IconServicesCache.icon(source, points: points, scale: scale, ignoreCache: true) else {
            metrics.set("\(prefix)_api", "absent")
            return ["\(prefix)_api"]
        }
        var failures: [String] = []
        if fresh.side != Int((points * scale).rounded()) { metrics.set("\(prefix)_side", fresh.side) }
        let edge = IconEdgeBand.asymmetry(IconEdgeBand.alpha(fresh.rgba), side: fresh.side)
        metrics.set("\(prefix)_edge", edge.field)
        if max(edge.right, edge.bottom) > fresh.side / 32 { failures.append("\(prefix)_edge") }
        if let cached, cached.side == fresh.side {
            let diff = IconServicesCache.differing(cached, fresh)
            metrics.set("\(prefix)_cached", diff)
            if diff > 0 { failures.append("\(prefix)_cached") }
        } else if cached != nil {
            metrics.set("\(prefix)_cached", "unmatched")
            failures.append("\(prefix)_cached")
        }
        if let placeholder {
            if IconServicesCache.differing(placeholder, fresh) == 0 {
                metrics.set("\(prefix)_placeholder", true)
                failures.append("\(prefix)_placeholder")
            }
        } else {
            metrics.set("\(prefix)_placeholder", "unknown")
        }
        return failures
    }

    /// Every arm of `points` at the device's own scale and at the 1.25 Settings asks its named images at (boot 22's
    /// `scaleFactor:1.250000`), where the size is whole pixels
    static func run(_ name: String, _ source: IconServicesCache.IconSource, points: [CGFloat]) async -> Outcome {
        let screen = await MainActor.run { UIScreen.main.scale }
        let store = IconServicesCache.cacheInfo().store?.deletingLastPathComponent()
        var metrics = Metrics()
        var failures: [String] = []
        for scale in screen == 1.25 ? [screen] : [screen, 1.25] {
            for size in points where (size * scale).rounded() == size * scale {
                let missing = placeholder(source, points: size, scale: scale)
                failures += judge(
                    name, source, points: size, scale: scale, placeholder: missing, store: store, &metrics)
            }
        }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.joined(separator: ","), metrics)
    }
}

/// Settings' icon at the App Library's sizes (68, 28 and 15 points, boot 22's 136x136, 56x56 and 30x30 surfaces at
/// scale 2, and 40 and 16) at the device's scale and at 1.25: IconArm's fields. FAIL: an arm's edge bands asymmetric,
/// its cached picture another, a placeholder, or IconServices' calls absent.
struct IconSmallSizesTest: HWTest {
    let id = "gpu.raw.icon_small_sizes"
    let name = "IconServices' Settings icon at the App Library's sizes: edges, cache and placeholder"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        await IconArm.run("set", IconServicesCache.settings, points: [68, 40, 28, 16, 15])
    }
}

/// Settings' Passcode pane icon (com.apple.graphic-icon.passcode, which Settings shows on a device without Face ID or
/// Touch ID) at Settings' 29-point row size and 68 points: IconArm's fields. SKIP where the system declares no such
/// type. FAIL as gpu.raw.icon_small_sizes.
struct IconPasscodeTest: HWTest {
    let id = "gpu.raw.icon_passcode"
    let name = "IconServices' Passcode pane icon: edges, cache and placeholder"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }
    static let type = "com.apple.graphic-icon.passcode"

    func run(_ context: TestContext) async throws -> Outcome {
        guard UTType(Self.type)?.isDeclared == true else { return .skip("the system declares no \(Self.type)") }
        return await IconArm.run("pc", .type(Self.type), points: [29, 68])
    }
}

/// Calendar's icon (com.apple.mobilecal, drawn with today's date, so a cached picture of another day is another
/// picture) at the App Library's 68 and 28 points: IconArm's fields. FAIL as gpu.raw.icon_small_sizes.
struct IconCalendarTest: HWTest {
    let id = "gpu.raw.icon_calendar"
    let name = "IconServices' Calendar icon: edges, today's cache and placeholder"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    func run(_ context: TestContext) async throws -> Outcome {
        await IconArm.run("cal", .bundle("com.apple.mobilecal"), points: [68, 28])
    }
}

/// What iconservicesagent's rendered-icon cache holds (IconServicesCache.cacheInfo): `container` (the group container's
/// folder, a UUID or the group's name), `folders` and `first_folder` (24B119 keeps its cache in a salt folder; 21H461 at
/// the top), `info_at` (top, salt or none), `info_build` (__cache_info__'s ProductBuildVersion, without which the agent
/// resets the cache at launch), `reset_this_boot`, `system_build`. FAIL: no __cache_info__ the app can read, or a
/// ProductBuildVersion other than the system's.
struct IconCacheTest: HWTest {
    let id = "system.icon_cache"
    let name = "IconServices' rendered-icon cache: where it is and its build"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        let system = IconServicesCache.systemBuild() ?? "unreadable"
        let found = IconServicesCache.cacheInfo()
        metrics.set("system_build", system)
        metrics.set("container", found.container.isEmpty ? "-" : found.container)
        metrics.set("folders", found.folders.count)
        if let first = found.folders.first { metrics.set("first_folder", first) }
        metrics.set("info_at", found.at)
        metrics.set("info_build", found.build)
        if let reset = found.resetThisBoot { metrics.set("reset_this_boot", reset) }
        return .judged(
            [
                ("__cache_info__ is readable", found.at != "none"),
                ("__cache_info__ holds the system's build", found.build == system),
            ], metrics)
    }
}

/// The kernel's drop of the rendered-icon cache at boot (kernel/switch/boot/icon_cache.c):
/// `request`, the GPU build the last deploy named in /private/var/Switch/icon-cache-gpu (tools/deploy/icon_cache.py),
/// `none` without one; `dropped_for`, the build the kernel last removed __cache_info__ for (icon-cache-reset), and
/// `dropped_this_boot`; `container`, the agent's group container folder, which the kernel names by the group;
/// `reset_this_boot`, whether the agent wrote __cache_info__ since this boot. FAIL: a record the app cannot read, a
/// container folder the kernel does not look in, a named build the kernel did not drop the cache for, or a drop this
/// boot the agent did not follow with its own reset.
struct IconCacheResetTest: HWTest {
    let id = "system.icon_cache_reset"
    let name = "The kernel lets IconServices' icons go once per GPU build a deploy names"
    let subsystem = Subsystem.system
    static let request = "/private/var/Switch/icon-cache-gpu"
    static let marker = "/private/var/Switch/icon-cache-reset"

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        let request = Self.record(Self.request), dropped = Self.record(Self.marker)
        let found = IconServicesCache.cacheInfo()
        metrics.set("request", request.text)
        metrics.set("dropped_for", dropped.text)
        metrics.set("dropped_this_boot", dropped.thisBoot)
        metrics.set("container", found.container.isEmpty ? "-" : found.container)
        metrics.set("info_at", found.at)
        metrics.set("info_build", found.build)
        if let reset = found.resetThisBoot { metrics.set("reset_this_boot", reset) }
        let readable = !request.text.hasPrefix("unreadable") && !dropped.text.hasPrefix("unreadable")
        return .judged(
            [
                ("both records are readable", readable),
                ("the agent's container is the folder the kernel drops in", found.container == IconServicesCache.group),
                (
                    "the kernel dropped the cache for the named build",
                    request.text == "none" || request.text == dropped.text
                ),
                ("a drop this boot is followed by the agent's reset", !dropped.thisBoot || found.resetThisBoot == true),
            ], metrics)
    }

    /// A record's first line, at most 12 characters (`empty` for an empty line, `none` without the file, or why the app
    /// cannot read it), and whether it was written since this boot
    static func record(_ path: String) -> (text: String, thisBoot: Bool) {
        do {
            let line = try String(contentsOfFile: path, encoding: .utf8).prefix { $0 != "\n" }.prefix(12)
            let written = try FileManager.default.attributesOfItem(atPath: path)[.modificationDate] as? Date
            let boot = Date(timeIntervalSinceNow: -ProcessInfo.processInfo.systemUptime)
            return (line.isEmpty ? "empty" : String(line), (written ?? .distantPast) > boot)
        } catch let error as NSError where error.domain == NSCocoaErrorDomain && error.code == NSFileReadNoSuchFileError
        {
            return ("none", false)
        } catch {
            return ("unreadable:\((error as NSError).domain)_\((error as NSError).code)", false)
        }
    }
}
