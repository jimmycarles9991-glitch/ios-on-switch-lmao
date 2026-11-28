import AVFoundation
import Foundation
import HWTestCore
import HWTestKit
import HWTestShared

struct AudioKernelSnapshot {
    let fields: [String: String]

    var backend: String { fields["backend"] ?? "" }
    var state: Int { Int(fields["state"] ?? "") ?? 0 }
    var powered: Int { Int(fields["powered"] ?? "") ?? 0 }
    var gates: UInt64 { UInt64(fields["gates"] ?? "") ?? 0 }
    var ungates: UInt64 { UInt64(fields["ungates"] ?? "") ?? 0 }
    var resumeUs: UInt64 { UInt64(fields["resume_us"] ?? "") ?? 0 }
    var rate: Int { Int(fields["rate"] ?? "") ?? 0 }
    var linkRate: Int { Int(fields["link_rate"] ?? "") ?? 0 }
    var pllaMilliHz: UInt64 { UInt64(fields["plla_millihz"] ?? "") ?? 0 }
    var sfc1En: Int { Int(fields["sfc1_en"] ?? "") ?? 0 }
    var sfc1Coef: Int { Int(fields["sfc1_coef"] ?? "") ?? 0 }
    var mixerEn: Int { Int(fields["mixer_en"] ?? "") ?? 0 }
    var mixerSamples: UInt32 { UInt32(fields["mixer_samples"] ?? "") ?? 0 }
    var mvcEn: Int { Int(fields["mvc_en"] ?? "") ?? 0 }
    var mvcVol: Int { Int(fields["mvc_vol"] ?? "") ?? 0 }
    var mvcMuted: Int { Int(fields["mvc_muted"] ?? "") ?? 0 }
    var mvcPeak: UInt32 { UInt32(fields["mvc_peak"] ?? "") ?? 0 }
    var mvcPeakL: UInt32 { UInt32(fields["mvc_peak_l"] ?? "") ?? 0 }
    var mvcPeakR: UInt32 { UInt32(fields["mvc_peak_r"] ?? "") ?? 0 }
    var mvcErr: UInt32 { Self.hex32(fields["mvc_err"]) }
    var opeEn: Int { Int(fields["ope_en"] ?? "") ?? 0 }
    var peqEn: Int { Int(fields["peq_en"] ?? "") ?? 0 }
    var mbdrcEn: Int { Int(fields["mbdrc_en"] ?? "") ?? 0 }
    var route: String { fields["route"] ?? "" }
    var jack: Int { Int(fields["jack"] ?? "") ?? 0 }
    var hwSeed: UInt32 { UInt32(fields["hw_seed"] ?? "") ?? 0 }
    var hwSample: UInt64 { UInt64(fields["hw_sample"] ?? "") ?? 0 }
    var underruns: UInt32 { UInt32(fields["underruns"] ?? "") ?? 0 }
    var codecOk: Int { Int(fields["codec_ok"] ?? "") ?? 0 }
    var hpVol: UInt16 { Self.hex16(fields["hp_vol"]) }
    var spkVol: UInt16 { Self.hex16(fields["spk_vol"]) }
    var addaClk1: UInt16 { Self.hex16(fields["adda_clk1"]) }
    var svZcd1: UInt16 { Self.hex16(fields["sv_zcd1"]) }

    private static func hex16(_ text: String?) -> UInt16 {
        guard let text else { return 0 }
        let stripped = text.hasPrefix("0x") ? String(text.dropFirst(2)) : text
        return UInt16(stripped, radix: 16) ?? 0
    }

    private static func hex32(_ text: String?) -> UInt32 {
        guard let text else { return 0 }
        let stripped = text.hasPrefix("0x") ? String(text.dropFirst(2)) : text
        return UInt32(stripped, radix: 16) ?? 0
    }
}

enum AudioSysctl {
    static let name = "kern.switch_audio"

    static func read() -> KernelSysctl.Read<AudioKernelSnapshot> {
        switch KernelSysctl.string(name) {
        case .value(let line):
            return .value(AudioKernelSnapshot(fields: KernelLine.fields(line)))
        case .failed(let error):
            return .failed(error)
        }
    }

    static func optional() -> AudioKernelSnapshot? {
        if case .value(let snap) = read(), snap.backend != "" { return snap }
        return nil
    }

