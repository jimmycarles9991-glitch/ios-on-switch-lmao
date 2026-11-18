import Foundation

public final class SwitchInstallerDownload: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var session: URLSession?
    private var task: URLSessionDownloadTask?
    private var completion: ((Result<URL, Error>) -> Void)?
    private var update: ((Int64, Int64) -> Void)?
    private var failure: Error?
    private let destination: URL

    public init(destination: URL) {
        self.destination = destination
    }

    public func start(
        url: URL,
        progress: @escaping (Int64, Int64) -> Void,
        completion: @escaping (Result<URL, Error>) -> Void
    ) {
        lock.lock()
        self.update = progress
        self.completion = completion
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 60
        configuration.timeoutIntervalForResource = 60 * 60
        let session = URLSession(configuration: configuration, delegate: self, delegateQueue: nil)
        self.session = session
        let task = session.downloadTask(with: url)
        self.task = task
        lock.unlock()
        task.resume()
    }

    public func cancel() {
        lock.lock()
        let task = task
        lock.unlock()
        task?.cancel()
    }

    private func finish(_ result: Result<URL, Error>) {
        lock.lock()
        let completion = completion
        self.completion = nil
        update = nil
        task = nil
        let session = session
        self.session = nil
        lock.unlock()
        session?.finishTasksAndInvalidate()
        DispatchQueue.main.async { completion?(result) }
    }

    public func urlSession(
        _ session: URLSession,
        task: URLSessionTask,
        willPerformHTTPRedirection response: HTTPURLResponse,
        newRequest request: URLRequest,
        completionHandler: @escaping (URLRequest?) -> Void
    ) {
        guard let url = request.url,
              (try? InstallerInput.downloadURL(url.absoluteString)) != nil
        else {
            lock.lock()
            failure = InstallerFailure("The server redirected this download to an insecure or invalid link.")
            lock.unlock()
            completionHandler(nil)
            return
        }
        completionHandler(request)
    }

    public func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didWriteData bytesWritten: Int64,
        totalBytesWritten: Int64,
        totalBytesExpectedToWrite: Int64
    ) {
        lock.lock()
        if totalBytesWritten > InstallerInput.maximumDownloadSize
            || totalBytesExpectedToWrite > InstallerInput.maximumDownloadSize {
            failure = InstallerFailure("This IPA is larger than the 8 GB download limit.")
            lock.unlock()
            downloadTask.cancel()
            return
        }
        let update = update
        lock.unlock()
        DispatchQueue.main.async { update?(totalBytesWritten, totalBytesExpectedToWrite) }
    }

    public func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didFinishDownloadingTo location: URL
    ) {
        lock.lock()
        let failure = failure
        lock.unlock()
        do {
            if let failure { throw failure }
            try InstallerInput.checkResponse(downloadTask.response)
            let size = try location.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
            guard size > 0, Int64(size) <= InstallerInput.maximumDownloadSize else {
                throw InstallerFailure("The server returned an empty or oversized file.")
            }
            try FileManager.default.createDirectory(
                at: destination.deletingLastPathComponent(), withIntermediateDirectories: true
            )
            try FileManager.default.moveItem(at: location, to: destination)
            finish(.success(destination))
        } catch {
            finish(.failure(error))
        }
    }

    public func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error else { return }
        lock.lock()
        let failure = failure
        lock.unlock()
        finish(.failure(failure ?? error))
    }
}
