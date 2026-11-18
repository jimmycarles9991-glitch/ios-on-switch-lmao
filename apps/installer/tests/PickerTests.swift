import UIKit
import XCTest

final class SwitchPickerTests: XCTestCase {
    @MainActor
    func testPickerRequestsAnImportedCopy() {
        let picker = InstallerFilePicker.controller()
        XCTAssertEqual(picker.documentPickerMode, .import)
        XCTAssertFalse(picker.allowsMultipleSelection)
    }

    @MainActor
    func testSelectedCopyIsDeliveredOnce() throws {
        let url = URL(fileURLWithPath: "/private/var/mobile/imported.ipa")
        var results: [Result<URL?, Error>] = []
        let picker = InstallerFilePicker.controller()
        let coordinator = InstallerFilePicker.Coordinator { results.append($0) }
        coordinator.documentPicker(picker, didPickDocumentsAt: [url])
        coordinator.documentPicker(picker, didPickDocumentsAt: [url])
        coordinator.documentPickerWasCancelled(picker)
        XCTAssertEqual(results.count, 1)
        XCTAssertEqual(try results[0].get(), url)
    }

    @MainActor
    func testCancellationDoesNotImportOrReportAnError() throws {
        var results: [Result<URL?, Error>] = []
        let picker = InstallerFilePicker.controller()
        let coordinator = InstallerFilePicker.Coordinator { results.append($0) }
        coordinator.documentPickerWasCancelled(picker)
        XCTAssertEqual(results.count, 1)
        XCTAssertNil(try results[0].get())
    }

    @MainActor
    func testEmptyOrMultipleSelectionsAreRejected() {
        let url = URL(fileURLWithPath: "/private/var/mobile/imported.ipa")
        for urls in [[], [url, url]] {
            var results: [Result<URL?, Error>] = []
            let picker = InstallerFilePicker.controller()
            let coordinator = InstallerFilePicker.Coordinator { results.append($0) }
            coordinator.documentPicker(picker, didPickDocumentsAt: urls)
            XCTAssertEqual(results.count, 1)
            XCTAssertThrowsError(try results[0].get())
        }
    }
}
