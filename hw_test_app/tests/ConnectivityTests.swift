import CoreBluetooth
import Foundation
import HWTestCore
import Network
import UIKit
import UniformTypeIdentifiers

struct NetworkPathTest: HWTest {
    let id = "net.path"
    let name = "Network path (Wi-Fi up)"
    let subsystem = Subsystem.connectivity

    func run(_ context: TestContext) async throws -> Outcome {
        let monitor = NWPathMonitor()
        let waiter = Waiter<NWPath>()
        monitor.pathUpdateHandler = { waiter.resume($0) }
        monitor.start(queue: DispatchQueue(label: "net.path"))
        defer { monitor.cancel() }
        guard let path = await waiter.wait(timeout: 10) else { return .fail("NWPathMonitor never reported a path") }
        var metrics = Metrics()
        metrics.set("status", "\(path.status)")
        metrics.set("interfaces", path.availableInterfaces.map { "\($0.name):\($0.type)" }.joined(separator: ","))
        metrics.set("expensive", path.isExpensive)
        return .judged(
            [("satisfied", path.status == .satisfied), ("over Wi-Fi", path.usesInterfaceType(.wifi))], metrics)
    }
}

/// Apple's connectivity check page over HTTPS: DNS, TCP, TLS and the body, end to end.
struct ReachabilityTest: HWTest {
    let id = "net.reachability"
    let name = "Fetch from a known host"
    let subsystem = Subsystem.connectivity

    func run(_ context: TestContext) async throws -> Outcome {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 15
        let session = URLSession(configuration: configuration)
        let start = CFAbsoluteTimeGetCurrent()
        let (data, response) = try await session.data(
            from: URL(string: "https://www.apple.com/library/test/success.html")!)
        let milliseconds = (CFAbsoluteTimeGetCurrent() - start) * 1000
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        var metrics = Metrics()
        metrics.set("status", status)
        metrics.set("ms", milliseconds, digits: 1)
        return .judged(
            [
                ("HTTP 200", status == 200),
                ("the Success page", String(decoding: data, as: UTF8.self).contains("Success")),
            ], metrics)
    }
}

final class BluetoothProbe: NSObject, CBCentralManagerDelegate, @unchecked Sendable {
    let state = Waiter<CBManagerState>()
    private let lock = NSLock()
    private var seen = Set<UUID>()
    private(set) var manager: CBCentralManager!

    override init() {
        super.init()
        manager = CBCentralManager(delegate: self, queue: DispatchQueue(label: "bt.probe"))
    }

    var discovered: Int {
        lock.lock()
        defer { lock.unlock() }
        return seen.count
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state != .unknown && central.state != .resetting { state.resume(central.state) }
    }

    func centralManager(
        _ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any],
        rssi: NSNumber
    ) {
        lock.lock()
        seen.insert(peripheral.identifier)
        lock.unlock()
    }
}

struct BluetoothTest: HWTest {
    let id = "net.bluetooth"
    let name = "Bluetooth power-on and scan"
    let subsystem = Subsystem.connectivity

    func run(_ context: TestContext) async throws -> Outcome {
        let probe = BluetoothProbe()
        guard let state = await probe.state.wait(timeout: 15) else {
            return .fail("CBCentralManager state never settled")
        }
        if state == .unauthorized { return .skip("Bluetooth permission denied") }
        guard state == .poweredOn else { return .fail("CBCentralManager state \(state.rawValue), not poweredOn") }
        probe.manager.scanForPeripherals(
            withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        try await Task.sleep(nanoseconds: 5_000_000_000)
        probe.manager.stopScan()
        var metrics = Metrics()
        metrics.set("peripherals", probe.discovered)
        return .pass(metrics, detail: probe.discovered == 0 ? "nothing advertising nearby" : "")
    }
}

/// A folder the person picks on a USB drive (through the Files provider): a file written, synced and read back.
struct USBStorageTest: HWTest {
    let id = "net.usb_storage"
    let name = "USB storage through the document picker"
    let subsystem = Subsystem.connectivity
    let interactive = true
    var timeout: TimeInterval { 120 }

    final class PickerDelegate: NSObject, UIDocumentPickerDelegate {
        let waiter: Waiter<URL>

        init(_ waiter: Waiter<URL>) { self.waiter = waiter }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            waiter.resume(urls.first)
        }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { waiter.resume(nil) }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        var delegate: PickerDelegate?
        let folder: URL? = await context.interact("Pick a folder on a USB drive", timeout: 90) { waiter in
            let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder])
            delegate = PickerDelegate(waiter)
            picker.delegate = delegate
            let root = UIApplication.shared.connectedScenes.compactMap {
                ($0 as? UIWindowScene)?.keyWindow?.rootViewController
            }.first
            root?.present(picker, animated: true)
        }
        guard let folder else { return .skip("no folder picked") }
        guard folder.startAccessingSecurityScopedResource() else {
            return .fail("no access to \(folder.lastPathComponent)")
        }
        defer { folder.stopAccessingSecurityScopedResource() }
        let file = folder.appendingPathComponent("switch-hwtest.bin")
        let data = Data((0..<(16 << 20)).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ $0 >> 9) })
        let writeStart = CFAbsoluteTimeGetCurrent()
        try data.write(to: file)
        let handle = try FileHandle(forUpdating: file)
        try handle.synchronize()
        try handle.close()
        let writeSeconds = CFAbsoluteTimeGetCurrent() - writeStart
        let readStart = CFAbsoluteTimeGetCurrent()
        let read = try Data(contentsOf: file, options: .uncached)
        let readSeconds = CFAbsoluteTimeGetCurrent() - readStart
        try? FileManager.default.removeItem(at: file)
        var metrics = Metrics()
        metrics.set("write_mbps", Stats.megabytesPerSecond(bytes: data.count, seconds: writeSeconds), digits: 1)
        metrics.set("read_mbps", Stats.megabytesPerSecond(bytes: data.count, seconds: readSeconds), digits: 1)
        return .judged([("reads back what was written", read == data)], metrics, proof: folder.path)
    }
}
