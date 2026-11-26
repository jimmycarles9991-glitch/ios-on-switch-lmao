import AVFoundation
import Foundation
import HWTestCore

/// Counts the frames an engine's render thread asks for, so the measured rate is what the hardware consumed.
final class FrameCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var frames = 0
    private var callbacks = 0

    func add(_ count: Int) {
        lock.lock()
        frames += count
        callbacks += 1
        lock.unlock()
    }

    var snapshot: (frames: Int, callbacks: Int) {
        lock.lock()
        defer { lock.unlock() }
        return (frames, callbacks)
    }
}

enum Audio {
    /// Plays a 1 kHz tone at -20 dBFS for `seconds` and returns the frames rendered and the render callbacks.
    static func playTone(seconds: Double) async throws -> (frames: Int, callbacks: Int, rate: Double) {
        let session = AVAudioSession.sharedInstance()
        let rate = session.sampleRate
        let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2)!
        let counter = FrameCounter()
        var phase = 0.0
        let source = AVAudioSourceNode(format: format) { _, _, count, buffers in
            let list = UnsafeMutableAudioBufferListPointer(buffers)
            for frame in 0 ..< Int(count) {
                let sample = Float(sin(phase) * 0.1)
                phase += 2 * .pi * 1000 / rate
                for buffer in list { buffer.mData!.assumingMemoryBound(to: Float.self)[frame] = sample }
            }
            counter.add(Int(count))
            return noErr
        }
        let engine = AVAudioEngine()
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        try engine.start()
        try await Task.sleep(nanoseconds: 250_000_000)
        let before = counter.snapshot
        let start = CFAbsoluteTimeGetCurrent()
        try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
        let after = counter.snapshot
        let elapsed = CFAbsoluteTimeGetCurrent() - start
        engine.stop()
        return (
            after.frames - before.frames, after.callbacks - before.callbacks,
            Double(after.frames - before.frames) / elapsed
        )
    }

    static func activate(category: AVAudioSession.Category, rate: Double? = nil, buffer: Double? = nil) throws {
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(
            category, mode: .default, options: category == .playAndRecord ? [.defaultToSpeaker] : []
        )
        if let rate { try session.setPreferredSampleRate(rate) }
        if let buffer { try session.setPreferredIOBufferDuration(buffer) }
        try session.setActive(true)
    }

    /// Runs the input for `seconds`: the frames per second its tap received, and the input's own rate.
    static func captureRate(seconds: Double) async throws -> (measured: Double, rate: Double) {
        let engine = AVAudioEngine()
        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)
        let counter = FrameCounter()
        input.installTap(onBus: 0, bufferSize: 1024, format: format) { buffer, _ in
            counter.add(Int(buffer.frameLength))
        }
        defer {
            input.removeTap(onBus: 0)
            engine.stop()
        }
        try engine.start()
        try await Task.sleep(nanoseconds: 250_000_000)
        let before = counter.snapshot.frames
        let start = CFAbsoluteTimeGetCurrent()
        try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
        return (Double(counter.snapshot.frames - before) / (CFAbsoluteTimeGetCurrent() - start), format.sampleRate)
    }

    static func outputs() -> String {
        AVAudioSession.sharedInstance().currentRoute.outputs.map { $0.portType.rawValue }.joined(separator: ",")
    }
}

struct AudioPlaybackTest: HWTest {
    let rate: Double
    let subsystem = Subsystem.audio
    var id: String { "audio.playback_\(Int(rate / 1000))k" }
    var name: String { "AVAudioEngine playback at \(rate / 1000) kHz" }

