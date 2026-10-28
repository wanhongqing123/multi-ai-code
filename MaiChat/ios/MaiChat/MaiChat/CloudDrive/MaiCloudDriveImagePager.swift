// Cloud images stay in one native paging container throughout a drag. Downloaded
// preview URLs belong to this presentation and are removed when it closes.
import SwiftUI
import UIKit

@MainActor
struct MaiCloudDriveImagePager: UIViewControllerRepresentable {
    let images: [MaiCloudDriveVideo]
    let initialIndex: Int
    let loadImage: @MainActor (MaiCloudDriveVideo) async throws -> URL
    let close: () -> Void
    let hasMore: Bool
    let loadMore: @MainActor () -> Void

    init(images: [MaiCloudDriveVideo], initialIndex: Int,
         loadImage: @escaping @MainActor (MaiCloudDriveVideo) async throws -> URL,
         close: @escaping () -> Void, hasMore: Bool = false,
         loadMore: @escaping @MainActor () -> Void = {}) {
        self.images = images
        self.initialIndex = initialIndex
        self.loadImage = loadImage
        self.close = close
        self.hasMore = hasMore
        self.loadMore = loadMore
    }

    func makeUIViewController(context: Context) -> MaiCloudDriveImagePagerController {
        MaiCloudDriveImagePagerController(images: images, initialIndex: initialIndex,
                                         loader: loadImage, close: close,
                                         hasMore: hasMore, loadMore: loadMore)
    }

    func updateUIViewController(_ controller: MaiCloudDriveImagePagerController,
                                context: Context) {
        // Retain each loaded page's order/identity while later metadata pages
        // append to this presentation's snapshot.
        controller.updatePagination(hasMore: hasMore, loadMore: loadMore)
        controller.appendImages(images)
        controller.close = close
    }

    static func dismantleUIViewController(_ controller: MaiCloudDriveImagePagerController,
                                          coordinator: ()) {
        controller.dispose()
    }
}

