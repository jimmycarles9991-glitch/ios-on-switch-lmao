import Foundation
import HWTestCore

/// Each overlay app's LaunchServices record against its bundle in /Applications: installed, with the entitlements its
/// executable is signed with. A deploy that changes an app has switch-appregd (drivers/SwitchAppRegistration) register
/// it at the next boot; a record that missed it still launches the app, with the entitlements it had before.
struct OverlayAppRecordsTest: HWTest {
    let id = "system.overlay_app_records"
    let name = "Overlay apps' LaunchServices records match their bundles"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        let applications = OverlayAppsTest.applications
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: applications) else {
            return .skip("the sandbox lets the app list no \(applications)")
        }
        let apps = names.sorted().map { applications + "/" + $0 }.filter(OverlayAppsTest.isOverlay)
        var checks: [(String, Bool)] = [("the overlay shows an app", !apps.isEmpty)]
        var stale: [String] = []
        for app in apps {
            guard let info = NSDictionary(contentsOfFile: app + "/Info.plist"),
                let identifier = info["CFBundleIdentifier"] as? String,
                let executable = info["CFBundleExecutable"] as? String
            else {
                checks.append(("\(app) names its executable", false))
                continue
            }
            let proxy = OverlayAppsTest.proxy(identifier)
            let installed = (proxy?.value(forKey: "isInstalled") as? Bool) ?? false
            let recorded = (proxy?.value(forKey: "entitlements") as? [String: Any]) ?? [:]
            let image = try? Data(contentsOf: URL(fileURLWithPath: app + "/" + executable), options: .alwaysMapped)
            let signed = image.flatMap(EmbeddedEntitlements.of)
            let differing = signed.map { Self.differing(recorded, $0) } ?? ["unreadable_signature"]
            checks.append(("\(identifier) is installed", installed))
            checks.append(
                (
                    "\(identifier)'s record has its executable's entitlements"
                        + (differing.isEmpty ? "" : " (differ: \(differing.joined(separator: ",")))"),
                    differing.isEmpty
                ))
            if !installed || !differing.isEmpty { stale.append(identifier) }
        }
        var metrics = Metrics()
        metrics.set("apps", apps.count)
        metrics.set("stale", stale.count)
        if !stale.isEmpty { metrics.set("stale_apps", stale.joined(separator: ",")) }
        return .judged(checks, metrics)
    }

    /// The keys whose values the record and the signature disagree on, either missing one counting as a difference.
    static func differing(_ recorded: [String: Any], _ signed: [String: Any]) -> [String] {
        Set(recorded.keys).union(signed.keys).filter { key in
            guard let a = recorded[key], let b = signed[key] else { return true }
            return !(a as AnyObject).isEqual(b)
        }.sorted()
    }
}
