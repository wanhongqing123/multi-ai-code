import AVFoundation
import QuartzCore
import SwiftUI
import UIKit

struct MaiFfplayPresentation: Identifiable {
    let id = UUID()
    let path: String
}

@MainActor
enum MaiFfplayMobilePlayer {
    static var active: MaiFfplayVideoController?

    static var unavailableReason: String {
        guard let active else { return "no video popup is active" }
        return active.playbackUnavailableReason
    }

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
        if window != nil {
            updateDisplayScale()
            attach()
        }
        else { stop() }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateDisplayScale()
        guard viewID != 0 else { attach(); return }
        let size = CGSize(width: max(1, bounds.width * contentScaleFactor),
                          height: max(1, bounds.height * contentScaleFactor))
        guard size != lastSize else { return }
        lastSize = size
        #if DEBUG
        NSLog("[FFplayDisplay] resize %@", "view=\(Int(bounds.width))x\(Int(bounds.height)) scale=\(contentScaleFactor) pixels=\(Int(size.width))x\(Int(size.height))")
        #endif
        maiGraphicsPresenterResize(viewID, UInt32(size.width), UInt32(size.height))
    }

    private func updateDisplayScale() {
        guard let scale = window?.screen.scale else { return }
        if contentScaleFactor != scale { contentScaleFactor = scale }
        if let metalLayer = layer as? CAMetalLayer, metalLayer.contentsScale != scale {
            metalLayer.contentsScale = scale
        }
    }

    private func attach() {
        guard viewID == 0, window != nil, bounds.width > 0, bounds.height > 0,
              !videoPath.isEmpty, maiGraphicsPresenterEnsureStarted() else { return }
        do {
            let audioSession = AVAudioSession.sharedInstance()
            try audioSession.setCategory(.playback, mode: .moviePlayback)
            try audioSession.setActive(true)
        } catch {
            onError?("无法启动视频声音：\(error.localizedDescription)")
            return
        }
        lastSize = CGSize(width: max(1, bounds.width * contentScaleFactor),
                          height: max(1, bounds.height * contentScaleFactor))
        #if DEBUG
        NSLog("[FFplayDisplay] attach %@", "view=\(Int(bounds.width))x\(Int(bounds.height)) contentScale=\(contentScaleFactor) screenScale=\(window?.screen.scale ?? 0) pixels=\(Int(lastSize.width))x\(Int(lastSize.height))")
        #endif
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

    func playbackStatus() -> MaiFfplayPlaybackStatus? {
        guard session != nil else { return nil }
        var status = MaiFfplayPlaybackStatus()
        return maiFfplayGetPlaybackStatus(&status) == 1 ? status : nil
    }

    var unavailableReason: String {
        guard let session else { return "the video popup has no playback session" }
        if maiFfplayIosHasFinished(session) == 1 {
            return "FFplay exited with status \(maiFfplayIosExitCode(session))"
        }
        return "FFplay is running but did not accept the command"
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
    private let timeLabel = UILabel()
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
        timeLabel.text = "0:00 / 0:00"
        timeLabel.textColor = .white
        timeLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .medium)
        let controls = UIStackView(arrangedSubviews: [pauseButton, seekSlider, timeLabel])
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
        if let previous = MaiFfplayMobilePlayer.active, previous !== self {
            previous.stop()
        }
        MaiFfplayMobilePlayer.active = self
        if playbackTimer == nil {
            playbackTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) {
                [weak self] _ in self?.refreshPlayback()
            }
        }
        refreshPlayback()
    }

    private func refreshPlayback() {
        metalView.checkPlayback()
        guard let status = metalView.playbackStatus(), status.duration_us > 0 else { return }
        let position = min(max(status.position_us, 0), status.duration_us)
        if !seekSlider.isTracking {
            seekSlider.value = Float(Double(position) / Double(status.duration_us))
        }
        timeLabel.text = "\(Self.timestamp(position)) / \(Self.timestamp(status.duration_us))"
        playing = status.paused == 0
        pauseButton.setTitle(playing ? "暂停" : "播放", for: .normal)
    }

    private static func timestamp(_ microseconds: Int64) -> String {
        let seconds = max(0, microseconds / 1_000_000)
        return String(format: "%lld:%02lld", seconds / 60, seconds % 60)
    }

    func stop() {
        playbackTimer?.invalidate()
        playbackTimer = nil
        metalView.stop()
        if MaiFfplayMobilePlayer.active === self { MaiFfplayMobilePlayer.active = nil }
    }

    var playbackUnavailableReason: String { metalView.unavailableReason }

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
