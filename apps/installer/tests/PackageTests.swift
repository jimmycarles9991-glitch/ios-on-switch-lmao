import Foundation
import SwitchInstallerNative
import XCTest

final class SwitchPackageTests: XCTestCase {
    private func fixture(_ name: String) -> URL {
        let environment = ProcessInfo.processInfo.environment
        let root = URL(fileURLWithPath: environment["TEST_SRCDIR"]!)
            .appendingPathComponent(environment["FIXTURE"]!).deletingLastPathComponent()
        return root.appendingPathComponent(name + ".ipa")
    }

    func testValidDeviceBuilds() throws {
        for name in ["valid", "arm64e", "fat", "fat64", "nested_bundles", "internal_link"] {
            let package = try SwitchPackage.inspectURL(fixture(name), systemVersion: "17.7.11")
            XCTAssertEqual(package.name, "Example App", name)
            XCTAssertEqual(package.identifier, "org.example.app", name)
            XCTAssertEqual(package.version, "1.2.3", name)
            XCTAssertGreaterThan(package.expandedSize, 0, name)
        }
    }

    private func rejects(_ name: String) {
        XCTAssertThrowsError(try SwitchPackage.inspectURL(fixture(name), systemVersion: "27.2"), name)
    }

    func testRejectsOverlappingFatSlice() { rejects("fat_overlap") }
    func testRejectsEncryptedApp() { rejects("encrypted") }
    func testRejectsSimulatorApp() { rejects("simulator") }
    func testRejectsX86App() { rejects("x86") }
    func testRejectsMissingExecutable() { rejects("missing_binary") }
    func testRejectsMissingPlist() { rejects("missing_plist") }
    func testRejectsUnsafePath() { rejects("unsafe_path") }
    func testRejectsEscapingSymlink() { rejects("escaping_link") }
    func testRejectsCorruptPlist() { rejects("corrupt_plist") }
    func testRejectsDuplicatePath() { rejects("duplicate") }
    func testRejectsAppleIdentifier() { rejects("apple_id") }
    func testRejectsInstallerReplacement() { rejects("self_id") }
    func testRejectsFutureOS() { rejects("future_os") }
    func testRejectsOversizedMetadata() { rejects("large_info") }
    func testRejectsInvalidLoadCommand() { rejects("bad_commands") }
    func testRejectsMultipleApps() { rejects("multiple_apps") }
    func testRejectsInvalidVersion() { rejects("invalid_version") }
    func testRejectsUppercaseInstallerReplacement() { rejects("self_id_uppercase") }

    func testChecksDeploymentVersionNumerically() {
        XCTAssertThrowsError(try SwitchPackage.inspectURL(fixture("valid"), systemVersion: "16.9"))
        XCTAssertNoThrow(try SwitchPackage.inspectURL(fixture("valid"), systemVersion: "17.0"))
        XCTAssertNoThrow(try SwitchPackage.inspectURL(fixture("valid"), systemVersion: "27.2"))
    }

    func testRejectsMissingFileAndNonFileURL() {
        XCTAssertThrowsError(try SwitchPackage.inspectURL(fixture("absent"), systemVersion: "27.2"))
        XCTAssertThrowsError(
            try SwitchPackage.inspectURL(URL(string: "https://example.com/app.ipa")!, systemVersion: "27.2")
        )
    }

    func testInstallationIsUnavailableOnTheHost() {
        XCTAssertFalse(SwitchInstallation.isInstalled("org.example.app"))
        let url = fixture("valid")
        XCTAssertThrowsError(try SwitchInstallation.installURL(url, identifier: "org.example.app") { _, _ in
            XCTFail("An unavailable installation should not report progress")
        })
    }
}