    func run(_ context: TestContext) async throws -> Outcome {
        try Audio.activate(category: .playback, rate: rate)
        defer { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }
        let session = AVAudioSession.sharedInstance()
        let played = try await Audio.playTone(seconds: 2)
        var metrics = Metrics()
        metrics.set("session_rate", Int(session.sampleRate))
        metrics.set("measured_rate", played.rate, digits: 0)
        metrics.set("io_buffer_ms", session.ioBufferDuration * 1000, digits: 2)
        metrics.set("route", Audio.outputs())
        return .judged(
            [
                ("the hardware runs at \(Int(rate)) Hz", session.sampleRate == rate),
                (
                    "frames consumed at the session rate (within 2%)",
                    abs(played.rate - session.sampleRate) <= session.sampleRate * 0.02
                ),
            ], metrics, proof: "route=\(Audio.outputs())"
        )
    }
}

/// The smallest IO buffer the route grants, and that the engine runs on it without falling behind.
struct AudioLatencyTest: HWTest {
    let id = "audio.low_latency"
    let name = "Low-latency IO buffer"
    let subsystem = Subsystem.audio

    func run(_ context: TestContext) async throws -> Outcome {
        try Audio.activate(category: .playback, rate: 48_000, buffer: 0.0025)
        defer { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }
        let session = AVAudioSession.sharedInstance()
        let frames = Int((session.ioBufferDuration * session.sampleRate).rounded())
        let played = try await Audio.playTone(seconds: 1)
        var metrics = Metrics()
        metrics.set("buffer_frames", frames)
        metrics.set("buffer_ms", session.ioBufferDuration * 1000, digits: 2)
        metrics.set("output_latency_ms", session.outputLatency * 1000, digits: 2)
        metrics.set("callbacks_per_s", Double(played.callbacks), digits: 0)
        return .judged(
            [
                ("an IO buffer of 512 frames or fewer", frames <= 512),
                ("the engine keeps up (within 2%)", abs(played.rate - session.sampleRate) <= session.sampleRate * 0.02),
            ], metrics
        )
    }
}

struct AudioIdleWakeTest: HWTest {
    let id = "audio.idle_wake"
    let name = "Audio playback across APE idle power-off"
    let subsystem = Subsystem.audio

    func run(_ context: TestContext) async throws -> Outcome {
        try Audio.activate(category: .playback, rate: 48_000)
        var sessionActive = true
        defer {
            if sessionActive {
                try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
            }
        }
        let firstSessionRate = AVAudioSession.sharedInstance().sampleRate
        var beforeSnap: AudioKernelSnapshot?
        let beforeRate = try await AudioSysctl.playToneWithProbe(seconds: 0.6) {
            try await Task.sleep(nanoseconds: 300_000_000)
            beforeSnap = AudioSysctl.optional()
        }
        try AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        sessionActive = false
        try await Task.sleep(nanoseconds: 6_500_000_000)
        let idleSnap = AudioSysctl.optional()

        try Audio.activate(category: .playback, rate: 48_000)
        sessionActive = true
        let session = AVAudioSession.sharedInstance()
        var wakeSnap1: AudioKernelSnapshot?
        var wakeSnap2: AudioKernelSnapshot?
        let afterRate = try await AudioSysctl.playToneWithProbe(seconds: 1.0) {
            try await Task.sleep(nanoseconds: 300_000_000)
            wakeSnap1 = AudioSysctl.optional()
            try await Task.sleep(nanoseconds: 500_000_000)
            wakeSnap2 = AudioSysctl.optional()
        }

        var metrics = Metrics()
        metrics.set("session_rate", Int(session.sampleRate))
        metrics.set("before_rate", beforeRate, digits: 0)
        metrics.set("after_rate", afterRate, digits: 0)
        metrics.set("route", Audio.outputs())

        var checks: [(String, Bool)] = [
            ("hardware runs at 48000 Hz after idle wake", session.sampleRate == 48_000),
            (
                "initial playback runs at session rate (within 2%)",
                abs(beforeRate - firstSessionRate) <= firstSessionRate * 0.02
            ),
            (
                "post-idle playback runs at session rate (within 2%)",
                abs(afterRate - session.sampleRate) <= session.sampleRate * 0.02
            ),
        ]
        if let b = beforeSnap, let idle = idleSnap, let w1 = wakeSnap1, let w2 = wakeSnap2, b.backend == "tegra" {
            metrics.set("idle_powered", idle.powered)
            metrics.set("gates_delta", Int(idle.gates &- b.gates))
            metrics.set("ungates_delta", Int(w2.ungates &- b.ungates))
            metrics.set("resume_us", Int(w2.resumeUs))
            metrics.set("wake_mixer_delta", Int(w2.mixerSamples &- w1.mixerSamples))
            checks.append(("APE AUD partition powered off while idle", idle.powered == 0 && idle.gates > b.gates))
            checks.append(("APE AUD partition ungated cleanly on wake", w2.powered == 1 && w2.ungates > b.ungates))
            checks.append(
                ("MIXER1 active and advancing after wake", w2.mixerEn == 1 && w2.mixerSamples != w1.mixerSamples)
            )
        }
        return .judged(checks, metrics, proof: "route=\(Audio.outputs())")
    }
}

