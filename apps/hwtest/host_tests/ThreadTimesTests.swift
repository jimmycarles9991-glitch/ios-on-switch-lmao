import Darwin
import Foundation
import HWTestCore
import XCTest

final class ThreadTimesTests: XCTestCase {
    func times(_ threads: [UInt64: (String, Double)], exitedMs: Double) -> ThreadTimes {
        ThreadTimes(
            threads: threads.mapValues { ThreadTimes.Thread(name: $0.0, nanoseconds: UInt64($0.1 * 1e6)) },
            exitedNanoseconds: UInt64(exitedMs * 1e6), main: 1
        )
    }

    func testEachThreadsTimeSinceTheEarlierSampleLandsInItsClassAndAnExitedThreadCountsOnce() {
        let earlier = times(
            [
                1: ("", 10), 2: ("com.brunostjohn.switch.gm20b.submit", 5), 3: ("", 20),
                4: ("com.apple.uikit.eventfetch-thread", 1), 5: ("", 7),
            ],
            exitedMs: 50
        )
        let later = times(
            [
                1: ("", 12), 2: ("com.brunostjohn.switch.gm20b.submit", 9), 3: ("", 70),
                4: ("com.apple.uikit.eventfetch-thread", 1.5), 6: ("", 3),
            ],
            exitedMs: 59
        )
        let split = later.split(since: earlier)
        XCTAssertEqual(split.main, 2, accuracy: 1e-9)
        XCTAssertEqual(split.driver, 4, accuracy: 1e-9)
        XCTAssertEqual(split.pool, 53, accuracy: 1e-9, "a thread born in between counts whole")
        XCTAssertEqual(split.other, 2.5, accuracy: 1e-9, "thread 5's 2 ms after the earlier sample, not its 9 ms")
        XCTAssertEqual(split.field, "pool:53.0,driver:4.0,main:2.0,other:2.5")
    }

    func testALiveSampleFindsANamedDriverThreadsAndAnUnnamedThreadsWork() {
        let burned = DispatchSemaphore(value: 0), release = DispatchSemaphore(value: 0)
        let main = ThreadTimes.current()
        let start = ThreadTimes.sample(main: main)
        for name in ["com.brunostjohn.switch.gm20b.host-test", ""] {
            Thread {
                if !name.isEmpty { pthread_setname_np(name) }
                Self.burn(milliseconds: 40)
                burned.signal()
                release.wait()
            }.start()
        }
        burned.wait()
        burned.wait()
        let busy = ThreadTimes.sample(main: main)
        release.signal()
        release.signal()
        let split = busy.split(since: start)
        XCTAssertGreaterThanOrEqual(split.driver, 39, split.field)
        XCTAssertLessThan(split.driver, 400, split.field)
        XCTAssertGreaterThanOrEqual(split.pool, 39, split.field)
    }

    static func burn(milliseconds: Double) {
        let begun = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)
        while Double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - begun) < milliseconds * 1e6 {}
    }
}