@MainActor
final class MaiCloudDriveImagePagerController: UIViewController,
                                               UIPageViewControllerDataSource,
                                               UIPageViewControllerDelegate {
    private var images: [MaiCloudDriveVideo]
    private let loader: @MainActor (MaiCloudDriveVideo) async throws -> URL
    var close: () -> Void
    private var index: Int
    private let pager = UIPageViewController(transitionStyle: .scroll,
        navigationOrientation: .horizontal, options: [.interPageSpacing: 12])
    private let caption = UILabel()
    private let feedback = UILabel()
    private let shareButton = UIButton(type: .system)
    private let saveButton = UIButton(type: .system)
    private let moreButton = UIButton(type: .system)
    private var hasMore: Bool
    private var loadMore: @MainActor () -> Void
    private var requestedCount: Int?
    private var pages: [Int: MaiCloudDriveImagePageController] = [:]
    private var files: [Int: URL] = [:]
    private var recent: [Int] = []
    private var tasks: [Int: Task<Void, Never>] = [:]
    private var tickets: [Int: UUID] = [:]
    private var disposed = false
    private var transitioning = false
    private var refreshPending = false

    init(images: [MaiCloudDriveVideo], initialIndex: Int,
         loader: @escaping @MainActor (MaiCloudDriveVideo) async throws -> URL,
         close: @escaping () -> Void, hasMore: Bool,
         loadMore: @escaping @MainActor () -> Void) {
        self.images = images
        index = max(0, min(initialIndex, max(0, images.count - 1)))
        self.loader = loader
        self.close = close
        self.hasMore = hasMore
        self.loadMore = loadMore
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        addChild(pager)
        pager.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(pager.view)
        NSLayoutConstraint.activate([
            pager.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            pager.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            pager.view.topAnchor.constraint(equalTo: view.topAnchor),
            pager.view.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        ])
        pager.didMove(toParent: self)
        pager.dataSource = self
        pager.delegate = self
        buildControls()
        if let first = page(at: index) {
            pager.setViewControllers([first], direction: .forward, animated: false)
        }
        refreshCaption()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        if presentedViewController == nil && view.window == nil { dispose() }
    }

    private func buildControls() {
        let closeButton = actionButton(symbol: "xmark", label: "关闭图片",
                                       identifier: "cloud-drive-image-close")
        closeButton.addTarget(self, action: #selector(closeTapped), for: .touchUpInside)
        caption.textColor = .white
        caption.font = .systemFont(ofSize: 12, weight: .medium)
        caption.textAlignment = .center
        caption.lineBreakMode = .byTruncatingMiddle
        caption.accessibilityIdentifier = "cloud-drive-image-counter"
        caption.backgroundColor = UIColor.black.withAlphaComponent(0.6)
        caption.layer.cornerRadius = 15
        caption.clipsToBounds = true
        caption.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(closeButton)
        view.addSubview(caption)
        NSLayoutConstraint.activate([
            closeButton.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor,
                                                  constant: 16),
            closeButton.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor,
                                              constant: 12),
            caption.leadingAnchor.constraint(equalTo: closeButton.trailingAnchor, constant: 12),
            caption.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor,
                                                constant: -16),
            caption.centerYAnchor.constraint(equalTo: closeButton.centerYAnchor),
            caption.heightAnchor.constraint(equalToConstant: 32)
        ])
        configureAction(shareButton, symbol: "square.and.arrow.up", label: "分享图片",
                        identifier: "cloud-drive-image-share")
        configureAction(saveButton, symbol: "square.and.arrow.down", label: "保存图片",
                        identifier: "cloud-drive-image-save")
        shareButton.addTarget(self, action: #selector(shareTapped), for: .touchUpInside)
        saveButton.addTarget(self, action: #selector(saveTapped), for: .touchUpInside)
        let actions = UIStackView(arrangedSubviews: [shareButton, saveButton])
        actions.axis = .horizontal
        actions.spacing = 12
        actions.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(actions)
        feedback.textColor = .white
        feedback.font = .systemFont(ofSize: 13, weight: .medium)
        feedback.textAlignment = .right
        feedback.numberOfLines = 2
        feedback.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(feedback)
        moreButton.setTitle("加载更多图片", for: .normal)
        moreButton.titleLabel?.font = .systemFont(ofSize: 14, weight: .semibold)
        moreButton.tintColor = .white
        moreButton.backgroundColor = UIColor.black.withAlphaComponent(0.6)
        moreButton.layer.cornerRadius = 22
        moreButton.accessibilityIdentifier = "cloud-drive-image-load-more"
        moreButton.translatesAutoresizingMaskIntoConstraints = false
        moreButton.addTarget(self, action: #selector(loadMoreTapped), for: .touchUpInside)
        view.addSubview(moreButton)
        NSLayoutConstraint.activate([
            actions.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor,
                                               constant: -20),
            actions.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor,
                                              constant: -18),
            feedback.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor,
                                                constant: 20),
            feedback.trailingAnchor.constraint(equalTo: actions.trailingAnchor),
            feedback.bottomAnchor.constraint(equalTo: actions.topAnchor, constant: -12),
            moreButton.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor,
                                                  constant: 20),
            moreButton.centerYAnchor.constraint(equalTo: actions.centerYAnchor),
            moreButton.heightAnchor.constraint(equalToConstant: 44),
            moreButton.widthAnchor.constraint(equalToConstant: 130),
            moreButton.trailingAnchor.constraint(lessThanOrEqualTo: actions.leadingAnchor,
                                                   constant: -12)
        ])
    }

    private func actionButton(symbol: String, label: String, identifier: String) -> UIButton {
        let button = UIButton(type: .system)
        configureAction(button, symbol: symbol, label: label, identifier: identifier)
        return button
    }

    private func configureAction(_ button: UIButton, symbol: String,
                                 label: String, identifier: String) {
        button.setImage(UIImage(systemName: symbol,
            withConfiguration: UIImage.SymbolConfiguration(pointSize: 18, weight: .semibold)),
            for: .normal)
        button.tintColor = .white
        button.backgroundColor = UIColor.black.withAlphaComponent(0.6)
        button.layer.cornerRadius = 24
        button.accessibilityLabel = label
        button.accessibilityIdentifier = identifier
        button.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            button.widthAnchor.constraint(equalToConstant: 48),
            button.heightAnchor.constraint(equalToConstant: 48)
        ])
    }

    private func page(at target: Int) -> MaiCloudDriveImagePageController? {
        guard images.indices.contains(target), !disposed else { return nil }
        if let existing = pages[target] { return existing }
        let page = MaiCloudDriveImagePageController(index: target, name: images[target].name)
        page.retry = { [weak self] in self?.load(target, retry: true) }
        page.zoomChanged = { [weak self] zoomed in
            guard let self, self.index == target else { return }
            self.pagingScrollView?.isScrollEnabled = !zoomed
        }
        pages[target] = page
        load(target)
        return page
    }

    private var pagingScrollView: UIScrollView? {
        pager.view.subviews.compactMap { $0 as? UIScrollView }.first
    }

    private func load(_ target: Int, retry: Bool = false) {
        guard !disposed, images.indices.contains(target) else { return }
        if !retry, tasks[target] != nil { return }
        tasks[target]?.cancel()
        if retry, let old = files.removeValue(forKey: target) {
            try? FileManager.default.removeItem(at: old)
        }
        pages[target]?.showLoading()
        let ticket = UUID()
        tickets[target] = ticket
        let cached = files[target]
        let item = images[target]
        let loader = loader
        tasks[target] = Task { @MainActor [weak self] in
            do {
                let url: URL
                if let cached { url = cached }
                else { url = try await loader(item) }
                guard !Task.isCancelled, let self, !self.disposed,
                      self.tickets[target] == ticket else {
                    if cached == nil { try? FileManager.default.removeItem(at: url) }
                    return
                }
                self.files[target] = url
                self.recent.removeAll { $0 == target }
                self.recent.append(target)
                guard let image = await MaiPreviewImageLoader.image(at: url) else {
                    throw CocoaError(.fileReadCorruptFile)
                }
                guard !Task.isCancelled, !self.disposed,
                      self.tickets[target] == ticket else { return }
                self.pages[target]?.show(image)
                self.tasks[target] = nil
                self.trimCache()
                self.refreshActions()
            } catch {
                guard !Task.isCancelled, let self, !self.disposed,
                      self.tickets[target] == ticket else { return }
                self.tasks[target] = nil
                if let url = self.files.removeValue(forKey: target) {
                    try? FileManager.default.removeItem(at: url)
                }
                self.pages[target]?.showError(error.localizedDescription)
                self.refreshActions()
            }
        }
    }

    private func trimCache() {
        let neighbors = Set([index - 1, index, index + 1])
        // At most three decoded pages and eight downloaded originals survive.
        // In-flight pages next to the visible one are retained until transition.
        for target in Array(pages.keys) where !neighbors.contains(target) {
            pages.removeValue(forKey: target)
            tasks.removeValue(forKey: target)?.cancel()
            tickets.removeValue(forKey: target)
        }
        while files.count > 8,
              let oldest = recent.first(where: { !neighbors.contains($0) }) {
            recent.removeAll { $0 == oldest }
            if let url = files.removeValue(forKey: oldest) {
                try? FileManager.default.removeItem(at: url)
            }
        }
    }

    private func refreshCaption() {
        caption.text = images.isEmpty ? "没有可浏览的图片" :
            "\(index + 1)/\(hasMore ? "已加载\(images.count)" : "\(images.count)") · \(images[index].name)"
        moreButton.isHidden = !hasMore || index < images.count - 1
        feedback.text = nil
        refreshActions()
        requestMoreIfNeeded()
    }

    func updatePagination(hasMore: Bool, loadMore: @escaping @MainActor () -> Void) {
        self.loadMore = loadMore
        guard self.hasMore != hasMore else { return }
        self.hasMore = hasMore
        guard isViewLoaded else { return }
        refreshCaption()
    }

    private func requestMoreIfNeeded(force: Bool = false) {
        guard !disposed, hasMore, index >= images.count - 5,
              force || requestedCount != images.count else { return }
        requestedCount = images.count
        loadMore()
    }

    @objc private func loadMoreTapped() { requestMoreIfNeeded(force: true) }

    private func refreshActions() {
        let ready = pages[index]?.hasImage == true && files[index] != nil
        shareButton.isEnabled = ready
        saveButton.isEnabled = ready
        shareButton.alpha = ready ? 1 : 0.4
        saveButton.alpha = ready ? 1 : 0.4
    }

    func appendImages(_ incoming: [MaiCloudDriveVideo]) {
        guard !disposed else { return }
        var known = Set(images.map(\.id))
        let additions = incoming.filter { known.insert($0.id).inserted }
        guard !additions.isEmpty else { return }
        images.append(contentsOf: additions)
        guard isViewLoaded else { return }
        refreshCaption()
        refreshPending = true
        refreshPagingNeighbors()
    }

    private func refreshPagingNeighbors() {
        guard refreshPending, !transitioning else { return }
        refreshPending = false
        // Re-query a formerly empty end neighbor after pagination, keeping the
        // current controller (and its zoom state) in the same container.
        if let current = pager.viewControllers?.first ?? page(at: index) {
            pager.setViewControllers([current], direction: .forward, animated: false)
        }
    }

    func pageViewController(_ pageViewController: UIPageViewController,
                            viewControllerBefore viewController: UIViewController) -> UIViewController? {
        guard let page = viewController as? MaiCloudDriveImagePageController else { return nil }
        return self.page(at: page.index - 1)
    }

    func pageViewController(_ pageViewController: UIPageViewController,
                            viewControllerAfter viewController: UIViewController) -> UIViewController? {
        guard let page = viewController as? MaiCloudDriveImagePageController else { return nil }
        return self.page(at: page.index + 1)
    }

    func pageViewController(_ pageViewController: UIPageViewController,
                            willTransitionTo pendingViewControllers: [UIViewController]) {
        transitioning = true
    }

    func pageViewController(_ pageViewController: UIPageViewController,
                            didFinishAnimating finished: Bool,
                            previousViewControllers: [UIViewController],
                            transitionCompleted completed: Bool) {
        transitioning = false
        defer { refreshPagingNeighbors() }
        guard completed,
              let current = pageViewController.viewControllers?.first
                as? MaiCloudDriveImagePageController else { return }
        index = current.index
        pagingScrollView?.isScrollEnabled = !current.isZoomed
        refreshCaption()
        trimCache()
    }

    @objc private func closeTapped() {
        // SwiftUI may retain a dismissed representable before dismantling it.
        // Release this presentation's files at the explicit user close action.
        dispose()
        close()
    }

    @objc private func shareTapped() {
        guard let url = files[index], pages[index]?.hasImage == true else { return }
        let sheet = UIActivityViewController(activityItems: [url], applicationActivities: nil)
        sheet.popoverPresentationController?.sourceView = shareButton
        sheet.popoverPresentationController?.sourceRect = shareButton.bounds
        present(sheet, animated: true)
    }

    @objc private func saveTapped() {
        guard let url = files[index], pages[index]?.hasImage == true else { return }
        let savedIndex = index
        saveButton.isEnabled = false
        Task { @MainActor [weak self] in
            do {
                guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
                    self?.feedback.text = "请允许保存到相册"
                    self?.refreshActions()
                    return
                }
                try await MaiPhotoLibraryMediaWriter.saveImage(at: url, createdAt: Date())
                if self?.index == savedIndex { self?.feedback.text = "已保存到相册" }
            } catch {
                if self?.index == savedIndex { self?.feedback.text = error.localizedDescription }
            }
            self?.refreshActions()
        }
    }

    func dispose() {
        guard !disposed else { return }
        disposed = true
        for task in tasks.values { task.cancel() }
        tasks.removeAll()
        tickets.removeAll()
        for url in files.values { try? FileManager.default.removeItem(at: url) }
        files.removeAll()
        pages.removeAll()
        pager.dataSource = nil
        pager.delegate = nil
    }
}

