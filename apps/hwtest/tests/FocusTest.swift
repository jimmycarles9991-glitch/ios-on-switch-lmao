import CoreFoundation
import Darwin
import Foundation
import GameController
import HWTestCore
import HWTestKit
import UIKit

/// Whether UIKit runs a focus system with no keyboard and no game controller connected. UIKitCore 24B119 sets one up in
/// every scene while a hardware keyboard or a Game Pad / Joystick HID device exists, and its programmatic focus updates
/// then stall the main thread about once a second (drivers/SwitchJoyCon/SwitchJoyConUse.h). Checks raw IOKit HID
/// presence alongside GCController so cold XPC discovery to gamecontrollerd cannot misclassify an active controller as
/// a focus leak.
struct FocusIdleTest: HWTest {
    let id = "input.focus_idle"
    let name = "No focus system without a keyboard or game controller"
    let subsystem = Subsystem.input

    /// Counts IOHIDDevice entries declaring Generic Desktop / Joystick (1/4) or Game Pad (1/5), the exact usage pairs
    /// UIKit's _UIFocusGameControllerObserver monitors directly via IOHIDManager.
    static func hidGamepadCount() -> Int {
        typealias Matching = @convention(c) (UnsafePointer<CChar>) -> UnsafeMutableRawPointer?
        typealias GetServices = @convention(c) (
            mach_port_t, UnsafeMutableRawPointer?, UnsafeMutablePointer<mach_port_t>
        ) -> kern_return_t
        typealias Next = @convention(c) (mach_port_t) -> mach_port_t
        typealias CopyProp = @convention(c) (
            mach_port_t, CFString, CFAllocator?, UInt32
        ) -> Unmanaged<CFTypeRef>?
        typealias Release = @convention(c) (mach_port_t) -> kern_return_t

        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
              let matchingSym = dlsym(iokit, "IOServiceMatching"),
              let servicesSym = dlsym(iokit, "IOServiceGetMatchingServices"),
              let nextSym = dlsym(iokit, "IOIteratorNext"),
              let propSym = dlsym(iokit, "IORegistryEntryCreateCFProperty"),
              let releaseSym = dlsym(iokit, "IOObjectRelease")
        else { return 0 }

        let matching = unsafeBitCast(matchingSym, to: Matching.self)
        let getServices = unsafeBitCast(servicesSym, to: GetServices.self)
        let next = unsafeBitCast(nextSym, to: Next.self)
        let copyProp = unsafeBitCast(propSym, to: CopyProp.self)
        let release = unsafeBitCast(releaseSym, to: Release.self)

        var iterator: mach_port_t = 0
        guard getServices(0, matching("IOHIDDevice"), &iterator) == KERN_SUCCESS, iterator != 0 else { return 0 }
        defer { _ = release(iterator) }

        var count = 0
        while true {
            let service = next(iterator)
            if service == 0 { break }
            defer { _ = release(service) }
            if isGamepadService(service, copyProp: copyProp) { count += 1 }
        }
        return count
    }

    private static func isGamepadService(
        _ service: mach_port_t,
        copyProp: @convention(c) (mach_port_t, CFString, CFAllocator?, UInt32) -> Unmanaged<CFTypeRef>?
    ) -> Bool {
        let page = copyProp(service, "PrimaryUsagePage" as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue() as? Int
        let usage = copyProp(service, "PrimaryUsage" as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue() as? Int
        if page == 1 && (usage == 4 || usage == 5) { return true }
        if let pairs = copyProp(service, "DeviceUsagePairs" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
            as? [[String: Any]]
        {
            for pair in pairs {
                let page = (pair["DeviceUsagePage"] as? NSNumber)?.intValue
                let usage = (pair["DeviceUsage"] as? NSNumber)?.intValue
                if page == 1 && (usage == 4 || usage == 5) { return true }
            }
        }
        return false
    }

    static func railGamepadPublished() -> Bool {
        guard case .value(let rails) = RailTest.read() else { return false }
        return rails.values.contains { ($0.counters["gamepad"] ?? 0) != 0 }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        var (controllers, keyboard, focus) = await MainActor.run { () -> (Int, Bool, Bool) in
            let window = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.keyWindow }.first
            let system = window.flatMap { UIFocusSystem.focusSystem(for: $0) }
            return (GCController.controllers().count, GCKeyboard.coalesced != nil, system != nil)
        }
        let hidGamepads = Self.hidGamepadCount()
        let railGamepad = Self.railGamepadPublished()
        if controllers == 0 && (hidGamepads > 0 || railGamepad) {
            for _ in 0 ..< 10 {
                try await Task.sleep(nanoseconds: 50_000_000)
                (controllers, keyboard, focus) = await MainActor.run { () -> (Int, Bool, Bool) in
                    let window = UIApplication.shared.connectedScenes
                        .compactMap { ($0 as? UIWindowScene)?.keyWindow }.first
                    let system = window.flatMap { UIFocusSystem.focusSystem(for: $0) }
                    return (GCController.controllers().count, GCKeyboard.coalesced != nil, system != nil)
                }
                if controllers > 0 { break }
            }
        }
        var metrics = Metrics()
        metrics.set("controllers", controllers)
        metrics.set("hid_gamepads", hidGamepads)
        metrics.set("keyboard", keyboard)
        metrics.set("focus_system", focus)
        guard controllers == 0, hidGamepads == 0, !railGamepad, !keyboard else {
            return .skip("a game controller or keyboard is connected", metrics)
        }
        return .judged([("no focus system without a keyboard or game controller", !focus)], metrics)
    }
}
