import Foundation
import HWTestCore
import XCTest

/// The texture peer's entitlements (peer/texpeer.entitlements) against what gpu.iosurface.peer_user_client asks of the
/// staged binary's signature.
final class PeerEntitlementsTests: XCTestCase {
    func testThePeerIsEntitledToOpenIOSurfaceRootUserClient() throws {
        let environment = ProcessInfo.processInfo.environment
        let path = try XCTUnwrap(environment["TEXPEER_ENTITLEMENTS"])
        let root = environment["TEST_SRCDIR"].map { $0 + "/" } ?? ""
        let data = try Data(contentsOf: URL(fileURLWithPath: root + path))
        let entitlements = try XCTUnwrap(
            PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
        )
        XCTAssertTrue(EmbeddedEntitlements.userClientClasses(entitlements).contains("IOSurfaceRootUserClient"))
    }

    func testOneClassNamedAloneIsRead() {
        XCTAssertEqual(
            EmbeddedEntitlements.userClientClasses([EmbeddedEntitlements.userClientKey: "IOSurfaceRootUserClient"]),
            ["IOSurfaceRootUserClient"]
        )
        XCTAssertEqual(EmbeddedEntitlements.userClientClasses([:]), [])
    }
}
