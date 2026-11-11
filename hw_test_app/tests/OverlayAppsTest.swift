import Darwin
import Foundation
import HWTestCore

/// The overlay's apps reach /Applications through a bindfs union, a mount of its own. LaunchServices files a bundle
/// under the container of the volume getmntinfo names for the bundle's device, and lsd's first-boot work removes every
/// container but the root, system data, Preboot and user data volumes after each reboot: the overlay's apps were gone
/// from SpringBoard on every boot after the one that registered them. statfs and getmntinfo must name the root volume
/// for every overlay app, as for an app on the sealed system (kernel/switch/vfs/identity.c), and LaunchServices must
/// still hold this app's own record.
struct OverlayAppsTest: HWTest {
    let id = "system.overlay_apps"
    let name = "Overlay apps belong to the root volume and stay registered"
    let subsystem = Subsystem.system

    static let applications = "/Applications"
    static let prefix = "com.brunostjohn.switch."

    func run(_ context: TestContext) async throws -> Outcome {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: Self.applications) else {
            return .skip("the sandbox lets the app list no \(Self.applications)")
        }
        let apps = names.sorted().map { Self.applications + "/" + $0 }.filter(Self.isOverlay)
        var root = statfs()
        guard statfs("/", &root) == 0 else { return .fail("statfs(/) failed: errno \(errno)") }
        let mounts = Self.mountPoints()
        var metrics = Metrics()
        var checks: [(String, Bool)] = [("the overlay shows an app", !apps.isEmpty)]
        var misfiled = 0
        for app in apps {
            let name = (app as NSString).lastPathComponent
            var volume = statfs()
            var file = stat()
            let filed =
                statfs(app, &volume) == 0 && stat(app, &file) == 0 && Self.same(volume.f_fsid, root.f_fsid)
                && mounts[file.st_dev] == "/"
            if !filed { misfiled += 1 }
            checks.append(("\(name) is filed under the root volume", filed))
        }
        metrics.set("apps", apps.count)
        metrics.set("misfiled", misfiled)
        let own = Self.registered(Bundle.main.bundleIdentifier ?? "")
        metrics.set("own_record", own)
        checks.append(("LaunchServices holds this app's record", own))
        return .judged(checks, metrics)
    }

    static func isOverlay(_ path: String) -> Bool {
        guard path.hasSuffix(".app"), let info = NSDictionary(contentsOfFile: path + "/Info.plist"),
            let identifier = info["CFBundleIdentifier"] as? String
        else { return false }
        return identifier.hasPrefix(prefix)
    }

    static func same(_ a: fsid_t, _ b: fsid_t) -> Bool { a.val.0 == b.val.0 && a.val.1 == b.val.1 }

    /// getmntinfo's mount point for each device: what CoreServicesInternal's createVolumeURL files a bundle under.
    static func mountPoints() -> [dev_t: String] {
        var list: UnsafeMutablePointer<statfs>?
        let count = getmntinfo(&list, MNT_NOWAIT)
        guard count > 0, let list else { return [:] }
        var points: [dev_t: String] = [:]
        for entry in UnsafeBufferPointer(start: list, count: Int(count)) where points[entry.f_fsid.val.0] == nil {
            points[entry.f_fsid.val.0] = withUnsafePointer(to: entry.f_mntonname) {
                String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self))
            }
        }
        return points
    }

    static func registered(_ identifier: String) -> Bool {
        (proxy(identifier)?.value(forKey: "isInstalled") as? Bool) ?? false
    }

    static func proxy(_ identifier: String) -> NSObject? {
        guard let proxies = NSClassFromString("LSApplicationProxy") as? NSObject.Type else { return nil }
        return proxies.perform(NSSelectorFromString("applicationProxyForIdentifier:"), with: identifier)?
            .takeUnretainedValue() as? NSObject
    }
}
