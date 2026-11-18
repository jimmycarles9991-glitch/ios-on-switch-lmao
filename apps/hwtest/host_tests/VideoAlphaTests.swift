import Foundation
import HWTestCore
import XCTest

/// ui.video_alpha names the model a composite fits only if its grid tells every wrong model from the right one by more
/// than twice the test's 6-level tolerance.
final class VideoAlphaTests: XCTestCase {
    func testCodesAreBT709VideoRange() {
        XCTAssertEqual(
            VideoAlpha.encode(SIMD3(1, 1, 1), alpha: 255), VideoAlpha.Codes(y: 235, cb: 128, cr: 128, a: 255))
        XCTAssertEqual(VideoAlpha.encode(SIMD3(0, 0, 0), alpha: 0), VideoAlpha.Codes(y: 16, cb: 128, cr: 128, a: 0))
        XCTAssertEqual(
            VideoAlpha.encode(SIMD3(0.75, 0.25, 0.2), alpha: 64), VideoAlpha.Codes(y: 93, cb: 110, cr: 185, a: 64))
    }

    func testTheGridSeparatesEveryWrongModel() {
        for mode in [VideoAlpha.Mode.straight, .premultiplied] {
            let codes = VideoAlpha.grid(mode).flatMap { $0 }
            for space in VideoAlpha.Space.allCases {
                let expected = VideoAlpha.Model(mode, .plane2, .video, space)
                for model in VideoAlpha.Model.all where model != expected {
                    let separation = codes.map { code -> Float in
                        let d =
                            VideoAlpha.composite(code, over: VideoAlpha.background, model)
                            - VideoAlpha.composite(code, over: VideoAlpha.background, expected)
                        return max(abs(d.x), abs(d.y), abs(d.z))
                    }.max()!
                    XCTAssertGreaterThan(separation * 255, 12, "\(model.name) against \(expected.name)")
                }
            }
        }
    }
}
