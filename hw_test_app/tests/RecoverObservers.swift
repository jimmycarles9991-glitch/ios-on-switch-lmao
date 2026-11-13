import Foundation
import HWTestCore
import Metal
import QuartzCore
import UIKit

/// Another raw client working while gpu.recover.kill_one kills this process's channel: the bench's raw device, over a
/// connection and channel of its own, fills a target on the 2D engine back to back on a thread of its own (no program and no
/// pack record, so nothing of the bench's own draws), reads every texel back against the fill, and keeps its failures and
/// the longest wait between two finished runs.
final class RecoverBystander: @unchecked Sendable {
    /// The same byte in every channel, whichever order the solid colour's format packs them in
    static let fill = 0x8080_8080
    /// NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT_V_A8B8G8R8: the solid colour as one RGBA8 word
    static let a8b8g8r8 = 0xd5
    private let runner: BenchRunner
    private let experiment: BenchExperiment
    private let lock = NSLock()
    private let ended = DispatchSemaphore(value: 0)
    private var stopping = false, lastNs: UInt64 = 0
    private(set) var runs = 0, errors = 0, longestNs: UInt64 = 0
    private(set) var firstError = ""

    init(_ runner: BenchRunner) throws {
        self.runner = runner
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "bystander",
            "experiments": [
                [
                    "id": "bystander", "kind": "twod", "readback": "full", "repeats": 4,
                    "target": ["format": "rgba8unorm", "width": 64, "height": 64],
                    "twod": ["op": "fill", "colorFormat": Self.a8b8g8r8, "color": [Self.fill, 0, 0, 0]],
                ]
            ],
        ]
        let data = try JSONSerialization.data(withJSONObject: batch)
        guard case .experiment(let e) = try BenchBatch.decode(data).entries.first else {
            throw TestFailure("the bystander's experiment does not decode")
        }
        experiment = e
    }

    func start() {
        lastNs = DispatchTime.now().uptimeNanoseconds
        Thread { [self] in
            while !self.locked({ self.stopping }) {
                let outcome = self.runner.run(self.experiment)
                let now = DispatchTime.now().uptimeNanoseconds
                self.locked {
                    self.runs += 1
                    let wrong = outcome.status == "ok" && !(outcome.texels.map(Self.filled) ?? false)
                    if outcome.status != "ok" || wrong {
                        self.errors += 1
                        if self.firstError.isEmpty {
                            self.firstError = wrong ? "texels:not the fill" : "\(outcome.status):\(outcome.why)"
                        }
                    }
                    self.longestNs = max(self.longestNs, now - self.lastNs)
                    self.lastNs = now
                }
            }
            self.ended.signal()
        }.start()
    }

    static func filled(_ texels: Data) -> Bool {
        texels.count == 64 * 64 * 4 && texels.allSatisfy { $0 == 0x80 }
    }

    /// From now on: the runs, failures and waits before (the first, cold run among them) are not counted.
    func measure() {
        locked {
            runs = 0
            errors = 0
            longestNs = 0
            firstError = ""
            lastNs = DispatchTime.now().uptimeNanoseconds
        }
    }

    /// False: a run still had not returned after `seconds`.
    func stop(seconds: Double) -> Bool {
        locked { stopping = true }
        return ended.wait(timeout: .now() + seconds) == .success
    }

    private func locked<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }
}

/// The compositor's side: a probe layer whose drawable each display-link frame is filled by a bench raw device (a second
/// connection and channel of this process, not the one being killed) and presented with no command buffer, so the
/// render server composites a frame each time, with the times the display showed each drawable (ShownTimes, as the
/// pacing tests take them). fillErrors= counts the frames the bench did not fill.
@MainActor
final class RecoverCompositorProbe: NSObject {
    let probe = CAMetalLayer()
    private let bench: BenchRunner
    private let shown = ShownTimes()
    private var frame: UInt32 = 0
    private(set) var fillErrors = 0
    private(set) var firstFillError = ""
    private var link: CADisplayLink?
    private var measuring = false

    init(device: MTLDevice, bench: BenchRunner) {
        self.bench = bench
        probe.device = device
        probe.pixelFormat = .bgra8Unorm
        probe.framebufferOnly = true
    }

    var presented: [Double] { shown.measured }

    func start() {
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    /// Presented times count from now on.
    func measure() { measuring = true }

    func stop() {
        link?.invalidate()
        link = nil
    }

    @objc private func tick(_: CADisplayLink) {
        guard let drawable = probe.nextDrawable() else { return }
        frame += 1
        let filled =
            drawable.texture.iosurface.map {
                bench.fill($0, format: probe.pixelFormat, color: 0xff20_8000 | (frame % 60) * 4)
            } ?? "no IOSurface under the drawable"
        if filled != "ok" {
            fillErrors += 1
            if firstFillError.isEmpty { firstFillError = filled }
        }
        let shown = shown
        let submitted = CACurrentMediaTime()
        let measured = measuring
        drawable.addPresentedHandler { shown.add($0.presentedTime, submitted: submitted, measured: measured) }
        drawable.present()
    }
}
