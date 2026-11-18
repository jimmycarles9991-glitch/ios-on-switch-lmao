import Foundation
import SwitchInstallerInput
import XCTest

final class SwitchInputTests: XCTestCase {
    func testStageOnlyUpdatesKeepRealProgressAndStaleReportsDoNotRewindIt() {
        var progress = InstallerProgress()
        XCTAssertNil(progress.percent)
        progress.update(status: "ExtractingPackage", percent: 40)
        progress.update(status: "VerifyingApplication", percent: nil)
        progress.update(status: "ExtractingPackage", percent: 15)
        XCTAssertEqual(progress.percent, 40)
        XCTAssertEqual(progress.title, "Verifying app…")
        progress.update(status: "Complete", percent: 100)
        XCTAssertEqual(progress.title, "Finishing installation…")
    }

    func testInstallationDetailsIncludeEveryUnderlyingFailure() {
        let root = NSError(domain: NSPOSIXErrorDomain, code: 13, userInfo: [
            NSLocalizedDescriptionKey: "Permission denied",
            NSDebugDescriptionErrorKey: "Archive could not be read",
        ])
        let middle = NSError(domain: "MIInstallerErrorDomain", code: 7, userInfo: [
            NSLocalizedDescriptionKey: "Failed to stage package",
            NSUnderlyingErrorKey: root,
        ])
        let outer = NSError(domain: "Installation", code: 1, userInfo: [
            NSLocalizedDescriptionKey: "Install failed",
            NSLocalizedFailureReasonErrorKey: "Staging failed",
            NSUnderlyingErrorKey: middle,
        ])
        XCTAssertEqual(InstallerError.details(outer), """
        Installation (1): Install failed Staging failed
        MIInstallerErrorDomain (7): Failed to stage package
        NSPOSIXErrorDomain (13): Permission denied Archive could not be read
        """)
    }

    func testInstallationDetailsBoundDeepErrorChains() {
        var error = NSError(domain: "Installation", code: 0)
        for index in 1 ... 100 {
            error = NSError(domain: "Installation", code: index, userInfo: [NSUnderlyingErrorKey: error])
        }
        XCTAssertEqual(InstallerError.details(error).split(separator: "\n").count, 16)
    }

    func testImportKeepsAnIndependentCopyWithoutChangingTheSource() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source.ipa")
        let destination = root.appendingPathComponent("staging/package.ipa")
        let contents = Data([0x50, 0x4b, 0x03, 0x04, 1, 2, 3, 4])
        try contents.write(to: source)
        try InstallerImport.copy(from: source, to: destination)
        XCTAssertEqual(try Data(contentsOf: source), contents)
        XCTAssertEqual(try Data(contentsOf: destination), contents)
        try FileManager.default.removeItem(at: source)
        XCTAssertEqual(try Data(contentsOf: destination), contents)
    }

    func testImportRejectsEmptyFilesDirectoriesAndMissingFiles() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let empty = root.appendingPathComponent("empty.ipa")
        try Data().write(to: empty)
        for source in [empty, root, root.appendingPathComponent("missing.ipa")] {
            let destination = root.appendingPathComponent("staging/package.ipa")
            XCTAssertThrowsError(try InstallerImport.copy(from: source, to: destination))
            XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
        }
    }

    func testImportRejectsRemoteURLs() {
        XCTAssertThrowsError(try InstallerImport.copy(
            from: URL(string: "https://example.com/app.ipa")!,
            to: FileManager.default.temporaryDirectory.appendingPathComponent("package.ipa")
        ))
    }

    func testOnlyReadyPackagesAndInstallationFailuresCanInstall() {
        XCTAssertTrue(InstallerPhase.ready.canInstall)
        XCTAssertTrue(InstallerPhase.failed("Install failed", retryInstallation: true).canInstall)
        let phases: [InstallerPhase] = [
            .idle, .importing, .downloading(0, -1), .checking, .installing, .installed,
            .failed("Picker failed", retryInstallation: false),
        ]
        for phase in phases {
            XCTAssertFalse(phase.canInstall, "\(phase)")
        }
    }

    func testDirectHTTPSLink() throws {
        let url = try InstallerInput.downloadURL(" \nhttps://example.com/app.ipa?token=abc\n")
        XCTAssertEqual(url.absoluteString, "https://example.com/app.ipa?token=abc")
    }

    func testRejectsInsecureAndAmbiguousLinks() {
        for text in [
            "", "example.com/app.ipa", "http://example.com/app.ipa", "file:///private/var/app.ipa",
            "javascript:alert(1)", "https://", "https://user:password@example.com/app.ipa",
        ] {
            XCTAssertThrowsError(try InstallerInput.downloadURL(text), text)
        }
    }

    func testHTTPStatusValidation() throws {
        let url = URL(string: "https://example.com/app.ipa")!
        for code in [200, 201, 206] {
            try InstallerInput.checkResponse(
                HTTPURLResponse(url: url, statusCode: code, httpVersion: nil, headerFields: nil)
            )
        }
        for code in [301, 403, 404, 500] {
            XCTAssertThrowsError(
                try InstallerInput.checkResponse(
                    HTTPURLResponse(url: url, statusCode: code, httpVersion: nil, headerFields: nil)
                )
            )
        }
        XCTAssertThrowsError(try InstallerInput.checkResponse(nil))
    }

    func testRejectsOversizedResponses() {
        let response = HTTPURLResponse(
            url: URL(string: "https://example.com/app.ipa")!,
            statusCode: 200, httpVersion: nil,
            headerFields: ["Content-Length": String(InstallerInput.maximumDownloadSize + 1)]
        )
        XCTAssertThrowsError(try InstallerInput.checkResponse(response))
    }
}
