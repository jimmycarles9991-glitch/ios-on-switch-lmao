import Foundation
import HWTestCore
import XCTest

final class AppFilesTests: XCTestCase {
    var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    func names(_ directory: String) -> [String] {
        (try? FileManager.default.contentsOfDirectory(atPath: root.appendingPathComponent(directory).path)) ?? []
    }

    func testAPlacedFileArrivesWholeUnderItsNameAndLeavesNothingBehind() throws {
        let target = root.appendingPathComponent("Documents/gpu/scene/got.png")
        let staging = root.appendingPathComponent("Library/hwtest/staging")
        try AppFiles.place(Data("first".utf8), at: target, staging: staging)
        try AppFiles.place(Data("second picture".utf8), at: target, staging: staging)
        XCTAssertEqual(try Data(contentsOf: target), Data("second picture".utf8))
        XCTAssertEqual(names("Documents/gpu/scene"), ["got.png"])
        XCTAssertEqual(names("Library/hwtest/staging"), [])
    }

    func testADurableWriteStagedElsewhereLeavesItsFolderHoldingOnlyItsTarget() throws {
        let target = root.appendingPathComponent("Documents/benchloop/status.json")
        let folder = target.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let staging = root.appendingPathComponent("Library/hwtest/staging")
        try DurableFile.write(Data("{}\n".utf8), to: target, stagingIn: staging)
        XCTAssertEqual(try Data(contentsOf: target), Data("{}\n".utf8))
        XCTAssertEqual(names("Documents/benchloop"), ["status.json"])
        XCTAssertEqual(names("Library/hwtest/staging"), [])
    }

    func testAPlaceThatCannotRenameRemovesWhatItStaged() throws {
        let staging = root.appendingPathComponent("staging")
        let target = root.appendingPathComponent("Documents/taken")
        try FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
        try Data().write(to: target.appendingPathComponent("inside"))
        XCTAssertThrowsError(try AppFiles.place(Data("x".utf8), at: target, staging: staging))
        XCTAssertEqual(names("staging"), [])
    }
}
