import Foundation

/// display.media_completion_flip with no set armed: a skip names the counter that shows why, and only when that
/// counter moved. No video set at all on windows B and C (every counter 0) is a failure of the path under test, not a
/// reason to skip it.
public enum MediaFlipSkip {
    public enum Judgement: Equatable {
        case judge
        case skip(String)
        case fail(String)
    }

    public static func judge(armed: Int, refused: Int, doneAtSwap: Int, fenced: Int, declined: Int) -> Judgement {
        guard armed == 0 && refused == 0 else { return .judge }
        if fenced > 0 && declined >= fenced {
            return .skip("the video sets that waited for their producers were declined (d_declined=\(declined))")
        }
        if fenced == 0 && doneAtSwap > 0 {
            return .skip("every video set's producers were done at its swap (d_done_at_swap=\(doneAtSwap))")
        }
        if fenced > 0 {
            return .fail("\(fenced) video sets waited for their producers and none was armed or declined")
        }
        return .fail("no video set reached windows B and C during the clip: every counter stayed 0")
    }
}
