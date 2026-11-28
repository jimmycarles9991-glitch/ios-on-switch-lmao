import Foundation

/// gpu.path.commit_latency's bounds on how many of a shape's encoders the submit worker encoded before the commit, as
/// means over its measured frames
public enum EncodeAheadBound {
    /// Left to the commit in a shape without an event: the encoders recorded since the worker was last handed the
    /// buffer (24 tape commands, 3 passes) and the two it holds for its look-ahead
    public static let commitShare = 5.0
    /// The encode ahead stops at an event, so a shape with one encodes exactly the encoders before it. One frame of
    /// sixteen whose worker had not run by the commit takes the mean half an encoder under.
    public static let eventShortfall = 1.0

    /// Why the shape's encode ahead is out of bounds, nil where it is in them; `eventAfter`: the encoders before its
    /// event
    public static func failure(
        _ shape: String, pipelined: Double, encoders: Double, passes: Int, eventAfter: Int?
    ) -> String? {
        guard let event = eventAfter.map(Double.init) else {
            guard passes > 3 && pipelined < encoders - commitShare else { return nil }
            return "\(shape): \(pipelined) of \(encoders) encoders encoded ahead"
        }
        if pipelined > event {
            return "\(shape): \(pipelined) encoders encoded ahead past the event after encoder \(Int(event))"
        }
        if pipelined < event - eventShortfall {
            return "\(shape): \(pipelined) of the \(Int(event)) encoders before the event encoded ahead"
        }
        return nil
    }
}
