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
            for frame in 0..<Int(count) {
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
            category, mode: .default, options: category == .playAndRecord ? [.defaultToSpeaker] : [])
        if let rate { try session.setPreferredSampleRate(rate) }
        if let buffer { try session.setPreferredIOBufferDuration(buffer) }
        try session.setActive(true)
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
            ], metrics, proof: "route=\(Audio.outputs())")
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
            ], metrics)
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
        return .judged(
            [("an output port", !route.outputs.isEmpty), ("an input port", session.isInputAvailable)], metrics)
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
                    AVAudioSession.RouteChangeReason.init)
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
                (0..<Int(buffer.frameLength)).reduce(Float(0)) { $0 + samples[$1] * samples[$1] }
                    / Float(max(1, buffer.frameLength)))
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
            ], metrics)
    }
}