    /// Plays a 1 kHz sine at `amplitude` while running `probe` mid-stream, returning measured frames/s.
    static func playToneWithProbe(
        seconds: Double, amplitude: Float = 0.1, probe: () async throws -> Void
    ) async throws -> Double {
        let session = AVAudioSession.sharedInstance()
        let rate = session.sampleRate
        let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2)!
        let counter = FrameCounter()
        var phase = 0.0
        let source = AVAudioSourceNode(format: format) { _, _, count, buffers in
            let list = UnsafeMutableAudioBufferListPointer(buffers)
            for frame in 0 ..< Int(count) {
                let sample = Float(sin(phase)) * amplitude
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
        defer { engine.stop() }
        try await Task.sleep(nanoseconds: 250_000_000)
        let before = counter.snapshot.frames
        let start = CFAbsoluteTimeGetCurrent()
        try await probe()
        let remain = max(0.0, seconds - (CFAbsoluteTimeGetCurrent() - start))
        if remain > 0 { try await Task.sleep(nanoseconds: UInt64(remain * 1e9)) }
        let elapsed = max(0.001, CFAbsoluteTimeGetCurrent() - start)
        return Double(counter.snapshot.frames - before) / elapsed
    }
}

/// Proves the live Tegra210 APE pipeline (MIXER1 -> SFC1 -> MVC1 -> OPE1 -> I2S1 -> RT5639) via kern.switch_audio.
struct AudioApePipelineTest: HWTest {
    let id = "audio.ape_pipeline"
    let name = "Tegra APE hardware DSP pipeline and RT5639 register state"
    let subsystem = Subsystem.audio

    func run(_ context: TestContext) async throws -> Outcome {
        let initial = AudioSysctl.read()
        guard case .value = initial else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(initial.error))
            return .skip(KernelSysctl.unreadable(AudioSysctl.name, initial.error), metrics)
        }
        try Audio.activate(category: .playback, rate: 48_000)
        defer { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }

        var firstSnap: AudioKernelSnapshot?
        var secondSnap: AudioKernelSnapshot?
        let measured = try await AudioSysctl.playToneWithProbe(seconds: 1.2) {
            try await Task.sleep(nanoseconds: 250_000_000)
            firstSnap = AudioSysctl.optional()
            try await Task.sleep(nanoseconds: 700_000_000)
            secondSnap = AudioSysctl.optional()
        }
        guard let s1 = firstSnap, let s2 = secondSnap else {
            return .fail("kern.switch_audio stopped answering during playback")
        }

        let session = AVAudioSession.sharedInstance()
        let outputVolume = session.outputVolume
        let hpL = s2.hpVol & 0x3f
        let hpR = (s2.hpVol >> 8) & 0x3f
        let opeMatchesRoute =
            (s2.route == "speaker" && s2.peqEn == 1 && s2.mbdrcEn == 0)
            || (s2.route == "headphones" && s2.peqEn == 0 && s2.mbdrcEn == 0)
        let expectedSvZcd1: UInt16 = s2.route == "headphones" ? 0x9c09 : 0x0809

        var metrics = Metrics()
        metrics.set("backend", s2.backend)
        metrics.set("route", s2.route)
        metrics.set("plla_millihz", Int(s2.pllaMilliHz))
        metrics.set("sfc1_en", s2.sfc1En)
        metrics.set("mixer_en", s2.mixerEn)
        metrics.set("mixer_delta", Int(s2.mixerSamples &- s1.mixerSamples))
        metrics.set("mvc_en", s2.mvcEn)
        metrics.set("mvc_peak", Int(s2.mvcPeak))
        metrics.set("mvc_peak_l", Int(s2.mvcPeakL))
        metrics.set("mvc_peak_r", Int(s2.mvcPeakR))
        metrics.set("mvc_err", String(format: "0x%x", s2.mvcErr))
        metrics.set("ope_en", s2.opeEn)
        metrics.set("peq_en", s2.peqEn)
        metrics.set("mbdrc_en", s2.mbdrcEn)
        metrics.set("underruns_delta", Int(s2.underruns &- s1.underruns))
        metrics.set("hp_vol", String(format: "0x%04x", s2.hpVol))
        metrics.set("adda_clk1", String(format: "0x%04x", s2.addaClk1))
        metrics.set("sv_zcd1", String(format: "0x%04x", s2.svZcd1))
        metrics.set("output_volume", Double(outputVolume), digits: 2)
        metrics.set("measured_rate", measured, digits: 0)

