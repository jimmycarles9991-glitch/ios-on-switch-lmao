import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface

/// The texture peer (peer/texpeer.mm) opens IOSurfaceRootUserClient: 24B119's system policy denies it to a process
/// whose signature does not name the class (`System Policy: switch-texpeer deny(1) iokit-open-user-client
/// IOSurfaceRootUserClient`), and then every IOSurface the peer makes fails. `peer_classes`: the user client classes the
/// staged peer is signed for (`unreadable` where the app's sandbox keeps it from the file); `peer_make`: the peer's
/// answer to making a 16x16 surface. FAIL: a readable signature that leaves IOSurfaceRootUserClient unnamed, or no
/// surface made.
struct PeerUserClientTest: HWTest {
    let id = "gpu.iosurface.peer_user_client"
    let name = "The texture peer is signed for IOSurfaceRootUserClient and makes an IOSurface"
    let subsystem = Subsystem.metal

    static let peer = "/usr/bin/switch-texpeer"
    static let surfaceClass = "IOSurfaceRootUserClient"

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        let image = try? Data(contentsOf: URL(fileURLWithPath: Self.peer), options: .alwaysMapped)
        let classes = image.flatMap(EmbeddedEntitlements.of).map(EmbeddedEntitlements.userClientClasses)
        metrics.set("peer_classes", classes.map { $0.isEmpty ? "-" : $0.joined(separator: ",") } ?? "unreadable")
        var made = false
        do {
            let connection = try await TexturePeerLink.connection()
            let surface = HandlerValue<IOSurface?>(nil)
            let answer = await SurfaceCrossProcessTest.ask(connection) { peer, reply in
                peer.makeSurfaceWidth(16, height: 16, keep: false) {
                    surface.value = $0
                    reply($1)
                }
            }
            made = answer?.hasPrefix("ok ") == true && surface.value != nil
            let word = answer?.split(separator: " ").first.map { $0.replacingOccurrences(of: "=", with: ":") }
            metrics.set("peer_make", made ? "ok" : word ?? "no_answer")
        } catch {
            metrics.set("peer_make", "no_peer:\(error)")
        }
        let once = await TexturePeerLink.startedOnce(&metrics)
        return .judged(
            [
                once,
                ("the peer is signed for \(Self.surfaceClass)", classes?.contains(Self.surfaceClass) ?? true),
                ("the peer made an IOSurface", made),
            ], metrics, proof: ""
        )
    }
}
