import AVFoundation
import Darwin
import HWTestCore
import HWTestVideo
import UIKit

/// The display's second window (window B, the hardware plane): Core Animation detaches a playing video's layer onto
/// it when the display takes it, so the video costs no GPU composite per frame. Plays the H.264 1080p clip in an
/// AVPlayerLayer and reads the kernel's plane counters around it (kern.switch_planes_*, debug kernels): window B has
/// to have flipped and latched, and no layer Core Animation detached may have gone unshown.
struct PlanesTest: HWTest {
    let id = "display.planes"
    let name = "Video on the display's hardware plane (window B)"
    let subsystem = Subsystem.compositor

    static let counters = ["flips", "latched", "maps", "dropped"]

    func run(_ context: TestContext) async throws -> Outcome {
        let before = Self.totals()
        guard !before.isEmpty else { return .skip("the kernel has no kern.switch_planes_* (a release kernel)") }
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 1080 }) else {
            return .skip("no H.264 1080p clip")
        }
        let asset = AVURLAsset(url: context.media.appendingPathComponent(clip.file))
        let duration = try await asset.load(.duration)
        nonisolated(unsafe) let source = asset
        let (item, player) = await MainActor.run { () -> (AVPlayerItem, AVPlayer) in
            let item = AVPlayerItem(asset: source)
            return (item, AVPlayer(playerItem: item))
        }
        await context.onStage { stage in
            let view = PlayerView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            view.playerLayer.player = player
            view.playerLayer.videoGravity = .resizeAspect
            stage.addSubview(view)
        }
        let ended = Waiter<Bool>()
        let observer = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemDidPlayToEndTime, object: item, queue: .main
        ) { _ in ended.resume(true) }
        defer { NotificationCenter.default.removeObserver(observer) }
        await MainActor.run { player.play() }
        let finished = await ended.wait(timeout: duration.seconds + 15)
        await MainActor.run { player.pause() }
        let after = Self.totals()
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }

        var metrics = Metrics()
        for name in Self.counters { metrics.set("d_" + name, delta(name)) }
        return .judged(
            [
                ("played to the end", finished == true),
                ("window B flipped (d_flips > 0)", delta("flips") > 0),
                ("window B took its sets (d_latched > 0)", delta("latched") > 0),
                ("no detached layer went unshown (d_dropped 0)", delta("dropped") == 0),
            ], metrics)
    }

    /// kern.switch_planes_* by name; empty on a kernel without them.
    static func totals() -> [String: Int] {
        var values: [String: Int] = [:]
        for name in counters {
            var value: Int32 = 0
            var size = MemoryLayout<Int32>.size
            if sysctlbyname("kern.switch_planes_" + name, &value, &size, nil, 0) == 0 { values[name] = Int(value) }
        }
        return values
    }
}

/// CoreVideo describes none of Apple's compressed pixel formats (AGX, HTPC, universal): tiled layouts that only Apple's
/// GPU, decoders and display write or read. Without a description no producer allocates a surface in one, so none
/// reaches the compositor or the display.
struct PlainSurfacesTest: HWTest {
    let id = "display.plain_surfaces"
    let name = "No compressed pixel formats in CoreVideo"
    let subsystem = Subsystem.compositor

    static let formats = ["&BGA", "&8v0", "&8f0"]

    func run(_ context: TestContext) async throws -> Outcome {
        let checks = Self.formats.map { name -> (String, Bool) in
            let format = name.utf8.reduce(OSType(0)) { $0 << 8 | OSType($1) }
            let described = CVPixelFormatDescriptionCreateWithPixelFormatType(nil, format) != nil
            var buffer: CVPixelBuffer?
            let attributes = [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary
            let made = CVPixelBufferCreate(nil, 1280, 720, format, attributes, &buffer) == kCVReturnSuccess
            return ("CoreVideo neither describes nor allocates '\(name)'", !described && !made)
        }
        return .judged(checks, Metrics())
    }
}
