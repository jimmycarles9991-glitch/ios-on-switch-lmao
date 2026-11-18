import Foundation
import SwitchInstallerInput
import XCTest

final class SwitchDownloadTests: XCTestCase {
    private var directory: URL!
    private var server: Process!
    private var baseURL: URL!

    override func setUpWithError() throws {
        directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let output = Pipe()
        server = Process()
        server
            .executableURL = URL(fileURLWithPath: try XCTUnwrap(ProcessInfo.processInfo.environment["DOWNLOAD_SERVER"]))
        server.standardOutput = output
        try server.run()
        let address = try XCTUnwrap(String(data: output.fileHandleForReading.availableData, encoding: .utf8))
        baseURL = try XCTUnwrap(URL(string: address.trimmingCharacters(in: .whitespacesAndNewlines)))
    }

    override func tearDownWithError() throws {
        if server?.isRunning == true {
            server.terminate()
            server.waitUntilExit()
        }
        if let directory, FileManager.default.fileExists(atPath: directory.path) {
            try FileManager.default.removeItem(at: directory)
        }
    }

    private var destination: URL {
        directory.appendingPathComponent("staging/package.ipa")
    }

    private func download(_ path: String, cancelOnProgress: Bool = false) throws -> Result<URL, Error> {
        let finished = expectation(description: "Download completion")
        finished.assertForOverFulfill = true
        let transfer = SwitchInstallerDownload(destination: destination)
        var outcome: Result<URL, Error>?
        transfer.start(
            url: baseURL.appendingPathComponent(path),
            progress: { _, _ in
                XCTAssertTrue(Thread.isMainThread)
                if cancelOnProgress { transfer.cancel() }
            },
            completion: { result in
                XCTAssertTrue(Thread.isMainThread)
                outcome = result
                finished.fulfill()
            }
        )
        waitForExpectations(timeout: 10)
        return try XCTUnwrap(outcome)
    }

    private func assertFailure(_ path: String, contains message: String) throws {
        switch try download(path) {
        case .success:
            XCTFail("Unexpected successful download")
        case .failure(let error):
            XCTAssertTrue(error.localizedDescription.contains(message), "\(error)")
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
    }

    func testDownloadStagesIndependentBytesBeforeURLSessionDeletesItsTemporaryFile() throws {
        let staged = try download("ok").get()
        let expected = Data([0x50, 0x4b, 0x03, 0x04]) + Data((0 ..< 16384).map { UInt8($0 % 256) })
        XCTAssertEqual(staged, destination)
        XCTAssertEqual(try Data(contentsOf: staged), expected)
    }

    func testDownloadAcceptsUnknownContentLength() throws {
        let staged = try download("unknown-size").get()
        XCTAssertEqual(try Data(contentsOf: staged).count, 16388)
    }

    func testDownloadPreservesHTTPError() throws {
        try assertFailure("error", contains: "HTTP 404")
    }

    func testDownloadRejectsAnEmptyResponse() throws {
        try assertFailure("empty", contains: "empty")
    }

    func testDownloadRejectsAnOversizedResponse() throws {
        try assertFailure("oversized", contains: "8 GB")
    }

    func testTruncatedResponsePreservesTheNetworkError() throws {
        switch try download("truncated") {
        case .success:
            XCTFail("Incomplete download succeeded")
        case .failure(let error):
            XCTAssertEqual((error as NSError).domain, NSURLErrorDomain)
            XCTAssertEqual((error as NSError).code, NSURLErrorNetworkConnectionLost)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
    }

    func testDownloadRefusesAnInsecureRedirect() throws {
        try assertFailure("redirect", contains: "insecure")
    }

    func testCancellationCompletesWithoutStagingAnIncompleteArchive() throws {
        switch try download("hold", cancelOnProgress: true) {
        case .success:
            XCTFail("Cancelled download succeeded")
        case .failure(let error):
            XCTAssertEqual((error as NSError).domain, NSURLErrorDomain)
            XCTAssertEqual((error as NSError).code, NSURLErrorCancelled)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
    }
}
