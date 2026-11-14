import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// Another raw client working while gpu.recover.kill_one or kill_under_compositor kills this process's channel: a bench
/// raw device over a connection and channel of its own, on a thread of its own, back to back, every texel read back
/// against the fill (no program and no pack record, so nothing of the bench's own draws). On GR it fills a small target
/// with the 2D engine; on the copy engine it fills a 512x512 block-linear target whose texels go in and out through
/// the copy engine around each fill, so a kill most often lands with its copies in flight. A failure names it (`role`)
/// and the kernel's error; a kill of its channel is also its reopen (the bench opens a fresh device after one).
final class RecoverBystander: @unchecked Sendable {
    enum Engine: String {
        case gr = "the GR bystander's bench channel"
        case copy = "the copy-engine bystander's bench channel"
    }

    /// The same byte in every channel, whichever order the solid colour's format packs them in
    static let fill = 0x8080_8080
    /// NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT_V_A8B8G8R8: the solid colour as one RGBA8 word
    static let a8b8g8r8 = 0xd5
    let role: Engine
    private let runner: BenchRunner
    private let experiment: BenchExperiment
    private let side: Int
    private let lock = NSLock()
    private let ended = DispatchSemaphore(value: 0)
    private var stopping = false, lastNs: UInt64 = 0
    private(set) var runs = 0, errors = 0, kills = 0, longestNs: UInt64 = 0
    private(set) var firstError = ""

    init(_ runner: BenchRunner, on role: Engine = .gr) throws {
        self.runner = runner
        self.role = role
        side = role == .gr ? 64 : 512
        var twod: [String: Any] = ["op": "fill", "colorFormat": Self.a8b8g8r8, "color": [Self.fill, 0, 0, 0]]
        if role == .copy { twod["tiled"] = true }
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "bystander",
            "experiments": [
                [
                    "id": "bystander", "kind": "twod", "readback": "full", "repeats": 4,
                    "target": ["format": "rgba8unorm", "width": side, "height": side],
                    "twod": twod,
                ]
            ],
        ]
        let data = try JSONSerialization.data(withJSONObject: batch)
        guard case .experiment(let e) = try BenchBatch.decode(data).entries.first else {
            throw TestFailure("the bystander's experiment does not decode")
        }
        experiment = e
    }

    /// "<role>: <its first failure>", or "" with none
    var failure: String { firstError.isEmpty ? "" : "\(role.rawValue): \(firstError)" }

    func start() {
        lastNs = DispatchTime.now().uptimeNanoseconds
        Thread { [self] in
            while !self.locked({ self.stopping }) {
                let outcome = self.runner.run(self.experiment)
                let now = DispatchTime.now().uptimeNanoseconds
                self.locked {
                    self.runs += 1
                    let wrong = outcome.status == "ok" && !(outcome.texels.map(self.filled) ?? false)
                    if outcome.status == "killed" { self.kills += 1 }
                    if outcome.status != "ok" || wrong {
                        self.errors += 1
                        if self.firstError.isEmpty {
                            self.firstError =
                                wrong
                                ? "texels:not the fill"
                                : "\(outcome.status):\(outcome.why) (kernel error \(outcome.lostError))"
                        }
                    }
                    self.longestNs = max(self.longestNs, now - self.lastNs)
                    self.lastNs = now
                }
            }
            self.ended.signal()
        }.start()
    }

    func filled(_ texels: Data) -> Bool {
        texels.count == side * side * 4 && texels.allSatisfy { $0 == 0x80 }
    }

    /// From now on: the runs, failures and waits before (the first, cold run among them) are not counted.
    func measure() {
        locked {
            runs = 0
            errors = 0
            kills = 0
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

extension RecoverKillOneTest {
    /// One command buffer on the device, completed: a channel an earlier test left killed is made again here, so the
    /// counters read after it count this test's reopens alone (boot 21's kill_one counted gpu.raw.fetch_rung3's).
    static func settle(_ gpu: GPU) throws {
        let scratch = try gpu.buffer(256)
        guard let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() else {
            throw TestFailure("no command buffer")
        }
        blit.fill(buffer: scratch, range: 0..<256, value: 0)
        blit.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
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
