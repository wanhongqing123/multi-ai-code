import QuartzCore
import SwiftUI
import UIKit

struct MaiFfplayPresentation: Identifiable {
    let id = UUID()
    let path: String
}

@MainActor
enum MaiFfplayMobilePlayer {
    static weak var active: MaiFfplayVideoController?

    static func command(_ action: String, percent: Double?) -> Bool {
        guard let active else { return false }
        if action == "close" {
            active.close()
            return true
        }
        if action == "seek_percent" {
            guard let percent, (0...100).contains(percent) else { return false }
            return active.seekPercent(percent / 100)
        }
        return active.sendCommand(action)
    }
}

private func maiFfplayRetainLayer(_ pointer: UnsafeMutableRawPointer?) {
    if let pointer { _ = Unmanaged<AnyObject>.fromOpaque(pointer).retain() }
}

private func maiFfplayReleaseLayer(_ pointer: UnsafeMutableRawPointer?) {
    if let pointer { Unmanaged<AnyObject>.fromOpaque(pointer).release() }
}

@MainActor
private final class MaiFfplayMetalView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var videoPath = ""
    var onError: ((String) -> Void)?
    private var viewID: UInt64 = 0
    private var session: OpaquePointer?
    private var lastSize: CGSize = .zero

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil { attach() }
        else { stop() }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard viewID != 0 else { attach(); return }
        let size = CGSize(width: max(1, bounds.width * contentScaleFactor),
                          height: max(1, bounds.height * contentScaleFactor))
        guard size != lastSize else { return }
        lastSize = size
        maiGraphicsPresenterResize(viewID, UInt32(size.width), UInt32(size.height))
    }

    private func attach() {
        guard viewID == 0, window != nil, bounds.width > 0, bounds.height > 0,
              !videoPath.isEmpty, maiGraphicsPresenterEnsureStarted() else { return }
        lastSize = CGSize(width: max(1, bounds.width * contentScaleFactor),
                          height: max(1, bounds.height * contentScaleFactor))
        viewID = maiGraphicsPresenterAttach(
            Unmanaged.passUnretained(layer).toOpaque(), UInt32(lastSize.width),
            UInt32(lastSize.height), maiFfplayRetainLayer, maiFfplayReleaseLayer
        )
        guard viewID != 0 else {
            onError?("Metal 视频视图无法创建")
            return
        }
        session = videoPath.withCString { path in maiFfplayIosStart(viewID, path) }
        if session == nil {
            maiGraphicsPresenterDetach(viewID)
            viewID = 0
            onError?("FFplay 无法启动")
        }
    }

    func sendCommand(_ action: String) -> Bool {
        guard let session else { return false }
        return action.withCString { maiFfplayIosCommand(session, $0) == 1 }
    }

    func seekPercent(_ fraction: Double) -> Bool {
        guard let session else { return false }
        return maiFfplayIosSeekPercent(session, fraction) == 1
    }

    func checkPlayback() {
        guard let session, maiFfplayIosHasFinished(session) == 1,
              maiFfplayIosExitCode(session) != 0 else { return }
        onError?("FFplay 无法播放此视频")
    }

    func stop() {
        if let session {
            self.session = nil
            maiFfplayIosStop(session)
        }
        if viewID != 0 {
            maiGraphicsPresenterDetach(viewID)
            viewID = 0
        }
    }
}

@MainActor
final class MaiFfplayVideoController: UIViewController {
    private let path: String
    private let onClose: () -> Void
    private let metalView = MaiFfplayMetalView()
    private let pauseButton = UIButton(type: .system)
    private let seekSlider = UISlider()
    private let errorLabel = UILabel()
    private var playbackTimer: Timer?
    private var playing = true

