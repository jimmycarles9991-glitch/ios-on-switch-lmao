import Foundation
import HWTestCore
import XCTest

final class PeerLinkTests: XCTestCase {
    /// launchd's start of an on-demand job: never sooner than its throttle interval after the last start (boot 2's
    /// texture peer: started at 23:57:24.978, 35.886 and 46.322 for tests 1.4 s and 10 s apart).
    final class Launchd: @unchecked Sendable {
        let throttle = 10.0
        var now = 0.0
        var waited = 0.0
        var starts: [Double] = []

        func start() -> Peer {
            let at = max(now, (starts.last ?? -.infinity) + throttle)
            waited += at - now
            now = at
            starts.append(at)
            return Peer()
        }
    }

    final class Peer: Sendable {}

    func testTheRunsPeerTestsShareOneStartSoNoneWaitsOutTheThrottle() async throws {
        let launchd = Launchd()
        let link = PeerLink { launchd.start() }
        var peers: [Peer] = []
        for at in [0.0, 1.4, 11.9, 620.0] {
            launchd.now = max(launchd.now, at)
            peers.append(try await link.connected())
        }
        XCTAssertEqual(launchd.starts, [0])
        XCTAssertEqual(launchd.waited, 0)
        XCTAssertTrue(peers.allSatisfy { $0 === peers[0] })
        let starts = await link.starts
        XCTAssertEqual(starts, 1)
    }

    func testALostPeerIsStartedAgainForTheNextTestAndAnOlderLossChangesNothing() async throws {
        let launchd = Launchd()
        let link = PeerLink { launchd.start() }
        let first = try await link.connected()
        await link.lost(first)
        let second = try await link.connected()
        await link.lost(first)
        let third = try await link.connected()
        XCTAssertFalse(first === second)
        XCTAssertTrue(second === third)
        XCTAssertEqual(launchd.starts.count, 2)
    }

    func testTestsAskingWhileThePeerStartsAllGetTheOneStart() async throws {
        let launchd = Launchd()
        let link = PeerLink { () async -> Peer in
            await Task.yield()
            return launchd.start()
        }
        async let a = link.connected()
        async let b = link.connected()
        let one = try await a
        let two = try await b
        XCTAssertTrue(one === two)
        XCTAssertEqual(launchd.starts.count, 1)
    }
}
