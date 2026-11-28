import Darwin
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import QuartzCore
import UIKit

/// An app launch's compositor cost, there and back, in two arms. Settings is opened (LSApplicationWorkspace, with
/// FrontBoard's launch entitlement), then this app again, and the debug kernel answers the compositor's longest GPU job
/// and every span of 5 ms or more of one state the render server's raw path noted (TegraGM20BPathsABI.h spans op;
/// span_marks.h), each "<us> us k<key> <state> <instructions> instr x<marks> <target>k[m]".
/// - The cold arm is the first round, run only where the process list shows Settings was not running when the test
///   began: the app holds no right to end another app, and a list the sandbox cuts to this app cannot tell (the
///   first round is then reported as "first").
/// - The warm arm is the next round.
/// The return counts: boot 22's generic spans came after this app was opened again, while Settings was still launching.
/// Any arm's job longer than three frames fails. The spans of Core Animation's generic merge states are named by key
/// from the build's own table (ca_generic_keys.txt), never by a state's name, which a span may lack: their count and
/// time are what CA's first draw of each state new to the render server costs.
struct LaunchHitchTest: HWTest {
    let id = "ui.launch_hitch"
    let name = "An app launch and the return keep the compositor's GPU jobs within three frames"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval {
        let waits = Self.leaveSeconds + Self.returnSeconds
        return 2 * (waits + Double(Self.settleSeconds + Self.returnSettleSeconds)) + 10
    }

    static let spansOp: UInt32 = 3
    static let launched = "com.apple.Preferences"
    static let launchedProcess = "Preferences"
    static let settleSeconds: UInt64 = 4
    static let returnSettleSeconds: UInt64 = 2
    static let leaveSeconds = 10.0
    static let returnSeconds = 15.0
    static let jobBoundUs = 50_000
    /// SwitchGpuSpanNotes.h kNotes: each ask takes at least one note, so this many asks and one more empty it
    static let keptNotes = 16

    func run(_ context: TestContext) async throws -> Outcome {
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection") }
        guard Self.drain(connection) != nil else {
            return .skip(
                KernelSysctl.isReleaseKernel()
                    ? "the kernel keeps no compositor spans (a release kernel)"
                    : "no compositor spans (build with --//bazel:stats_gpu)"
            )
        }
        guard let url = Bundle.main.url(forResource: "ca_generic_keys", withExtension: "txt"),
              let table = try? String(contentsOf: url, encoding: .utf8)
        else { return .fail("ca_generic_keys.txt is not in the app's bundle") }
        let states = GenericStates.parse(table)
        let processes = RunningProcesses.list(sysctl: "kern.proc.all")
        let target = LaunchTarget.state(of: Self.launchedProcess, in: processes, own: getpid())
        let task = await MainActor.run { UIApplication.shared.beginBackgroundTask(withName: id) }
        defer { Task { @MainActor in UIApplication.shared.endBackgroundTask(task) } }
        var arms: [LaunchHitchArm] = []
        for name in target.arms {
            guard let arm = try await Self.round(name, connection, states) else {
                return .skip("LSApplicationWorkspace refused to open \(Self.launched)")
            }
            arms.append(arm)
        }
        return LaunchHitchArm.outcome(arms, target: target, genericStates: states.names.count)
    }

    /// Settings opened and this app opened again, judged as one window from a drained start; nil where Settings would
    /// not open
    static func round(
        _ name: String, _ connection: GPUConnection, _ states: GenericStates
    ) async throws
        -> LaunchHitchArm?
    {
        _ = drain(connection)
        let (opened, launchMs) = await msUntil(UIApplication.didEnterBackgroundNotification, within: leaveSeconds) {
            open(launched)
        }
        guard opened else { return nil }
        try await Task.sleep(nanoseconds: settleSeconds * NSEC_PER_SEC)
        let launch = drain(connection)
        let (_, returnMs) = await msUntil(UIApplication.didBecomeActiveNotification, within: returnSeconds) {
            open(Bundle.main.bundleIdentifier ?? "")
        }
        try await Task.sleep(nanoseconds: returnSettleSeconds * NSEC_PER_SEC)
        let back = drain(connection)
        let verdict = LaunchHitchVerdict(
            reading: SpanReading.merged([launch, back].compactMap { $0 }), states: states, jobBoundUs: jobBoundUs
        )
        return LaunchHitchArm(name: name, verdict: verdict, launchMs: launchMs, returnMs: returnMs)
    }

    /// Whether `start` did what it asks, and the milliseconds from it until this app posts `name`; nil where it does
    /// not post within `seconds`
    @MainActor
    static func msUntil(_ name: Notification.Name, within seconds: Double, _ start: () -> Bool) async -> (Bool, Double?)
    {
        let began = CACurrentMediaTime()
        var started = false
        var token: NSObjectProtocol?
        let ms: Double? = await withCheckedContinuation { continuation in
            var resumed = false
            let finish = { (value: Double?) in
                guard !resumed else { return }
                resumed = true
                continuation.resume(returning: value)
            }
            token = NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { _ in
                finish((CACurrentMediaTime() - began) * 1000)
            }
            started = start()
            if !started { return finish(nil) }
            DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { finish(nil) }
        }
        if let token { NotificationCenter.default.removeObserver(token) }
        return (started, ms)
    }

    /// One spans line; nil off a debug kernel
    static func spans(_ connection: GPUConnection) -> String? {
        let (kr, reply) = connection.call(
            GPUPaths.method, GPUConnection.words([spansOp, 0]), outputSize: GPUPaths.replySize
        )
        guard kr == KERN_SUCCESS else { return nil }
        return String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self)
    }

    /// Every note waiting and the totals since the last ask, as one reading: the kernel keeps the notes a line has no
    /// room for, so it is asked until a line holds all it had; nil off a debug kernel
    static func drain(_ connection: GPUConnection) -> SpanReading? {
        var readings: [SpanReading] = []
        for _ in 0 ... keptNotes {
            guard let line = spans(connection) else { break }
            let reading = SpanReading.parse(line)
            readings.append(reading)
            if reading.complete { break }
        }
        return readings.isEmpty ? nil : SpanReading.merged(readings)
    }

    /// -[LSApplicationWorkspace openApplicationWithBundleID:], which answers a BOOL
    static func open(_ bundle: String) -> Bool {
        let selector = NSSelectorFromString("openApplicationWithBundleID:")
        guard let type = NSClassFromString("LSApplicationWorkspace") as? NSObject.Type,
              let workspace = type.perform(NSSelectorFromString("defaultWorkspace"))?
                .takeUnretainedValue() as? NSObject,
              workspace.responds(to: selector)
        else { return false }
        typealias Open = @convention(c) (NSObject, Selector, NSString) -> Bool
        let call = unsafeBitCast(workspace.method(for: selector), to: Open.self)
        return call(workspace, selector, bundle as NSString)
    }
}