        var checks: [(String, Bool)] = [
            ("Tegra APE backend active and powered", s2.backend == "tegra" && s2.powered == 1),
            ("PLLA runs clean 48000.030 Hz", s2.linkRate == 48_000 && s2.pllaMilliHz == 48_000_030),
            ("SFC1 bypassed on native 48 kHz link", s2.sfc1En == 0),
            ("MIXER1 enabled and sample counter advancing", s2.mixerEn == 1 && s2.mixerSamples != s1.mixerSamples),
            ("MVC1 enabled, unmuted, and error-free", s2.mvcEn == 1 && s2.mvcMuted == 0 && s2.mvcErr == 0),
            ("OPE1 enabled with route-matched PEQ", s2.opeEn == 1 && opeMatchesRoute),
            (
                "RT5639 64x DAC / 128x ADC OSR and route-matched SV_ZCD1",
                s2.codecOk == 1 && (s2.addaClk1 & 0xf) == 0x4 && s2.svZcd1 == expectedSvZcd1
            ),
            ("RT5639 HP analog PGA at or below unity (0 dB)", hpL >= 8 && hpR >= 8),
            ("zero playback underruns across the window", s2.underruns == s1.underruns),
            ("render rate within 2% of 48 kHz", abs(measured - 48_000) <= 960),
        ]
        if outputVolume > 0.01 {
            checks.append(("MVC1 hardware peak meter saw stereo L+R audio", s2.mvcPeakL > 0 && s2.mvcPeakR > 0))
        }
        return .judged(checks, metrics, proof: "route=\(s2.route) peak=\(s2.mvcPeakL)/\(s2.mvcPeakR)")
    }
}

/// Sweeps non-native sample rates across both 48 kHz and 44.1 kHz families and verifies hardware SFC1 conversion.
struct AudioSfcRatesTest: HWTest {
    let id = "audio.sfc_rates"
    let name = "Hardware SFC multi-rate conversion across 48k and 44.1k families"
    let subsystem = Subsystem.audio
    static let rates: [Double] = [8_000, 16_000, 22_050, 32_000, 88_200, 96_000]

    func run(_ context: TestContext) async throws -> Outcome {
        let debugKernel = AudioSysctl.optional() != nil
        var metrics = Metrics()
        var checks: [(String, Bool)] = []

        for rate in Self.rates {
            try Audio.activate(category: .playback, rate: rate)
            let session = AVAudioSession.sharedInstance()
            let actualRate = session.sampleRate
            var snap: AudioKernelSnapshot?
            let measured = try await AudioSysctl.playToneWithProbe(seconds: 0.6) {
                try await Task.sleep(nanoseconds: 350_000_000)
                snap = AudioSysctl.optional()
            }
            try? session.setActive(false, options: .notifyOthersOnDeactivation)

            let tag = "\(Int(rate))"
            let expectedLink = (rate == 22_050 || rate == 88_200) ? 44_100 : 48_000
            metrics.set("rate_\(tag)_actual", Int(actualRate))
            metrics.set("rate_\(tag)_measured", measured, digits: 0)
            checks.append(("session switched to \(tag) Hz", actualRate == rate))
            checks.append(("\(tag) Hz consumed within 2%", abs(measured - actualRate) <= actualRate * 0.02))
            if debugKernel, let s = snap, s.backend == "tegra" {
                metrics.set("rate_\(tag)_link", s.linkRate)
                metrics.set("rate_\(tag)_sfc", s.sfc1Coef)
                checks.append(
                    (
                        "\(tag) Hz runs hardware SFC1 Q23 table on \(expectedLink) Hz link",
                        s.sfc1En == 1 && s.sfc1Coef == 1 && s.rate == Int(rate) && s.linkRate == expectedLink
                    )
                )
            }
        }
        return .judged(checks, metrics, proof: "rates=\(Self.rates.count)")
    }
}

/// Verifies AVAudioSession.overrideOutputAudioPort(.speaker) drives SwitchAudioSetRoute and OPE1 speaker DSP.
struct AudioRouteOverrideTest: HWTest {
    let id = "audio.route_override"
    let name = "AVAudioSession speaker route override and OPE1 DSP switch"
    let subsystem = Subsystem.audio

    func run(_ context: TestContext) async throws -> Outcome {
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .default, options: [])
        try session.setPreferredSampleRate(48_000)
        try session.setActive(true)
        defer {
            try? session.overrideOutputAudioPort(.none)
            try? session.setActive(false, options: .notifyOthersOnDeactivation)
        }

