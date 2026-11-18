import Combine
import SwiftUI
import SwitchInstallerInput
import UIKit
import XCTest

@MainActor
final class SwitchNavigationTests: XCTestCase {
    private var observations = Set<AnyCancellable>()

    func testBackFromReviewDiscardsOnlyTheStagedCopy() async throws {
        let model = SwitchInstallerModel()
        let source = try fixture()
        await prepare(model, source: source)
        let staged = try XCTUnwrap(model.package?.url)
        XCTAssertNotEqual(staged, source)
        XCTAssertTrue(FileManager.default.fileExists(atPath: staged.path))
        model.dismissReview()
        XCTAssertEqual(model.phase, .idle)
        XCTAssertNil(model.package)
        XCTAssertFalse(FileManager.default.fileExists(atPath: staged.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: source.path))
    }

    func testBackDuringPreparationCancelsThePendingReview() async throws {
        let model = SwitchInstallerModel()
        let finished = expectation(description: "Preparation completed")
        model.$phase.filter { $0 == .checking || $0 == .ready }.first().sink { _ in
            finished.fulfill()
        }.store(in: &observations)
        model.importFile(try fixture())
        XCTAssertEqual(model.phase, .importing)
        model.dismissReview()
        XCTAssertEqual(model.phase, .idle)
        XCTAssertNil(model.package)
        finished.isInverted = true
        await fulfillment(of: [finished], timeout: 0.25)
        XCTAssertEqual(model.phase, .idle)
    }

    func testFailedPreparationReturnsToSourceSelection() {
        let model = SwitchInstallerModel()
        model.importFile(URL(string: "https://example.com/app.ipa")!)
        XCTAssertNotEqual(model.phase, .idle)
        model.dismissReview()
        XCTAssertEqual(model.phase, .idle)
        XCTAssertEqual(model.technicalDetails, "")
    }

    func testNewHandoffDoesNotReplaceABusyImport() throws {
        let model = SwitchInstallerModel()
        let source = try fixture()
        model.importFile(source)
        model.importFile(source)
        XCTAssertEqual(model.phase, .importing)
        XCTAssertNotNil(model.notice)
        model.dismissReview()
    }

