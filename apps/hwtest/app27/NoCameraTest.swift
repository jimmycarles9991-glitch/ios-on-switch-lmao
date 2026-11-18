import AVFoundation
import Foundation
import HWTestCore
import HWTestKit

/// The Switch has no camera, and iOS 27's capture stack is told so: J181's camera description
/// (CMCapture.framework/J181/AVCaptureSession.plist, the overlay's) lists no capture device and no video effect, so
/// cameracaptured compiles no effect shaders at launch (FigCapturePreloadShaders) and AVFoundation discovers no
/// camera. camera_model= is MobileGestalt's HWModelDescriptionForCamera, which stays J181's.
struct NoCameraTest: HWTest {
    let id = "system.no_camera"
    let name = "No camera: J181's camera description and capture devices"
    let subsystem = Subsystem.system

    static let description = "/System/Library/PrivateFrameworks/CMCapture.framework/J181/AVCaptureSession.plist"
    static let types: [AVCaptureDevice.DeviceType] = [
        .builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera, .builtInDualCamera,
        .builtInDualWideCamera, .builtInTripleCamera, .builtInTrueDepthCamera, .builtInLiDARDepthCamera,
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let plist = NSDictionary(contentsOfFile: Self.description)
        let devices = AVCaptureDevice.DiscoverySession(deviceTypes: Self.types, mediaType: nil, position: .unspecified)
            .devices
        var metrics = Metrics()
        metrics.set("description_keys", plist.map { $0.count } ?? -1)
        metrics.set("devices", devices.count)
        metrics.set("camera_model", Self.cameraModel() ?? "-")
        return .judged(
            [
                ("J181's camera description is a dictionary", plist != nil),
                ("it lists no capture device", plist?["AVCaptureDevices"] == nil),
                ("it states no video effect", plist?["AVCaptureDeviceCommonSettings"] == nil),
                ("AVFoundation discovers no capture device", devices.isEmpty),
            ], metrics)
    }

    static func cameraModel() -> String? {
        typealias CopyAnswer = @convention(c) (CFString, CFDictionary?) -> Unmanaged<CFTypeRef>?
        guard let library = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW),
            let symbol = dlsym(library, "MGCopyAnswer")
        else { return nil }
        let copy = unsafeBitCast(symbol, to: CopyAnswer.self)
        return copy("HWModelDescriptionForCamera" as CFString, nil)?.takeRetainedValue() as? String
    }
}
