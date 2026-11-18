import CoreFoundation
import Foundation
import HWTestCore
import HWTestKit

/// True Tone's default on iOS 27: CBColorAdaptationEnabled in com.apple.CoreBrightness, read as CoreBrightness reads
/// it (current user, current host), which the kernel adds as 0 where it is missing (native_switch_true_tone_default).
/// value= is that 0 or the user's choice since, "-" for none; keys= counts the domain's keys, 0 when the sandbox
/// refuses the read (SwitchHWTest27.entitlements). A missing key fails: CoreBrightness then turns True Tone on.
struct TrueToneDefaultTest: HWTest {
    let id = "display.true_tone_default"
    let name = "True Tone off by default, the user's choice kept"
    let subsystem = Subsystem.display

    func run(_ context: TestContext) async throws -> Outcome {
        let domain = "com.apple.CoreBrightness" as CFString
        let value = CFPreferencesCopyValue(
            "CBColorAdaptationEnabled" as CFString, domain, kCFPreferencesCurrentUser, kCFPreferencesCurrentHost)
        let keys = CFPreferencesCopyKeyList(domain, kCFPreferencesCurrentUser, kCFPreferencesCurrentHost) as? [String]
        let enabled = value as? NSNumber
        var metrics = Metrics()
        metrics.set("keys", keys?.count ?? 0)
        metrics.set("value", enabled.map { String($0.intValue) } ?? "-")
        return .judged(
            [
                ("com.apple.CoreBrightness is readable", (keys?.count ?? 0) > 0),
                ("CBColorAdaptationEnabled is set", enabled != nil),
            ], metrics)
    }
}