final class TimestampTracker: @unchecked Sendable {
    private let lock = NSLock()
    private var measuring = false
    private var callbacks = 0
    private var invalidTimestamps = 0
    private var sampleRegressions = 0
    private var hostRegressions = 0
    private var firstSample: Double?
    private var firstHost: UInt64?
    private var lastSample: Double?
    private var lastHost: UInt64?

    func beginMeasurement() {
        lock.lock()
        measuring = true
        lock.unlock()
    }

    func record(_ timestamp: AudioTimeStamp) {
        lock.lock()
        defer { lock.unlock() }
        guard measuring else { return }
        callbacks += 1
        let flags = timestamp.mFlags
        guard flags.contains(.sampleTimeValid), flags.contains(.hostTimeValid) else {
            invalidTimestamps += 1
            return
        }
        let sample = timestamp.mSampleTime
        let host = timestamp.mHostTime
        if let prevSample = lastSample, sample <= prevSample { sampleRegressions += 1 }
        if let prevHost = lastHost, host <= prevHost { hostRegressions += 1 }
        if firstSample == nil {
            firstSample = sample
            firstHost = host
        }
        lastSample = sample
        lastHost = host
    }

    struct Snapshot {
        let callbacks: Int
        let invalid: Int
        let sampleRegressions: Int
        let hostRegressions: Int
        let deltaSamples: Double
        let deltaHost: UInt64
    }

    var snapshot: Snapshot {
        lock.lock()
        defer { lock.unlock() }
        let ds = (lastSample ?? 0) - (firstSample ?? 0)
        let dh = (lastHost ?? 0) >= (firstHost ?? 0) ? (lastHost ?? 0) - (firstHost ?? 0) : 0
        return Snapshot(
            callbacks: callbacks, invalid: invalidTimestamps, sampleRegressions: sampleRegressions,
            hostRegressions: hostRegressions, deltaSamples: ds, deltaHost: dh
        )
    }
}

struct AudioZeroTimestampTest: HWTest {
    let id = "audio.zero_timestamp"
    let name = "Hardware-anchored zero timestamp monotonicity and drift"
    let subsystem = Subsystem.audio

