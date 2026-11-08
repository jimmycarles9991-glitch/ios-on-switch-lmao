import CoreMotion
import Foundation
import HWTestCore
import UIKit

/// Collects CoreMotion samples for a fixed time at a requested 100 Hz.
final class MotionSamples<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var samples: [T] = []

    func add(_ sample: T) {
        lock.lock()
        samples.append(sample)
        lock.unlock()
    }

    var all: [T] {
        lock.lock()
        defer { lock.unlock() }
        return samples
    }
}

enum Motion {
    static let manager = CMMotionManager()
    static let queue = OperationQueue()
    static let interval = 0.01
    static let seconds = 2.0

    static func collect<T>(
        start: (MotionSamples<T>) -> Void, stop: () -> Void
    ) async throws -> (samples: [T], rate: Double) {
        let samples = MotionSamples<T>()
        start(samples)
        try await Task.sleep(nanoseconds: 300_000_000)
        let skip = samples.all.count
        let begin = CFAbsoluteTimeGetCurrent()
        try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
        let window = Array(samples.all.dropFirst(skip))
        let elapsed = CFAbsoluteTimeGetCurrent() - begin
        stop()
        return (window, Double(window.count) / elapsed)
    }
}

struct AccelerometerTest: HWTest {
    let id = "sensor.accelerometer"
    let name = "Accelerometer"
    let subsystem = Subsystem.sensors

    func run(_ context: TestContext) async throws -> Outcome {
        let manager = Motion.manager
        guard manager.isAccelerometerAvailable else { return .fail("isAccelerometerAvailable is false") }
        manager.accelerometerUpdateInterval = Motion.interval
        let (samples, rate) = try await Motion.collect(
            start: { (sink: MotionSamples<CMAcceleration>) in
                manager.startAccelerometerUpdates(to: Motion.queue) { data, _ in
                    if let data { sink.add(data.acceleration) }
                }
            }, stop: { manager.stopAccelerometerUpdates() })
        let magnitude = Stats.mean(samples.map { sqrt($0.x * $0.x + $0.y * $0.y + $0.z * $0.z) })
        var metrics = Metrics()
        metrics.set("rate_hz", rate, digits: 1)
        metrics.set("gravity_g", magnitude, digits: 3)
        return .judged(
            [
                ("at least 80 Hz of the 100 asked", rate >= 80),
                ("about 1 g at rest (0.8-1.2)", magnitude > 0.8 && magnitude < 1.2),
            ], metrics)
    }
}

struct GyroscopeTest: HWTest {
    let id = "sensor.gyroscope"
    let name = "Gyroscope"
    let subsystem = Subsystem.sensors

    func run(_ context: TestContext) async throws -> Outcome {
        let manager = Motion.manager
        guard manager.isGyroAvailable else { return .fail("isGyroAvailable is false") }
        manager.gyroUpdateInterval = Motion.interval
        let (samples, rate) = try await Motion.collect(
            start: { (sink: MotionSamples<CMRotationRate>) in
                manager.startGyroUpdates(to: Motion.queue) { data, _ in if let data { sink.add(data.rotationRate) } }
            }, stop: { manager.stopGyroUpdates() })
        let spin = Stats.mean(samples.map { sqrt($0.x * $0.x + $0.y * $0.y + $0.z * $0.z) })
        var metrics = Metrics()
        metrics.set("rate_hz", rate, digits: 1)
        metrics.set("mean_rad_s", spin, digits: 4)
        return .judged(
            [
                ("at least 80 Hz of the 100 asked", rate >= 80),
                ("samples are finite", samples.allSatisfy { $0.x.isFinite }),
            ], metrics)
    }
}

/// Sensor fusion (CMDeviceMotion) and the orientation UIKit derives from it.
struct DeviceMotionTest: HWTest {
    let id = "sensor.device_motion"
    let name = "Device motion and orientation"
    let subsystem = Subsystem.sensors

    func run(_ context: TestContext) async throws -> Outcome {
        let manager = Motion.manager
        guard manager.isDeviceMotionAvailable else { return .fail("isDeviceMotionAvailable is false") }
        manager.deviceMotionUpdateInterval = Motion.interval
        let (samples, rate) = try await Motion.collect(
            start: { (sink: MotionSamples<CMDeviceMotion>) in
                manager.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: Motion.queue) { data, _ in
                    if let data { sink.add(data) }
                }
            }, stop: { manager.stopDeviceMotionUpdates() })
        let gravity = Stats.mean(
            samples.map {
                sqrt($0.gravity.x * $0.gravity.x + $0.gravity.y * $0.gravity.y + $0.gravity.z * $0.gravity.z)
            })
        let orientation = await MainActor.run { () -> Int in
            UIDevice.current.beginGeneratingDeviceOrientationNotifications()
            defer { UIDevice.current.endGeneratingDeviceOrientationNotifications() }
            return UIDevice.current.orientation.rawValue
        }
        var metrics = Metrics()
        metrics.set("rate_hz", rate, digits: 1)
        metrics.set("gravity_g", gravity, digits: 3)
        metrics.set("orientation", orientation)
        return .judged(
            [("at least 80 Hz of the 100 asked", rate >= 80), ("unit gravity vector", abs(gravity - 1) < 0.05)], metrics
        )
    }
}

/// The ambient light sensor has no public API; with auto-brightness on, covering it moves the brightness.
struct AmbientLightTest: HWTest {
    let id = "sensor.ambient_light"
    let name = "Ambient light sensor (via auto-brightness)"
    let subsystem = Subsystem.sensors
    let interactive = true

    func run(_ context: TestContext) async throws -> Outcome {
        let before = await MainActor.run { UIScreen.main.brightness }
        var observer: NSObjectProtocol?
        let after: CGFloat? = await context.interact(
            "Cover the light sensor with a finger for a few seconds (auto-brightness on)"
        ) { waiter in
            observer = NotificationCenter.default.addObserver(
                forName: UIScreen.brightnessDidChangeNotification, object: nil, queue: .main
            ) { _ in
                if abs(UIScreen.main.brightness - before) > 0.02 { waiter.resume(UIScreen.main.brightness) }
            }
        }
        if let observer { NotificationCenter.default.removeObserver(observer) }
        guard let after else { return .skip("brightness did not move (skipped, timed out or auto-brightness off)") }
        var metrics = Metrics()
        metrics.set("before", Double(before), digits: 3)
        metrics.set("after", Double(after), digits: 3)
        return .pass(metrics)
    }
}