    init(path: String, close: @escaping () -> Void) {
        self.path = path
        onClose = close
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { nil }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        metalView.videoPath = path
        metalView.onError = { [weak self] message in
            self?.errorLabel.text = message
            self?.errorLabel.isHidden = false
        }
        metalView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(metalView)

        errorLabel.textColor = .white
        errorLabel.textAlignment = .center
        errorLabel.isHidden = true
        errorLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(errorLabel)

        pauseButton.setTitle("暂停", for: .normal)
        pauseButton.accessibilityIdentifier = "ffplay-pause"
        pauseButton.tintColor = .white
        pauseButton.addTarget(self, action: #selector(togglePause), for: .touchUpInside)
        seekSlider.addTarget(self, action: #selector(seekReleased), for: .touchUpInside)
        seekSlider.accessibilityIdentifier = "ffplay-seek"
        seekSlider.addTarget(self, action: #selector(seekReleased), for: .touchUpOutside)
        let controls = UIStackView(arrangedSubviews: [pauseButton, seekSlider])
        controls.axis = .horizontal
        controls.spacing = 16
        controls.backgroundColor = UIColor.black.withAlphaComponent(0.6)
        controls.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(controls)

        let closeButton = UIButton(type: .system)
        closeButton.setImage(UIImage(systemName: "xmark.circle.fill"), for: .normal)
        closeButton.accessibilityIdentifier = "ffplay-close"
        closeButton.tintColor = .white
        closeButton.addTarget(self, action: #selector(closeTapped), for: .touchUpInside)
        closeButton.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(closeButton)

        NSLayoutConstraint.activate([
            metalView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            metalView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            metalView.topAnchor.constraint(equalTo: view.topAnchor),
            metalView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            errorLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            errorLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            controls.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 16),
            controls.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -16),
            controls.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor,
                                             constant: -12),
            closeButton.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -18),
            closeButton.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor,
                                             constant: 12),
            closeButton.widthAnchor.constraint(equalToConstant: 44),
            closeButton.heightAnchor.constraint(equalToConstant: 44),
        ])
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        MaiFfplayMobilePlayer.active?.stop()
        MaiFfplayMobilePlayer.active = self
        playbackTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) {
            [weak self] _ in self?.metalView.checkPlayback()
        }
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        stop()
    }

    func stop() {
        playbackTimer?.invalidate()
        playbackTimer = nil
        metalView.stop()
        if MaiFfplayMobilePlayer.active === self { MaiFfplayMobilePlayer.active = nil }
    }

    func sendCommand(_ action: String) -> Bool {
        let command: String
        if action == "play" {
            if playing { return true }
            command = "pause"
        } else if action == "pause" {
            if !playing { return true }
            command = "pause"
        } else {
            command = action == "toggle_pause" ? "pause" : action
        }
        let accepted = metalView.sendCommand(command)
        if accepted && command == "pause" {
            playing.toggle()
            pauseButton.setTitle(playing ? "暂停" : "播放", for: .normal)
        }
        return accepted
    }

    func seekPercent(_ fraction: Double) -> Bool {
        metalView.seekPercent(fraction)
    }

    func close() { onClose() }

    @objc private func togglePause() { _ = sendCommand("toggle_pause") }
    @objc private func seekReleased() { _ = seekPercent(Double(seekSlider.value)) }
    @objc private func closeTapped() { close() }
}

struct MaiFfplayVideoScreen: UIViewControllerRepresentable {
    let path: String
    let close: () -> Void

    func makeUIViewController(context _: Context) -> MaiFfplayVideoController {
        MaiFfplayVideoController(path: path, close: close)
    }

    func updateUIViewController(_ controller: MaiFfplayVideoController, context _: Context) {
        _ = controller
    }

    static func dismantleUIViewController(_ controller: MaiFfplayVideoController,
                                          coordinator _: Void) {
        controller.stop()
    }
}

#if targetEnvironment(simulator)
struct MaiFfplayUITestRoot: View {
    @State private var showingVideo = false

    var body: some View {
        AIAssistantView()
            .fullScreenCover(isPresented: $showingVideo) {
                if let file = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4") {
                    MaiFfplayVideoScreen(path: file.path) { showingVideo = false }
                }
            }
            .task { showingVideo = true }
    }
}
#endif