    func run(_ context: TestContext) async throws -> Outcome {
        try Audio.activate(category: .playback, rate: 48_000)
        defer { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }
        let session = AVAudioSession.sharedInstance()
        let rate = session.sampleRate
        let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2)!
        let tracker = TimestampTracker()
        var phase = 0.0
        let source = AVAudioSourceNode(format: format) { _, timestamp, count, buffers in
            if count > 0 { tracker.record(timestamp.pointee) }
            let list = UnsafeMutableAudioBufferListPointer(buffers)
            for frame in 0 ..< Int(count) {
                let sample = Float(sin(phase) * 0.1)
                phase += 2 * .pi * 1000 / rate
                for buffer in list { buffer.mData!.assumingMemoryBound(to: Float.self)[frame] = sample }
            }
            return noErr
        }
        let engine = AVAudioEngine()
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        try engine.start()
        defer { engine.stop() }
        try await Task.sleep(nanoseconds: 250_000_000)
        let startSnap = AudioSysctl.optional()
        tracker.beginMeasurement()
        try await Task.sleep(nanoseconds: 2_000_000_000)
        let endSnap = AudioSysctl.optional()
        let stats = tracker.snapshot
        var tb = mach_timebase_info_data_t()
        mach_timebase_info(&tb)
        let hostSeconds = Double(stats.deltaHost) * Double(tb.numer) / Double(tb.denom) / 1e9
        let sampleSeconds = stats.deltaSamples / rate
        let driftPct = hostSeconds > 0 ? abs(sampleSeconds - hostSeconds) / hostSeconds * 100.0 : 100.0
        let measuredRate = hostSeconds > 0 ? stats.deltaSamples / hostSeconds : 0
        var metrics = Metrics()
        metrics.set("session_rate", Int(rate))
        metrics.set("callbacks", stats.callbacks)
        metrics.set("invalid_timestamps", stats.invalid)
        metrics.set("sample_regressions", stats.sampleRegressions)
        metrics.set("host_regressions", stats.hostRegressions)
        metrics.set("drift_pct", driftPct, digits: 4)
        metrics.set("measured_rate", measuredRate, digits: 1)
        var checks: [(String, Bool)] = [
            ("render callbacks ran across the window", stats.callbacks >= 20),
            ("every callback had valid sample and host timestamps", stats.invalid == 0),
            ("mSampleTime is strictly monotonic", stats.sampleRegressions == 0),
            ("mHostTime is strictly monotonic", stats.hostRegressions == 0),
            ("clock drift against host clock is under 0.5%", hostSeconds > 0.5 && driftPct < 0.5),
        ]
        if let s1 = startSnap, let s2 = endSnap, s2.backend == "tegra" {
            let hwDelta = s2.hwSample >= s1.hwSample ? s2.hwSample - s1.hwSample : 0
            metrics.set("hw_seed", Int(s2.hwSeed))
            metrics.set("hw_sample_delta", Int(hwDelta))
            checks.append(("hardware ZeroTimeStamp seqlock seed published", s2.hwSeed > 0))
            checks.append(("ADMA period interrupts advanced hw_sample", hwDelta >= 49_152))
        }
        return .judged(checks, metrics, proof: "route=\(Audio.outputs())")
    }
}

struct AudioRouteTest: HWTest {
    let id = "audio.route"
    let name = "Audio route"
    let subsystem = Subsystem.audio

    func run(_ context: TestContext) async throws -> Outcome {
        try Audio.activate(category: .playAndRecord)
        defer { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }
        let session = AVAudioSession.sharedInstance()
        let route = session.currentRoute
        var metrics = Metrics()
        metrics.set("outputs", Audio.outputs())
        metrics.set("inputs", route.inputs.map { $0.portType.rawValue }.joined(separator: ","))
        metrics.set("input_available", session.isInputAvailable)
        metrics.set("output_channels", session.outputNumberOfChannels)
        var checks = [
            ("an output port", !route.outputs.isEmpty), ("an input port", session.isInputAvailable),
            ("the route has an input", !route.inputs.isEmpty),
        ]
        // 27 plays as J181, whose built-in input is its Digital Mic: the Built-In Microphone port.
        if ProcessInfo.processInfo.isOperatingSystemAtLeast(
            OperatingSystemVersion(majorVersion: 27, minorVersion: 0, patchVersion: 0)
        )
        {
            let builtIn = session.availableInputs?.contains { $0.portType == .builtInMic } ?? false
            checks.append(("the built-in microphone is an input", builtIn))
        }
        // Capture needs the record permission, which only the interactive audio.microphone asks for.
        if AVAudioApplication.shared.recordPermission == .granted {
            let captured = try await Audio.captureRate(seconds: 1)
            metrics.set("captured_rate", captured.measured, digits: 0)
            checks.append(
                (
                    "input frames arrive at the input's rate (within 5%)",
                    abs(captured.measured - captured.rate) <= captured.rate * 0.05
                )
            )
        }
        return .judged(checks, metrics)
    }
}

