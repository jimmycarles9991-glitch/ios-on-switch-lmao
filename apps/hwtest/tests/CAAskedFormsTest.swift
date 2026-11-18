import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The Core Animation states the raw pack holds by deduction (ca_asked.log): those this device's QuartzCore asks for where
/// Apple's offline compilation manifest names another colour precision (raw/pack/ca/deepcolour.py), and those device logs
/// named as made at run time (precompile/ca-names). Each is made on the raw facade and has to come out of the pack
/// (switchPackHit): a miss costs Core Animation a compile at an app's launch. QuartzCore names them by CADeviceUseCIF10,
/// which this process reads as QuartzCore does (MobileGestalt's HasExtendedColorDisplay, the CAEnableDeepFramebuffer
/// preference over it): it has to be the answer the pack was made for.
struct CAAskedFormsTest: HWTest {
    let id = "gpu.ca.asked_forms"
    let name = "Core Animation's states in this device's colour precision come from the pack"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    typealias BoolAnswer = @convention(c) (CFString) -> Bool

    /// CADeviceUseCIF10 as QuartzCore's block computes it (24B119 0x184409e94, 21H461 0x18957be58), nil where
    /// MobileGestalt cannot be asked
    static func cif10() -> Bool? {
        guard let gestalt = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW),
            let symbol = dlsym(gestalt, "MGGetBoolAnswer")
        else { return nil }
        guard unsafeBitCast(symbol, to: BoolAnswer.self)("HasExtendedColorDisplay" as CFString) else { return false }
        var exists: DarwinBoolean = false
        let preference = CFPreferencesGetAppBooleanValue(
            "CAEnableDeepFramebuffer" as CFString, kCFPreferencesCurrentApplication, &exists)
        return exists.boolValue ? preference : true
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let url = Bundle.main.url(forResource: "ca_asked", withExtension: "log"),
            let text = try? String(contentsOf: url, encoding: .utf8)
        else { return .fail("ca_asked.log is not in the app's bundle") }
        let header = text.prefix { $0 != "\n" }
        let lines = text.split(separator: "\n").compactMap { PipelineLine(String($0)) }
        var metrics = Metrics()
        metrics.set("states", lines.count)
        let device = Self.cif10().map { String($0) } ?? "unknown"
        metrics.set("cif10", device)
        metrics.set("pack_made_for", String(header.dropFirst("# cif10=".count)))
        guard header == "# cif10=\(device)" else {
            return .fail("the device's CADeviceUseCIF10 is \(device), the pack was made for \(header)", metrics)
        }
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        var missed: [String] = []
        var failed: [String] = []
        var slowest = 0.0
        let started = DispatchTime.now().uptimeNanoseconds
        let missesBefore = RawFacade.counters(of: gpu.device)["pack_misses"] ?? 0
        var unanswered = 0
        for line in lines {
            let made = DispatchTime.now().uptimeNanoseconds
            guard let state = try? CAState.pipeline(line, library: library, device: gpu.device) else {
                failed.append(line.shortLabel)
                continue
            }
            slowest = max(slowest, Double(DispatchTime.now().uptimeNanoseconds - made) / 1e6)
            switch RawFacade.packHit(state) {
            case .some(true): break
            case .some(false): missed.append(line.shortLabel)
            case .none: unanswered += 1
            }
        }
        metrics.set("process_pack_misses", (RawFacade.counters(of: gpu.device)["pack_misses"] ?? 0) - missesBefore)
        metrics.set("made_ms", Double(DispatchTime.now().uptimeNanoseconds - started) / 1e6)
        metrics.set("slowest_ms", slowest)
        metrics.set("pack_misses", missed.count)
        metrics.set("failed", failed.count)
        if !missed.isEmpty { metrics.set("missed_states", missed.prefix(12).joined(separator: ",")) }
        if !failed.isEmpty { metrics.set("failed_states", failed.prefix(12).joined(separator: ",")) }
        guard unanswered == 0 else {
            return .fail("\(unanswered) states answered no switchPackHit (a debug build's)", metrics)
        }
        if !missed.isEmpty || !failed.isEmpty {
            return .fail("\(missed.count) states the pack lacks, \(failed.count) not made", metrics)
        }
        return .pass(metrics)
    }
}
