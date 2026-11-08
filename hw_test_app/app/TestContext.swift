import Foundation
import SwiftUI
import UIKit

/// A one-shot value a test waits for: delivered by an observer (a route change, a button press), by the prompt's
/// Skip button (nil) or by the timeout (nil), whichever comes first. A value delivered before `wait` is kept.
final class Waiter<T>: @unchecked Sendable {
    private enum State {
        case idle
        case waiting(CheckedContinuation<T?, Never>)
        case done(T?)
    }

    private let lock = NSLock()
    private var state = State.idle

    func resume(_ value: T?) {
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

    func wait(timeout: TimeInterval) async -> T? {
        let timer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(timeout * 1e9))
            self?.resume(nil)
        }
        defer { timer.cancel() }
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

final class TestContext: @unchecked Sendable {
    let media: URL
    let documents: URL
    let prompter: Prompter
    let stage: Stage

    init(media: URL, documents: URL, prompter: Prompter, stage: Stage) {
        self.media = media
        self.documents = documents
        self.prompter = prompter
        self.stage = stage
    }

    /// Shows `message` with a Skip button and waits for what `setup`'s observers deliver; nil when skipped or timed
    /// out.
    func interact<T>(
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

    func onStage<R>(fullScreen: Bool = false, _ build: @escaping @MainActor (UIView) -> R) async -> R {
        await MainActor.run {
            var result: R?
            stage.present(fullScreen: fullScreen) { result = build($0) }
            return result!
        }
    }

    func clearStage() async {
        await MainActor.run { stage.dismiss() }
    }
}
