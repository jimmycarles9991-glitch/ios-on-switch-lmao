import HWTestCore
import XCTest

/// gpu.gm20b.fetch_*'s route checks on the counters boot 1 measured: its BGR5A1Unorm states, which have a storage form,
/// moved fetch_interlocked one a draw and neither fetch_sequential nor fetch_triangles (fetch_list: 8 triangles in one
/// draw; fetch_lines: two draws).
final class FetchRouteTests: XCTestCase {
    let boot1List = ["fetch_interlocked": 1, "fetch_sequential": 0, "fetch_triangles": 0]
    let boot1Lines = ["fetch_interlocked": 2, "fetch_sequential": 0, "fetch_triangles": 0]

    func failed(_ checks: [(String, Bool)]) -> [String] { checks.filter { !$0.1 }.map(\.0) }

    func testBoot1sStorageFormDrawsAreOneInterlockedCallEach() {
        XCTAssertEqual(failed(FetchRoute.interlock.checks(calls: 1, triangles: 8, moved: boot1List)), [])
        XCTAssertEqual(failed(FetchRoute.interlock.checks(calls: 2, triangles: 16, moved: boot1Lines)), [])
    }

    func testAStorageFormDrawSplitOnTheCpuFails() {
        let split = ["fetch_interlocked": 0, "fetch_sequential": 8, "fetch_triangles": 8]
        XCTAssertEqual(
            failed(FetchRoute.interlock.checks(calls: 1, triangles: 8, moved: split)),
            [
                "interlock: fetch_interlocked rose by 0, want 1", "interlock: fetch_sequential rose by 8, want 0",
                "interlock: fetch_triangles rose by 8, want 0",
            ]
        )
    }

    func testTheMacroRouteIsOneCallADrawOfItsTriangles() {
        let strip = ["fetch_sequential": 1, "fetch_triangles": 12]
        XCTAssertEqual(failed(FetchRoute.macro.checks(calls: 1, triangles: 12, moved: strip)), [])
        XCTAssertEqual(
            failed(FetchRoute.macro.checks(calls: 1, triangles: 13, moved: strip)),
            ["macro: fetch_triangles rose by 12, want 13"]
        )
    }

    func testBoot1sCountsAreNotTheMacros() {
        XCTAssertEqual(
            failed(FetchRoute.macro.checks(calls: 1, triangles: 8, moved: boot1List)),
            [
                "macro: fetch_interlocked rose by 1, want 0", "macro: fetch_sequential rose by 0, want 1",
                "macro: fetch_triangles rose by 0, want 8",
            ]
        )
    }
}