struct HeadsetTest: HWTest {
    let id = "audio.headset"
    let name = "Headset plug detection"
    let subsystem = Subsystem.audio
    let interactive = true
    var timeout: TimeInterval { 75 }

    static let headsetPorts: Set<AVAudioSession.Port> = [.headphones, .headsetMic]

    func run(_ context: TestContext) async throws -> Outcome {
        try Audio.activate(category: .playAndRecord)
        defer { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }
        let pluggedIn = AVAudioSession.sharedInstance().currentRoute.outputs.contains {
            Self.headsetPorts.contains($0.portType)
        }
        var observer: NSObjectProtocol?
        let change: String? = await context.interact(pluggedIn ? "Unplug the headphones" : "Plug in headphones") {
            waiter in
            observer = NotificationCenter.default.addObserver(
                forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main
            ) { note in
                let reason = (note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt).flatMap(
                    AVAudioSession.RouteChangeReason.init
                )
                if reason == .newDeviceAvailable || reason == .oldDeviceUnavailable { waiter.resume(Audio.outputs()) }
            }
        }
        if let observer { NotificationCenter.default.removeObserver(observer) }
        guard let change else { return .skip("no route change (skipped or timed out)") }
        var metrics = Metrics()
        metrics.set("route", change)
        let nowPlugged = AVAudioSession.sharedInstance().currentRoute.outputs.contains {
            Self.headsetPorts.contains($0.portType)
        }
        return .judged([("the route followed the plug", nowPlugged != pluggedIn)], metrics)
    }
}

struct MicrophoneTest: HWTest {
    let id = "audio.microphone"
    let name = "Microphone capture"
    let subsystem = Subsystem.audio
    let interactive = true

    func run(_ context: TestContext) async throws -> Outcome {
        guard await AVAudioApplication.requestRecordPermission() else { return .skip("microphone permission denied") }
        try Audio.activate(category: .playAndRecord)
        defer { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }
        let engine = AVAudioEngine()
        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)
        let counter = FrameCounter()
        let peak = Waiter<Float>()
        var loudest: Float = 0
        input.installTap(onBus: 0, bufferSize: 1024, format: format) { buffer, _ in
            counter.add(Int(buffer.frameLength))
            guard let samples = buffer.floatChannelData?[0] else { return }
            let rms = sqrt(
                (0 ..< Int(buffer.frameLength)).reduce(Float(0)) { $0 + samples[$1] * samples[$1] }
                    / Float(max(1, buffer.frameLength))
            )
            loudest = max(loudest, rms)
            if counter.snapshot.frames >= Int(format.sampleRate * 3) { peak.resume(loudest) }
        }
        try engine.start()
        let start = CFAbsoluteTimeGetCurrent()
        let level: Float? = await context.interact("Talk or clap near the Switch for three seconds", timeout: 20) {
            waiter in
            Task { waiter.resume(await peak.wait(timeout: 20)) }
        }
        let elapsed = CFAbsoluteTimeGetCurrent() - start
        input.removeTap(onBus: 0)
        engine.stop()
        guard let level else { return .skip("skipped") }
        let frames = counter.snapshot.frames
        var metrics = Metrics()
        metrics.set("rate", Int(format.sampleRate))
        metrics.set("captured_rate", Double(frames) / elapsed, digits: 0)
        metrics.set("peak_dbfs", 20 * log10(Double(max(level, 1e-9))), digits: 1)
        return .judged(
            [
                (
                    "frames arrive at the input rate (within 5%)",
                    abs(Double(frames) / elapsed - format.sampleRate) <= format.sampleRate * 0.05
                ),
                ("something above -60 dBFS was heard", level > 0.001),
            ], metrics
        )
    }
}