    func testFileProviderWaitShowsCancelableImportAboveSourceScreen() async throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".ipa")
        try FileManager.default.copyItem(at: fixture(), to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        let model = SwitchInstallerModel()
        let window = try host(model)
        defer { window.isHidden = true }
        await settleLayout()
        let held = expectation(description: "Provider holds the selected file")
        let release = DispatchSemaphore(value: 0)
        defer { release.signal() }
        DispatchQueue.global(qos: .userInitiated).async {
            NSFileCoordinator().coordinate(writingItemAt: url, options: [], error: nil) { _ in
                held.fulfill()
                _ = release.wait(timeout: .now() + 8)
            }
        }
        await fulfillment(of: [held], timeout: 4)
        model.importFile(url)
        await settleLayout()
        XCTAssertTrue(model.showsImportDialog)
        XCTAssertEqual(findNavigation(window.rootViewController)?.viewControllers.count, 1)
        XCTAssertNotNil(window.rootViewController?.presentedViewController)
        capture(window, name: "import-provider-wait")
        model.cancel()
        XCTAssertEqual(model.phase, .idle)
        XCTAssertFalse(model.showsImportDialog)
        XCTAssertTrue(FileManager.default.fileExists(atPath: url.path))
    }

    func testReviewUsesTheSystemNavigationStackAndBackButton() async throws {
        let model = SwitchInstallerModel()
        let window = try host(model)
        defer { window.isHidden = true }
        await settleLayout()
        let navigation = try XCTUnwrap(findNavigation(window.rootViewController))
        XCTAssertEqual(navigation.viewControllers.count, 1)
        XCTAssertEqual(navigation.topViewController?.navigationItem.largeTitleDisplayMode, .always)
        await prepare(model, source: try fixture())
        await settleLayout()
        XCTAssertEqual(navigation.viewControllers.count, 2)
        let top = try XCTUnwrap(navigation.topViewController)
        XCTAssertEqual(top.navigationItem.title, "Review App")
        XCTAssertFalse(top.navigationItem.hidesBackButton)
        XCTAssertEqual(top.navigationItem.largeTitleDisplayMode, .never)
        navigation.popViewController(animated: false)
        await settleLayout()
        XCTAssertEqual(model.phase, .idle)
        XCTAssertNil(model.package)
        XCTAssertEqual(navigation.viewControllers.count, 1)
    }

    func testRenderedLightDarkAndLargeTextLayouts() async throws {
        let variants: [(String, UIUserInterfaceStyle, DynamicTypeSize, CGSize)] = [
            ("landscape-light", .light, .large, CGSize(width: 1024, height: 576)),
            ("landscape-dark", .dark, .large, CGSize(width: 1024, height: 576)),
            ("narrow-large-text", .light, .accessibility3, CGSize(width: 500, height: 720)),
        ]
        for (name, style, typeSize, size) in variants {
            let model = SwitchInstallerModel()
            let window = try host(model, style: style, typeSize: typeSize, size: size)
            await settleLayout()
            capture(window, name: "\(name)-sources")
            await prepare(model, source: try fixture())
            await settleLayout()
            capture(window, name: "\(name)-review")
            let navigation = try XCTUnwrap(findNavigation(window.rootViewController))
            let controller = try XCTUnwrap(navigation.topViewController)
            let scroll = try XCTUnwrap(findScroll(controller.view))
            XCTAssertEqual(scroll.bounds.width, size.width, accuracy: 1)
            if typeSize.isAccessibilitySize {
                let bottom = max(-scroll.adjustedContentInset.top,
                                 scroll.contentSize.height - scroll.bounds.height + scroll.adjustedContentInset.bottom)
                scroll.setContentOffset(CGPoint(x: 0, y: bottom), animated: false)
                await settleLayout()
                capture(window, name: "\(name)-review-bottom")
            }
            model.dismissReview()
            window.isHidden = true
        }
    }

    func testReportedInstallProgressLayout() async throws {
        var progress = InstallerProgress()
        progress.update(status: "VerifyingApplication", percent: 64)
        let window = try host(SwitchInstallerModel())
        defer { window.isHidden = true }
        window.rootViewController = UIHostingController(rootView: List {
            Section {
                InstallerProgressRow(progress: progress)
                    .listRowSeparator(.hidden)
            }
            .listSectionSeparator(.hidden)
        }.listStyle(.plain))
        await settleLayout()
        capture(window, name: "install-progress")
    }

    private func fixture() throws -> URL {
        try XCTUnwrap(Bundle(for: Self.self).url(forResource: "valid", withExtension: "ipa"))
    }

    private func prepare(_ model: SwitchInstallerModel, source: URL) async {
        let ready = expectation(description: "Package is ready for explicit confirmation")
        model.$phase.filter { $0 == .ready }.first().sink { _ in
            ready.fulfill()
        }.store(in: &observations)
        model.importFile(source)
        await fulfillment(of: [ready], timeout: 5)
        XCTAssertEqual(model.phase, .ready, model.technicalDetails)
    }

    private func host(
        _ model: SwitchInstallerModel,
        style: UIUserInterfaceStyle = .light,
        typeSize: DynamicTypeSize = .large,
        size: CGSize = CGSize(width: 1024, height: 576)
    ) throws -> UIWindow {
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(origin: .zero, size: size)
        window.overrideUserInterfaceStyle = style
        window.rootViewController = UIHostingController(
            rootView: InstallerView(model: model).environment(\.dynamicTypeSize, typeSize)
        )
        window.makeKeyAndVisible()
        return window
    }

    private func findNavigation(_ controller: UIViewController?) -> UINavigationController? {
        guard let controller else { return nil }
        if let navigation = controller as? UINavigationController { return navigation }
        return controller.children.compactMap { findNavigation($0) }.first
    }

    private func findScroll(_ view: UIView) -> UIScrollView? {
        if let scroll = view as? UIScrollView { return scroll }
        return view.subviews.compactMap { findScroll($0) }.first
    }

    private func settleLayout() async {
        let layout = expectation(description: "Navigation layout committed")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { layout.fulfill() }
        await fulfillment(of: [layout], timeout: 2)
    }

    private func capture(_ window: UIWindow, name: String) {
        let image = UIGraphicsImageRenderer(bounds: window.bounds).image { _ in
            window.drawHierarchy(in: window.bounds, afterScreenUpdates: true)
        }
        let attachment = XCTAttachment(image: image)
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }
}
