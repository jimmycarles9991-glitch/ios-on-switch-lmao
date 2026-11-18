import HWTestCore
import XCTest

final class TargetPeakTests: XCTestCase {
    func testTheKernelsLineReadsLargestFirst() {
        let peaks = TargetPeak.parse("backboardd=1440x1830x1;MercuryPosterExt=720x1280x4;Web Content=720x1280x1;")
        XCTAssertEqual(
            peaks,
            [
                TargetPeak(owner: "backboardd", width: 1440, height: 1830, samples: 1),
                TargetPeak(owner: "MercuryPosterExt", width: 720, height: 1280, samples: 4),
                TargetPeak(owner: "Web Content", width: 720, height: 1280, samples: 1),
            ])
        XCTAssertEqual(peaks[1].text, "MercuryPosterExt:720x1280x4")
        XCTAssertEqual(TargetPeak.parse(""), [])
        XCTAssertEqual(
            TargetPeak.parse("=1x1x1;bad=1x1;ok=2x3x1"), [TargetPeak(owner: "ok", width: 2, height: 3, samples: 1)])
    }

    func testPostersAndTheHostThatSnapshotsThem() {
        XCTAssertTrue(TargetPeak(owner: "MercuryPosterExt", width: 1, height: 1, samples: 1).isPoster)
        XCTAssertTrue(TargetPeak(owner: "PosterBoard", width: 1, height: 1, samples: 1).isPoster)
        XCTAssertTrue(TargetPeak(owner: "WeatherPoster", width: 1, height: 1, samples: 1).isPoster)
        XCTAssertFalse(TargetPeak(owner: "SpringBoard", width: 1, height: 1, samples: 1).isPoster)
    }

    func testAPosterAtThePanelFitsInEitherOrientationAndOneTwiceItDoesNot() {
        let panel = (width: 720, height: 1280)
        XCTAssertTrue(
            TargetPeak(owner: "p", width: 720, height: 1280, samples: 4).fits(width: panel.width, height: panel.height))
        XCTAssertTrue(
            TargetPeak(owner: "p", width: 1280, height: 720, samples: 1).fits(width: panel.width, height: panel.height))
        XCTAssertFalse(
            TargetPeak(owner: "p", width: 1440, height: 2560, samples: 1).fits(width: panel.width, height: panel.height)
        )
        XCTAssertFalse(
            TargetPeak(owner: "p", width: 1600, height: 100, samples: 1).fits(width: panel.width, height: panel.height))
    }
}
