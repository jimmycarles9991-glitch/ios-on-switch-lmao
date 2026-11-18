import Foundation
import ImageIO
import SwitchInstallerInput
import SwitchInstallerNative
import UIKit

struct InstallerPackage {
    let info: SwitchPackage
    let url: URL
    let source: String
    let isUpdate: Bool

    var icon: UIImage? {
        guard let data = info.iconData,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? Int,
              let height = properties[kCGImagePropertyPixelHeight] as? Int,
              width > 0, height > 0, width <= 1024, height <= 1024
        else { return nil }
        return UIImage(data: data)
    }
}

@MainActor
final class SwitchInstallerModel: ObservableObject {
    @Published private(set) var phase: InstallerPhase = .idle
    @Published private(set) var package: InstallerPackage?
    @Published private(set) var technicalDetails = ""
    @Published private(set) var installProgress = InstallerProgress()
    @Published private(set) var importName: String?
    @Published private(set) var copyFraction: Double?
    @Published private(set) var notice: String?
    private var generation = UUID()
    private var copyTask: Task<Void, Error>?
    private var download: SwitchInstallerDownload?
    private var workingDirectory: URL?
    private var backgroundTask: UIBackgroundTaskIdentifier = .invalid

    var showsImportDialog: Bool {
        importName != nil && (phase == .importing || phase == .checking)
    }

    private func begin() throws -> (UUID, URL) {
        guard !phase.isBusy else { throw InstallerFailure("An installation task is already in progress.") }
        clear()
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("SwitchInstaller", isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        workingDirectory = directory
        return (generation, directory.appendingPathComponent("package.ipa"))
    }

    func importFile(_ url: URL) {
        guard !phase.isBusy else {
            notice = "Wait for the current task to finish, or cancel it before opening another IPA."
            return
        }
        guard url.isFileURL else {
            fail(InstallerFailure("Open an IPA from Files, or use Download from Link."))
            return
        }
        do {
            let (token, destination) = try begin()
            importName = url.lastPathComponent
            phase = .importing
            let worker = Task.detached(priority: .userInitiated) {
                try InstallerImport.copy(from: url, to: destination) { bytes, total in
                    DispatchQueue.main.async {
                        guard self.generation == token, self.phase == .importing else { return }
                        self.copyFraction = Double(bytes) / Double(total)
                    }
                }
            }
            copyTask = worker
            Task {
                do {
                    try await worker.value
                    await inspect(destination, source: url.lastPathComponent, token: token)
                } catch {
                    finishPreparation(error, token: token, directory: destination.deletingLastPathComponent())
                }
            }
        } catch {
            fail(error)
        }
    }

    func downloadLink(_ text: String) {
        guard !phase.isBusy else { return }
        do {
            let url = try InstallerInput.downloadURL(text)
            let (token, destination) = try begin()
            phase = .downloading(0, -1)
            let transfer = SwitchInstallerDownload(destination: destination)
            download = transfer
            transfer.start(
                url: url,
                progress: { [weak self] received, expected in
                    guard let self, self.generation == token else { return }
                    self.phase = .downloading(received, expected)
                },
                completion: { [weak self] result in
                    guard let self else { return }
                    guard self.generation == token else {
                        try? FileManager.default.removeItem(at: destination.deletingLastPathComponent())
                        return
                    }
                    self.download = nil
                    switch result {
                    case .success(let staged):
                        Task { await self.inspect(staged, source: url.host ?? "Web download", token: token) }
                    case .failure(let error):
                        self.finishPreparation(error, token: token, directory: destination.deletingLastPathComponent())
                    }
                }
            )
        } catch {
            fail(error)
        }
    }

    private func inspect(_ url: URL, source: String, token: UUID) async {
        guard generation == token else {
            try? FileManager.default.removeItem(at: url.deletingLastPathComponent())
            return
        }
        phase = .checking
        let systemVersion = UIDevice.current.systemVersion
        do {
            let selected = try await Task.detached(priority: .userInitiated) {
                let info = try SwitchPackage.inspectURL(url, systemVersion: systemVersion)
                let available = try url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
                    .volumeAvailableCapacityForImportantUsage
                let compressed = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
                let required = info.expandedSize + UInt64(compressed) + 128 * 1024 * 1024
                if let available, available >= 0, UInt64(available) < required {
                    throw InstallerFailure(
                        "There isn’t enough free space to install this app. Free some space and try again."
                    )
                }
                return InstallerPackage(
                    info: info, url: url, source: source, isUpdate: SwitchInstallation.isInstalled(info.identifier)
                )
            }.value
            guard generation == token else {
                try? FileManager.default.removeItem(at: url.deletingLastPathComponent())
                return
            }
            package = selected
            phase = .ready
        } catch {
            finishPreparation(error, token: token, directory: url.deletingLastPathComponent())
        }
    }

    func install() {
        guard phase.canInstall, let package else { return }
        generation = UUID()
        let token = generation
        installProgress = InstallerProgress()
        phase = .installing
        technicalDetails = ""
        backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "SwitchInstall") { [weak self] in
            Task { @MainActor in self?.endBackgroundTask() }
        }
        Task {
            defer { endBackgroundTask() }
            do {
                try await Task.detached(priority: .userInitiated) {
                    let identifier = package.info.identifier
                    try SwitchInstallation.installURL(package.url, identifier: identifier) { status, number in
                        let value = number?.doubleValue
                        DispatchQueue.main.async {
                            guard self.generation == token, self.phase == .installing else { return }
                            self.installProgress.update(status: status, percent: value)
                        }
                    }
                }.value
                guard generation == token else { return }
                phase = .installed
                if let directory = workingDirectory {
                    try? FileManager.default.removeItem(at: directory)
                    workingDirectory = nil
                }
            } catch {
                guard generation == token else { return }
                fail(error, retryInstallation: true)
            }
        }
    }

    func openInstalledApp() {
        guard let package, phase == .installed else { return }
        if !SwitchInstallation.openApplication(package.info.identifier) {
            notice = "The app was installed, but could not be opened. Try opening it from the Home Screen."
        }
    }

    func dismissNotice() {
        notice = nil
    }

    func dismissReview() {
        guard phase != .installing else { return }
        if phase.canCancel {
            cancel()
        } else {
            clear()
        }
    }

    func cancel() {
        guard phase.canCancel else { return }
        download?.cancel()
        download = nil
        clear()
    }

    func report(_ error: Error) {
        guard !phase.isBusy else { return }
        fail(error)
    }

    private func finishPreparation(_ error: Error, token: UUID, directory: URL) {
        try? FileManager.default.removeItem(at: directory)
        guard generation == token else { return }
        workingDirectory = nil
        fail(error)
    }

    private func fail(_ error: Error, retryInstallation: Bool = false) {
        phase = .failed(error.localizedDescription, retryInstallation: retryInstallation)
        technicalDetails = InstallerError.details(error)
    }

    private func clear() {
        generation = UUID()
        copyTask?.cancel()
        copyTask = nil
        importName = nil
        copyFraction = nil
        package = nil
        technicalDetails = ""
        phase = .idle
        if let directory = workingDirectory {
            try? FileManager.default.removeItem(at: directory)
            workingDirectory = nil
        }
    }

    private func endBackgroundTask() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }
}
