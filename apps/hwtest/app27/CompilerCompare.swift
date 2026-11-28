import CryptoKit
import Foundation
import HWTestCore
import HWTestKit
import Metal
import SwitchCompilerBench

struct CompilerCompare: HWTest {
    let key: String
    let title: String
    let digest: String
    let subsystem = Subsystem.metal
    let timeout: TimeInterval = 900
    var id: String { "gpu.compiler_ab." + key }
    var name: String { "Compiler A/B · " + title }

    static let cases = [
        Self(
            key: "3ef3a99e4f382ce1",
            title: "Compositor XR",
            digest: "8ef21cd826c1c9de85cd070dc75388bce2c03435ac29ef61aa0a249a916e8699"
        ),
        Self(
            key: "ae6d01e7cac0d964",
            title: "Hillshade",
            digest: "7c9986b9d5072f3e7efc298f3b9c82135aad6119bd66d9325f4e0e7ede9a45d4"
        ),
        Self(
            key: "17d6ade58fef2152",
            title: "RenderBox",
            digest: "07febb8e92e01208f2fe9ac0de8132fcc2deee820ab9bb2141954b0769177076"
        ),
        Self(
            key: "357f8355b68acebd",
            title: "GlowAlpha",
            digest: "9b2e5d6bf01b48cbcca2fbbfdb94fd2e8d278092025e20ef024a21f158dc84f2"
        ),
        Self(
            key: "3f9f7f04b2b91507",
            title: "PatternedRibbon",
            digest: "d0def780090bb4f89573443e3c4df3b09e1a7aac7893f0c92562cc17baee1855"
        ),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let url = URL(fileURLWithPath: "/private/var/mobile/Media/Switch/benchloop/compiler/\(key).swab")
        let bytes: Data
        do {
            bytes = try Data(contentsOf: url)
        } catch let error as CocoaError where error.code == .fileReadNoSuchFile {
            return .skip("not staged")
        }
        guard SHA256.hash(data: bytes).map({ String(format: "%02x", $0) }).joined() == digest else {
            return .fail("Staged payload differs from this build's compiler comparison")
        }
        _ = MTLCreateSystemDefaultDevice()
        var message = [CChar](repeating: 0, count: 4096)
        let ok = bytes.withUnsafeBytes {
            switch_compiler_compare(key, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, &message, message.count)
        }
        let detail = String(cString: message)
        return ok != 0 ? .pass(detail: detail) : .fail(detail)
    }
}