// At fit size the image pan recognizer yields to the paging container. Pinching
// and double-tapping still belong to the image; zoomed drags only pan that image.
private final class MaiCloudDriveZoomScrollView: UIScrollView {
    override func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
        if gestureRecognizer === panGestureRecognizer && zoomScale <= minimumZoomScale + 0.01 {
            return false
        }
        return super.gestureRecognizerShouldBegin(gestureRecognizer)
    }
}

@MainActor
private final class MaiCloudDriveImagePageController: UIViewController, UIScrollViewDelegate {
    let index: Int
    private let name: String
    var retry: (() -> Void)?
    var zoomChanged: ((Bool) -> Void)?
    private let scroll = MaiCloudDriveZoomScrollView()
    private let imageView = UIImageView()
    private let spinner = UIActivityIndicatorView(style: .large)
    private let message = UILabel()
    private let retryButton = UIButton(type: .system)
    private let stateStack = UIStackView()
    private var lastSize = CGSize.zero
    var hasImage: Bool { imageView.image != nil }
    var isZoomed: Bool { scroll.zoomScale > scroll.minimumZoomScale + 0.01 }

    init(index: Int, name: String) {
        self.index = index
        self.name = name
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        scroll.delegate = self
        scroll.minimumZoomScale = 1
        scroll.maximumZoomScale = 4
        scroll.bounces = false
        scroll.bouncesZoom = false
        scroll.showsHorizontalScrollIndicator = false
        scroll.showsVerticalScrollIndicator = false
        scroll.contentInsetAdjustmentBehavior = .never
        view.addSubview(scroll)
        imageView.contentMode = .scaleAspectFit
        imageView.isAccessibilityElement = true
        imageView.accessibilityLabel = name
        imageView.accessibilityIdentifier = "cloud-drive-image-preview"
        scroll.addSubview(imageView)
        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(toggleZoom(_:)))
        doubleTap.numberOfTapsRequired = 2
        scroll.addGestureRecognizer(doubleTap)
        spinner.color = .white
        message.textColor = .white
        message.font = .preferredFont(forTextStyle: .body)
        message.numberOfLines = 4
        message.textAlignment = .center
        retryButton.setTitle("重试", for: .normal)
        retryButton.tintColor = .white
        retryButton.titleLabel?.font = .preferredFont(forTextStyle: .headline)
        retryButton.accessibilityIdentifier = "cloud-drive-image-retry"
        retryButton.addTarget(self, action: #selector(retryTapped), for: .touchUpInside)
        stateStack.axis = .vertical
        stateStack.alignment = .center
        stateStack.spacing = 16
        [spinner, message, retryButton].forEach { stateStack.addArrangedSubview($0) }
        stateStack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stateStack)
        NSLayoutConstraint.activate([
            stateStack.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            stateStack.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            stateStack.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor, constant: -64)
        ])
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        guard lastSize != view.bounds.size else { return }
        lastSize = view.bounds.size
        scroll.frame = view.bounds
        fitImage()
    }

    func showLoading() {
        loadViewIfNeeded()
        stateStack.isHidden = false
        spinner.isHidden = false
        spinner.startAnimating()
        message.text = "正在加载图片…"
        retryButton.isHidden = true
    }

    func show(_ image: UIImage) {
        loadViewIfNeeded()
        imageView.image = image
        stateStack.isHidden = true
        spinner.stopAnimating()
        fitImage()
    }

    func showError(_ detail: String) {
        loadViewIfNeeded()
        stateStack.isHidden = false
        spinner.stopAnimating()
        spinner.isHidden = true
        message.text = "图片加载失败\n\(detail)"
        retryButton.isHidden = false
    }

    private func fitImage() {
        scroll.setZoomScale(1, animated: false)
        guard let image = imageView.image, image.size.width > 0, image.size.height > 0,
              scroll.bounds.width > 0, scroll.bounds.height > 0 else { return }
        let scale = min(scroll.bounds.width / image.size.width,
                        scroll.bounds.height / image.size.height)
        let fitted = CGSize(width: image.size.width * scale, height: image.size.height * scale)
        imageView.frame = CGRect(origin: .zero, size: fitted)
        scroll.contentSize = fitted
        centerImage()
        zoomChanged?(false)
    }

    private func centerImage() {
        let vertical = max(0, (scroll.bounds.height - scroll.contentSize.height) / 2)
        let horizontal = max(0, (scroll.bounds.width - scroll.contentSize.width) / 2)
        scroll.contentInset = UIEdgeInsets(top: vertical, left: horizontal,
                                          bottom: vertical, right: horizontal)
    }

    func viewForZooming(in scrollView: UIScrollView) -> UIView? { imageView }

    func scrollViewWillBeginZooming(_ scrollView: UIScrollView, with view: UIView?) {
        zoomChanged?(true)
    }

    func scrollViewDidZoom(_ scrollView: UIScrollView) {
        centerImage()
        zoomChanged?(isZoomed)
    }

    func scrollViewDidEndZooming(_ scrollView: UIScrollView, with view: UIView?, atScale scale: CGFloat) {
        zoomChanged?(isZoomed)
    }

    @objc private func toggleZoom(_ gesture: UITapGestureRecognizer) {
        guard hasImage else { return }
        if isZoomed { scroll.setZoomScale(1, animated: true) }
        else {
            zoomChanged?(true)
            let point = gesture.location(in: imageView)
            let size = CGSize(width: scroll.bounds.width / 2.5, height: scroll.bounds.height / 2.5)
            scroll.zoom(to: CGRect(x: point.x - size.width / 2, y: point.y - size.height / 2,
                                   width: size.width, height: size.height), animated: true)
        }
    }

    @objc private func retryTapped() { retry?() }
}

