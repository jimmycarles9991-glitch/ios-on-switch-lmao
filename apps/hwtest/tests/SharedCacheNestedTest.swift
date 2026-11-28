import Foundation
import HWTestCore
import HWTestKit
import SwitchSharedCacheProbe

/// The dyld shared cache's read-only data (each subcache's __DATA_CONST and __AUTH_CONST mappings) as the app's own map
/// holds it: still the shared region's nested pmap. dyld remaps all of them VM_PROT_COPY in every process when it has
/// to store into the cache without TPRO pagers. iOS 27's function-variant slots would make it do so; the shared region
/// pager delivers them filled (kernel/switch/include/switch/fvariant.h). This covers an app process, and the kernel's
/// lookup of the cache it maps, on the device.
struct SharedCacheNestedTest: HWTest {
    let id = "system.shared_cache_nested"
    let name = "The shared cache's read-only data stays nested in the app's map"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        var probe = switch_shared_cache_probe()
        switch_shared_cache_probe(&probe)
        let uuid = withUnsafeBytes(of: probe.uuid) { $0.map { String(format: "%02x", $0) }.joined() }
        var metrics = Metrics()
        metrics.set("cache", uuid)
        metrics.set("slide", String(format: "0x%llx", probe.slide))
        metrics.set("files", Int(probe.files))
        metrics.set("mappings", Int(probe.mappings))
        metrics.set("nested", Int(probe.nested))
        metrics.set("mb", Int(probe.bytes >> 20))
        metrics.set("nested_mb", Int(probe.nested_bytes >> 20))
        if probe.first_unnested != 0 {
            metrics.set("first_unnested", String(format: "0x%llx", probe.first_unnested))
            metrics.set("first_mode", Int(probe.first_mode))
        }
        guard probe.error == 0 else {
            return .fail("probe error \(probe.error) (switch_shared_cache_probe.h)", metrics)
        }
        return .judged(
            [
                ("the cache's read-only data mappings were found (mappings > 0)", probe.mappings > 0),
                ("every one is still nested (nested == mappings)", probe.nested == probe.mappings),
            ], metrics
        )
    }
}
