import SwiftUI
import SwitchInstallerInput
import UIKit
import UniformTypeIdentifiers

struct InstallerFilePicker: UIViewControllerRepresentable {
    let completion: (Result<URL?, Error>) -> Void

    static func controller() -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(
            forOpeningContentTypes: [UTType(importedAs: "com.apple.itunes.ipa", conformingTo: .data)],
            asCopy: true
        )
        picker.allowsMultipleSelection = false
        return picker
    }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = Self.controller()
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIDocumentPickerViewController, context: Context) {
        controller.delegate = context.coordinator
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(completion: completion)
    }

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        private let completion: (Result<URL?, Error>) -> Void
        private var completed = false

        init(completion: @escaping (Result<URL?, Error>) -> Void) {
            self.completion = completion
        }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            guard urls.count == 1, let url = urls.first else {
                finish(.failure(InstallerFailure("Files did not return a single IPA. Please choose the file again.")))
                return
            }
            finish(.success(url))
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            finish(.success(nil))
        }

        private func finish(_ result: Result<URL?, Error>) {
            guard !completed else { return }
            completed = true
            completion(result)
        }
    }
}
