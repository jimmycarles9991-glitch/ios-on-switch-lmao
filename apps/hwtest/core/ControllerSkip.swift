import Foundation

/// Why input.controllers and input.joycon found no controller after asking for a press. A Joy-Con on a running rail is
/// a game controller only from a gamepad press or stick push until 900 s idle (drivers/SwitchJoyCon/SwitchJoyConUse.h,
/// as iPadOS keeps a Bluetooth gamepad).
public enum ControllerSkip {
    /// `rails`: the letters of the rails running a Joy-Con (kern.switch_joycon); nil where unreadable.
    public static func reason(railsRunning rails: [String]?) -> String {
        guard let rails, !rails.isEmpty else { return "no Joy-Con on a rail and no other game controller connected" }
        return "the Joy-Cons on rails \(rails.sorted().joined(separator: ",")) publish a game controller only after "
            + "a gamepad press or stick push (SwitchJoyConUse.h), and none came before the prompt ended"
    }
}