#if targetEnvironment(simulator)
@MainActor
private final class MaiCloudImagePagerFixture: ObservableObject {
    @Published var loadedCount = ProcessInfo.processInfo.environment[
        "MAICHAT_CLOUD_PAGER_APPEND_TEST"] == "YES" ? 2 : 3
    private var appending = false
    let images = (0..<3).map { index in
        MaiCloudDriveVideo(id: "cloud-image-\(index)", name: "图片\(index + 1).png",
            sizeBytes: 1024, contentType: "image/png", mediaType: "image",
            createdAt: 1_793_181_600_000, folderId: nil)
    }
    var files: [URL] = []
    var failSecond = true

    func loadMore() {
        guard loadedCount == 2, !appending else { return }
        appending = true
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(5))
            loadedCount = 3
        }
    }

    func load(_ item: MaiCloudDriveVideo) async throws -> URL {
        if item.id == images[1].id && failSecond {
            failSecond = false
            throw CocoaError(.fileReadNoPermission)
        }
        let index = images.firstIndex(where: { $0.id == item.id }) ?? 0
        let colors: [UIColor] = [.systemRed, .systemGreen, .systemBlue]
        let pixels = UIGraphicsImageRenderer(size: CGSize(width: 900, height: 1200)).image { context in
            colors[index].setFill()
            context.fill(CGRect(x: 0, y: 0, width: 900, height: 1200))
            ("CLOUD IMAGE \(index + 1)" as NSString).draw(at: CGPoint(x: 100, y: 550),
                withAttributes: [.font: UIFont.boldSystemFont(ofSize: 65),
                                 .foregroundColor: UIColor.white])
        }
        let file = FileManager.default.temporaryDirectory
            .appendingPathComponent("cloud-image-pager-fixture-" + UUID().uuidString + ".png")
        try pixels.pngData()!.write(to: file)
        files.append(file)
        return file
    }
}

struct MaiCloudImagePagerUITestRoot: View {
    @StateObject private var fixture = MaiCloudImagePagerFixture()
    @State private var showing = false
    @State private var cleaned = false

    var body: some View {
        VStack(spacing: 20) {
            Button("打开云盘图片测试") { showing = true }
                .accessibilityIdentifier("cloud-drive-pager-fixture-open")
            if cleaned { Text("云盘图片缓存已清理") }
        }
        .fullScreenCover(isPresented: $showing) {
            MaiCloudDriveImagePager(images: Array(fixture.images.prefix(fixture.loadedCount)),
                initialIndex: 0, loadImage: { try await fixture.load($0) },
                close: { showing = false }, hasMore: fixture.loadedCount < 3,
                loadMore: { fixture.loadMore() })
                .ignoresSafeArea()
        }
        .onChange(of: showing) { open in
            guard !open else { return }
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(500))
                cleaned = !fixture.files.isEmpty && fixture.files.allSatisfy {
                    !FileManager.default.fileExists(atPath: $0.path)
                }
            }
        }
    }
}
#endif
