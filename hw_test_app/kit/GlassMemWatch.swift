import Darwin
import Foundation
import SwiftUI
import notify

/// The render server's blur-texture probe (TegraGM20BMetal raw_glass_mem.h, debug builds). HW Test's glass tests arm it
/// for their own run; the main screen's watch arms it for five minutes of normal use (scroll Settings, come back). While
/// armed, its copy-engine reads run on every compositor frame, so the tests that measure frames refuse to run under it.
public enum GlassMemWatch {
    static let prefix = "com.brunostjohn.switch.debug."
    public static let watchSeconds: TimeInterval = 300

    /// Seconds the probe stays armed, from the render server's glass-mem.armed (mach time it ends, top bit: HW Test
    /// armed it); nil when disarmed or in a release build, which publishes nothing.
    public static func armedSeconds() -> TimeInterval? {
        var token: Int32 = 0
        guard notify_register_check(prefix + "glass-mem.armed", &token) == NOTIFY_STATUS_OK else { return nil }
        defer { notify_cancel(token) }
        var state: UInt64 = 0
        guard notify_get_state(token, &state) == NOTIFY_STATUS_OK else { return nil }
        let until = state & ~(UInt64(1) << 63), now = mach_absolute_time()
        guard until > now else { return nil }
        var base = mach_timebase_info_data_t()
        mach_timebase_info(&base)
        return Double(until - now) * Double(base.numer) / Double(base.denom) / 1e9
    }

    public static func watch() { notify_post(prefix + "glass-mem-watch") }
    public static func stopWatching() { notify_post(prefix + "glass-mem-watch-off") }

    /// Ends what a glass test armed (the draw lines, the record and the probe's per-command-buffer lines)
    static func endGlassTest() { notify_post(prefix + "glass-probe-off") }

    static func armsTheProbe(_ id: String) -> Bool { id.hasPrefix("ui.glass") }

    /// The tests whose numbers the probe's per-frame reads would move: frame pacing and the GPU's
    static func measuresFrames(_ id: String) -> Bool {
        !armsTheProbe(id) && (id.hasPrefix("pacing.") || id.hasPrefix("gpu.") || id.hasSuffix(".pacing"))
    }

    static func refusal(_ left: TimeInterval) -> String {
        "the glass memory probe is armed (\(Int(left.rounded(.up))) s left): its copy-engine reads run on every "
            + "compositor frame and move this test's numbers; stop it on the main screen and run again"
    }
}

/// The main screen's control: arm the probe for five minutes and see how long it has left. Disarmed, it draws nothing
/// that changes, so the screen commits nothing.
struct GlassWatchRow: View {
    @State private var until: Date?

    var body: some View {
        Group {
            if let until {
                TimelineView(.periodic(from: .now, by: 1)) { context in
                    let left = max(0, Int(until.timeIntervalSince(context.date).rounded(.up)))
                    HStack {
                        Text("Glass probe armed: \(left / 60):\(String(format: "%02d", left % 60)) left")
                        Spacer()
                        Button("Stop") {
                            GlassMemWatch.stopWatching()
                            self.until = nil
                        }
                    }
                }
            } else {
                Button("Glass probe: arm for 5 minutes") {
                    GlassMemWatch.watch()
                    until = Date().addingTimeInterval(GlassMemWatch.watchSeconds)
                }
            }
        }
        .onAppear(perform: refresh)
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)) { _ in
            refresh()
        }
        .task(id: until) {
            guard let until else { return }
            try? await Task.sleep(nanoseconds: UInt64(max(0, until.timeIntervalSinceNow) * 1e9))
            if self.until == until { self.until = nil }
        }
    }

    private func refresh() {
        until = GlassMemWatch.armedSeconds().map { Date().addingTimeInterval($0) }
    }
}
