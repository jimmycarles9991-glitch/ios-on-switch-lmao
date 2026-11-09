import Foundation
import SwiftUI
import UIKit

/// A one-shot value a test waits for: delivered by an observer (a route change, a button press), by the prompt's
/// Skip button (nil) or by the timeout (nil), whichever comes first. A value delivered before `wait` is kept.
public final class Waiter<T>: @unchecked Sendable {
    private enum State {
        case idle
        case waiting(CheckedContinuation<T?, Never>)
        case done(T?)
    }

    private let lock = NSLock()
    private var state = State.idle

    public init() {}

    public func resume(_ value: T?) {
        lock.lock()
        switch state {
        case .idle:
            state = .done(value)
            lock.unlock()
        case .waiting(let continuation):
            state = .done(value)
            lock.unlock()
            continuation.resume(returning: value)
        case .done:
            lock.unlock()
        }
    }

    /// The timeout fires from a dispatch queue, not a task: a hung test can hold the cooperative pool's few threads
    /// (a waitUntilCompleted on a wedged GPU), and the timeout is what ends the wait for it.
    public func wait(timeout: TimeInterval) async -> T? {
        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + timeout) { [weak self] in
            self?.resume(nil)
        }
        return await withCheckedContinuation { continuation in
            lock.lock()
            if case .done(let value) = state {
                lock.unlock()
                continuation.resume(returning: value)
            } else {
                state = .waiting(continuation)
                lock.unlock()
            }
        }
    }
}

/// The prompt card interactive tests show; Skip resolves the test's waiter with nil.
@MainActor
final class Prompter: ObservableObject {
    @Published private(set) var message: String?
    private var skip: (() -> Void)?

    func show(_ message: String, skip: @escaping () -> Void) {
        self.message = message
        self.skip = skip
    }

    func hide() {
        message = nil
        skip = nil
    }

    func skipTapped() {
        skip?()
        hide()
    }
}

/// The area tests draw into (a video layer, a colour swatch, a touch pad); full screen when a test needs the
/// whole panel, such as frame pacing.
@MainActor
final class Stage: ObservableObject {
    @Published private(set) var visible = false
    @Published private(set) var fullScreen = false
    let container = UIView()

    func present(fullScreen: Bool = false, _ build: (UIView) -> Void) {
        container.subviews.forEach { $0.removeFromSuperview() }
        container.layer.sublayers?.forEach { $0.removeFromSuperlayer() }
        container.backgroundColor = .black
        build(container)
        self.fullScreen = fullScreen
        visible = true
    }

    func dismiss() {
        visible = false
        fullScreen = false
        container.subviews.forEach { $0.removeFromSuperview() }
        container.layer.sublayers?.forEach { $0.removeFromSuperlayer() }
    }
}

public final class TestContext: @unchecked Sendable {
    public let media: URL
    public let documents: URL
    /// The iOS major version the app is built for.
    public let ios: Int
    let prompter: Prompter
    let stage: Stage

    init(media: URL, documents: URL, ios: Int, prompter: Prompter, stage: Stage) {
        self.media = media
        self.documents = documents
        self.ios = ios
        self.prompter = prompter
        self.stage = stage
    }

    /// Shows `message` with a Skip button and waits for what `setup`'s observers deliver; nil when skipped or timed
    /// out.
    public func interact<T>(
        _ message: String, timeout: TimeInterval = 45, _ setup: @escaping @MainActor (Waiter<T>) -> Void
    ) async -> T? {
        let waiter = Waiter<T>()
        await MainActor.run {
            prompter.show(message) { waiter.resume(nil) }
            setup(waiter)
        }
        let value = await waiter.wait(timeout: timeout)
        await MainActor.run { prompter.hide() }
        return value
    }

    public func onStage<R>(fullScreen: Bool = false, _ build: @escaping @MainActor (UIView) -> R) async -> R {
        await MainActor.run {
            var result: R?
            stage.present(fullScreen: fullScreen) { result = build($0) }
            return result!
        }
    }

    public func clearStage() async {
        await MainActor.run { stage.dismiss() }
    }
}
