import Foundation
import Metal

/// What a device answers about function pointers: 1 YES, 0 NO, -1 where it has no such method
public enum FunctionPointerAnswers {
    public static let selectors = [
        ("function_pointers", "supportsFunctionPointers"),
        ("function_pointers_from_render", "supportsFunctionPointersFromRender"),
        ("function_pointers_from_mesh", "supportsFunctionPointersFromMesh"),
    ]

    public static func answers(_ device: MTLDevice) -> [(key: String, answer: Int)] {
        let object = device as AnyObject as! NSObject
        return selectors.map { key, name in
            let sel = NSSelectorFromString(name)
            guard object.responds(to: sel) else { return (key, -1) }
            typealias Flag = @convention(c) (NSObject, Selector) -> Bool
            return (key, unsafeBitCast(object.method(for: sel), to: Flag.self)(object, sel) ? 1 : 0)
        }
    }
}