        try session.overrideOutputAudioPort(.speaker)
        var spkSnap: AudioKernelSnapshot?
        let spkMeasured = try await AudioSysctl.playToneWithProbe(seconds: 0.7) {
            try await Task.sleep(nanoseconds: 400_000_000)
            spkSnap = AudioSysctl.optional()
        }
        let spkHasBuiltIn = session.currentRoute.outputs.contains { $0.portType == .builtInSpeaker }

        try session.overrideOutputAudioPort(.none)
        var restoreSnap: AudioKernelSnapshot?
        let restoreMeasured = try await AudioSysctl.playToneWithProbe(seconds: 0.7) {
            try await Task.sleep(nanoseconds: 400_000_000)
            restoreSnap = AudioSysctl.optional()
        }

        var metrics = Metrics()
        metrics.set("spk_rate", spkMeasured, digits: 0)
        metrics.set("restore_rate", restoreMeasured, digits: 0)
        metrics.set("restore_outputs", Audio.outputs())

        var checks: [(String, Bool)] = [
            ("override(.speaker) routed to BuiltInSpeaker", spkHasBuiltIn),
            ("speaker override playback within 2%", abs(spkMeasured - session.sampleRate) <= session.sampleRate * 0.02),
            ("restored playback within 2%", abs(restoreMeasured - session.sampleRate) <= session.sampleRate * 0.02),
        ]
        if let s = spkSnap, let r = restoreSnap, s.backend == "tegra" {
            metrics.set("spk_kernel_route", s.route)
            metrics.set("spk_peq", s.peqEn)
            metrics.set("spk_mbdrc", s.mbdrcEn)
            metrics.set("restore_kernel_route", r.route)
            metrics.set("restore_peq", r.peqEn)
            checks.append((
                "kernel routed speaker with OPE1 PEQ active (MBDRC bypassed)",
                s.route == "speaker" && s.peqEn == 1 && s.mbdrcEn == 0
            ))
            if (r.jack & 1) != 0 {
                checks.append((
                    "clearing override returned to headphones with OPE1 bypassed",
                    r.route == "headphones" && r.peqEn == 0 && r.mbdrcEn == 0
                ))
            }
        }
        return .judged(checks, metrics, proof: "route=\(Audio.outputs())")
    }
}

/// Drives a full-scale 0 dBFS stereo burst through MVC1/OPE1 to confirm stereo headroom and zero underruns.
struct AudioLimiterHeadroomTest: HWTest {
    let id = "audio.limiter_headroom"
    let name = "Full-scale 0 dBFS stereo playback through MVC1 and OPE1 PEQ"
    let subsystem = Subsystem.audio

    func run(_ context: TestContext) async throws -> Outcome {
        try Audio.activate(category: .playback, rate: 48_000)
        defer { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }

        let before = AudioSysctl.optional()
        var during: AudioKernelSnapshot?
        let measured = try await AudioSysctl.playToneWithProbe(seconds: 0.8, amplitude: 0.99) {
            try await Task.sleep(nanoseconds: 450_000_000)
            during = AudioSysctl.optional()
        }
        let session = AVAudioSession.sharedInstance()

        var metrics = Metrics()
        metrics.set("measured_rate", measured, digits: 0)
        metrics.set("output_volume", Double(session.outputVolume), digits: 2)
        var checks: [(String, Bool)] = [
            ("full-scale playback kept up within 2%", abs(measured - session.sampleRate) <= session.sampleRate * 0.02),
        ]
        if let s = during, s.backend == "tegra" {
            metrics.set("route", s.route)
            metrics.set("mvc_peak", Int(s.mvcPeak))
            metrics.set("mvc_peak_l", Int(s.mvcPeakL))
            metrics.set("mvc_peak_r", Int(s.mvcPeakR))
            metrics.set("mbdrc_en", s.mbdrcEn)
            metrics.set("peq_en", s.peqEn)
            if let b = before {
                metrics.set("underruns_delta", Int(s.underruns &- b.underruns))
                checks.append(("zero underruns during 0 dBFS burst", s.underruns == b.underruns))
            }
            if session.outputVolume > 0.01 {
                checks.append(("MVC1 peak meter registered stereo L+R signal", s.mvcPeakL > 0 && s.mvcPeakR > 0))
            }
            if s.route == "speaker" {
                checks.append(("OPE1 speaker PEQ active with MBDRC bypassed", s.peqEn == 1 && s.mbdrcEn == 0))
            }
        }
        return .judged(checks, metrics, proof: "route=\(Audio.outputs())")
    }
}
