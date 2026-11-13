import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import UIKit

/// An app launch's compositor cost: Settings is opened (LSApplicationWorkspace, with FrontBoard's launch entitlement), and
/// after its zoom the debug kernel answers the compositor's longest GPU job since the test began and every span of 5 ms
/// or more of one state the render server's raw path noted meanwhile (TegraGM20BPathsABI.h spans op; raw_span_marks.h),
/// each "<us> us <state> k<key> <instructions> instr x<marks> <target>k[m]". Then this app is opened again. A job longer
/// than three frames fails, naming the spans behind it; Core Animation's generic merge state (..Xghfc) is counted.
struct LaunchHitchTest: HWTest {
    let id = "ui.launch_hitch"
    let name = "An app launch keeps the compositor's GPU jobs within three frames"
    let subsystem = Subsystem.compositor

    static let spansOp: UInt32 = 3
    static let launched = "com.apple.Preferences"
    static let settleSeconds: UInt64 = 4
    static let jobBoundUs = 50_000

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection") }
        guard Self.spans(connection) != nil else {
            return .skip("the kernel keeps no compositor spans (a release kernel)")
        }
        let task = await MainActor.run { UIApplication.shared.beginBackgroundTask(withName: id) }
        defer { Task { @MainActor in UIApplication.shared.endBackgroundTask(task) } }
        guard Self.open(Self.launched) else { return .skip("LSApplicationWorkspace refused to open \(Self.launched)") }
        try await Task.sleep(nanoseconds: Self.settleSeconds * NSEC_PER_SEC)
        let line = Self.spans(connection) ?? ""
        let returned = Self.open(Bundle.main.bundleIdentifier ?? "")
        metrics.set("returned", returned)
        for _ in 0..<50 where await MainActor.run(body: { UIApplication.shared.applicationState != .active }) {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        let reading = SpanReading.parse(line)
        metrics.set("job_max_us", reading.jobMaxUs)
        metrics.set("spans", reading.spans.count)
        metrics.set("generic_spans", reading.spans.filter { $0.contains("Xghfc") }.count)
        for key in ["sync_subs", "sync_us", "behind", "behind_sync_us", "sync_max_us"] {
            metrics.set(key, reading.counts[key] ?? 0)
        }
        for (i, span) in reading.spans.prefix(6).enumerated() { metrics.set("span\(i)", span) }
        if reading.jobMaxUs > Self.jobBoundUs {
            let named = reading.spans.isEmpty ? "no span of 5 ms named" : reading.spans.joined(separator: " | ")
            return .fail("the compositor's longest job took \(reading.jobMaxUs) us: \(named)", metrics)
        }
        return .pass(metrics)
    }

    /// The spans line, which the kernel starts again after answering; nil off a debug kernel
    static func spans(_ connection: GPUConnection) -> String? {
        let (kr, reply) = connection.call(GPUPaths.method, GPUConnection.words([spansOp, 0]), outputSize: 512)
        guard kr == KERN_SUCCESS else { return nil }
        return String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self)
    }

    /// -[LSApplicationWorkspace openApplicationWithBundleID:], which answers a BOOL
    static func open(_ bundle: String) -> Bool {
        let selector = NSSelectorFromString("openApplicationWithBundleID:")
        guard let type = NSClassFromString("LSApplicationWorkspace") as? NSObject.Type,
            let workspace = type.perform(NSSelectorFromString("defaultWorkspace"))?.takeUnretainedValue() as? NSObject,
            workspace.responds(to: selector)
        else { return false }
        typealias Open = @convention(c) (NSObject, Selector, NSString) -> Bool
        let call = unsafeBitCast(workspace.method(for: selector), to: Open.self)
        return call(workspace, selector, bundle as NSString)
    }
}

/// "job_max_us=N spans=K; <owner>: <span>; ..." (drivers/TegraGM20B/SwitchGpuSpanNotes.h)
/// With the compositor's sync pushes in its profiled submissions (SwitchGpuCompositorSync.h): sync_subs, syncs, sync_us,
/// behind (kicks that found an app on GR), behind_sync_us, sync_max_us
struct SpanReading {
    let jobMaxUs: Int
    let counts: [String: Int]
    let spans: [String]

    static func parse(_ line: String) -> SpanReading {
        let parts = line.components(separatedBy: "; ")
        var counts: [String: Int] = [:]
        for field in (parts.first ?? "").split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1)
            if pair.count == 2, let value = Int(pair[1]) { counts[String(pair[0])] = value }
        }
        return SpanReading(jobMaxUs: counts["job_max_us"] ?? 0, counts: counts, spans: Array(parts.dropFirst()))
    }
}
