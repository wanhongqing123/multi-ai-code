// 文件作用：实现 iOS 视频播放器界面及 FFplay 控制桥接，供用户和 Agent 共用播放状态。
// 阅读边界：视频解码在底层 FFplay；本文件负责播放控制、进度、音频会话和手势反馈。
// 持有播放器视图、进度、声音和分屏控件，并接收 Agent 播放命令。
import AVFoundation
import QuartzCore
import SwiftUI
import UIKit

enum MaiMediaKind: Equatable {
    case image
    case video
}

struct MaiMediaItem: Equatable {
    let kind: MaiMediaKind
    let path: String
}

struct MaiMediaSequence: Equatable {
    let items: [MaiMediaItem]
    let index: Int
    let groupID: UUID

    var current: MaiMediaItem { items[index] }

    init(selected: MaiMediaItem, items: [MaiMediaItem], groupID: UUID = UUID()) {
        self.groupID = groupID
        var seen = Set<String>()
        var available = items.filter { item in
            FileManager.default.fileExists(atPath: item.path) &&
                seen.insert("\(item.kind):\(item.path)").inserted
        }
        if !available.contains(selected) { available.insert(selected, at: 0) }
        self.items = available
        index = available.firstIndex(of: selected) ?? 0
    }

    func adjacent(_ direction: Int) -> MaiMediaSequence? {
        var next = index + direction
        while items.indices.contains(next) {
            let candidate = items[next]
            if FileManager.default.fileExists(atPath: candidate.path) {
                return MaiMediaSequence(selected: candidate, items: items, groupID: groupID)
            }
            next += direction
        }
        return nil
    }

    func updatingItems(_ next: [MaiMediaItem]) -> MaiMediaSequence {
        MaiMediaSequence(selected: current, items: next, groupID: groupID)
    }
}

struct MaiFfplayPresentation: Identifiable {
    let id: UUID
    let startSeconds: Double
    let sequence: MaiMediaSequence
    let entryDirection: Int
    let vod: MaiCloudVodSession?

    var groupID: UUID { sequence.groupID }
    var path: String { sequence.current.path }
    var playlist: [String] { sequence.items.map(\.path) }
    var index: Int { sequence.index }

    init(path: String, startSeconds: Double = 0, playlist: [String] = [],
         id: UUID = UUID(),
         groupID: UUID = UUID(), entryDirection: Int = 0,
         vod: MaiCloudVodSession? = nil) {
        self.id = id
        self.startSeconds = startSeconds
        self.entryDirection = entryDirection
        self.vod = vod
        sequence = MaiMediaSequence(selected: MaiMediaItem(kind: .video, path: path),
                                    items: playlist.map { MaiMediaItem(kind: .video, path: $0) },
                                    groupID: groupID)
    }

    init(sequence: MaiMediaSequence, startSeconds: Double = 0, id: UUID = UUID(),
         entryDirection: Int = 0, vod: MaiCloudVodSession? = nil) {
        self.id = id
        self.startSeconds = startSeconds
        self.sequence = sequence
        self.entryDirection = entryDirection
        self.vod = vod
    }

    func updatingPlaylist(_ paths: [String]) -> MaiFfplayPresentation {
        // 保持播放器视图身份不变，后台补齐历史视频时不能重播当前文件。
        updatingMediaItems(paths.map { MaiMediaItem(kind: .video, path: $0) })
    }

    func updatingMediaItems(_ items: [MaiMediaItem]) -> MaiFfplayPresentation {
        MaiFfplayPresentation(sequence: sequence.updatingItems(items),
                              startSeconds: startSeconds, id: id,
                              entryDirection: entryDirection, vod: vod)
    }

    func adjacent(_ direction: Int) -> MaiFfplayPresentation? {
        guard let next = sequence.adjacent(direction), next.current.kind == .video else { return nil }
        return MaiFfplayPresentation(sequence: next, entryDirection: direction)
    }
}

struct MaiGalleryImageScreen: View {
    let sequence: MaiMediaSequence
    let entryDirection: Int
    let navigate: (Int) -> Bool
    let close: () -> Void
    @State private var isZoomed = false
    @State private var status = ""
    @State private var arrived = false

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                Color.black.ignoresSafeArea()
                ImagePreviewBackdrop(filePath: sequence.current.path,
                                     viewportSize: geometry.size)
                    .offset(x: arrived ? 0 : CGFloat(entryDirection * 48))
                    .opacity(arrived ? 1 : 0.48)
                RemoteIMAsyncImage(filePath: sequence.current.path,
                    maximumPointSize: ImagePreviewZoomModifier.maximumDecodePointSize(
                        for: geometry.size)) { image in
                    let fitted = fittedSize(image.size, inside: geometry.size)
                    Image(uiImage: image).resizable().scaledToFit()
                        .frame(width: fitted.width, height: fitted.height)
                        .modifier(ImagePreviewZoomModifier(enabled: true,
                            imageSize: fitted, viewportSize: geometry.size,
                            onScaleChanged: { isZoomed = $0 > 1.01 }))
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .contentShape(Rectangle())
                        .onTapGesture(perform: close)
                        .simultaneousGesture(DragGesture(minimumDistance: 50).onEnded { value in
                            guard !isZoomed,
                                  abs(value.translation.width) > abs(value.translation.height) * 1.4
                            else { return }
                            let direction = value.translation.width < 0 ? 1 : -1
                            if !navigate(direction) {
                                status = direction > 0 ? "已经是最后一项" : "已经是第一项"
                            }
                        })
                } placeholder: { failed in
                    if failed { Text("图片无法显示").foregroundStyle(.white) }
                    else { ProgressView().tint(.white) }
                }
                .id(sequence.current.path)
                .offset(x: arrived ? 0 : CGFloat(entryDirection * 48))
                .opacity(arrived ? 1 : 0.48)

                VStack {
                    HStack(alignment: .top) {
                        Button(action: close) {
                            Image(systemName: "xmark")
                                .font(.system(size: 20, weight: .semibold))
                                .foregroundStyle(.white)
                                .frame(width: 50, height: 50)
                                .background(.black.opacity(0.58), in: Circle())
                        }
                        .accessibilityLabel("关闭图片预览")
                        Spacer(minLength: 12)
                        Text("\(sequence.index + 1)/\(sequence.items.count) · " +
                             URL(fileURLWithPath: sequence.current.path).lastPathComponent)
                            .font(.system(size: 12, weight: .medium))
                            .lineLimit(1)
                            .foregroundStyle(.white)
                            .padding(.horizontal, 10).padding(.vertical, 7)
                            .background(.black.opacity(0.58), in: Capsule())
                        Spacer(minLength: 62)
                    }
                    Spacer()
                    if !status.isEmpty {
                        Text(status).font(.system(size: 13, weight: .medium))
                            .foregroundStyle(.white)
                            .padding(10).background(.black.opacity(0.65), in: Capsule())
                    }
                    HStack(spacing: 14) {
                        ShareLink(item: URL(fileURLWithPath: sequence.current.path)) {
                            Image(systemName: "square.and.arrow.up")
                                .frame(width: 48, height: 48)
                        }
                        Button {
                            Task {
                                do {
                                    guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
                                        status = "请允许保存到相册"
                                        return
                                    }
                                    try await MaiPhotoLibraryMediaWriter.saveImage(
                                        at: URL(fileURLWithPath: sequence.current.path),
                                        createdAt: Date())
                                    status = "已保存到相册"
                                } catch { status = error.localizedDescription }
                            }
                        } label: {
                            Image(systemName: "square.and.arrow.down")
                                .frame(width: 48, height: 48)
                        }
                        .accessibilityLabel("保存图片")
                    }
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(.white)
                    .buttonStyle(.plain)
                    .background(.black.opacity(0.58), in: Capsule())
                    .frame(maxWidth: .infinity, alignment: .trailing)
                }
                .padding(.horizontal, 20).padding(.vertical, 54)
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
        }
        .ignoresSafeArea()
        .onAppear {
            withAnimation(.easeOut(duration: 0.22)) { arrived = true }
        }
        .onChange(of: sequence.current.path) { _ in
            isZoomed = false
            status = ""
        }
    }

    private func fittedSize(_ source: CGSize, inside viewport: CGSize) -> CGSize {
        guard source.width > 0, source.height > 0 else { return viewport }
        let factor = min(viewport.width / source.width, viewport.height / source.height)
        return CGSize(width: source.width * factor, height: source.height * factor)
    }
}

@MainActor
enum MaiFfplayMobilePlayer {
    static var active: MaiFfplayVideoController?

    static var unavailableReason: String {
        guard let active else { return "no video popup is active" }
        return active.playbackUnavailableReason
    }

    static func command(_ action: String, percent: Double?, speed: Double?, seconds: Double?) -> Bool {
        guard let active else { return false }
        if action == "close" {
            active.close()
            return true
        }
        // 语音输入会暂时切换全局 AVAudioSession。Agent 指令到达时先恢复视频输出，
        // 否则 seek 成功了，后续画面在播，扬声器却仍停留在录音模式。
        let restartsPlayback = action == "replay" || action == "set_speed" ||
            (action == "seek_seconds" && seconds == 0)
        if !restartsPlayback && !active.restoreAudioForAgentCommand() { return false }
        if action == "seek_percent" {
            guard let percent, (0...100).contains(percent) else { return false }
            return active.seekPercent(percent / 100)
        }
        if action == "seek_seconds" {
            guard let seconds else { return false }
            return seconds == 0 ? active.replay() : active.seekSeconds(seconds)
        }
        if action == "replay" {
            return active.replay()
        }
        if action == "set_speed" {
            guard let speed else { return false }
            return active.setSpeed(speed)
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
    var vod: MaiCloudVodSession?
    var startPositionUs: Int64 = 0
    var onError: ((String) -> Void)?
    private var viewID: UInt64 = 0
    private var session: OpaquePointer?
    private var playbackSpeed = 1.0
    private var lastSize: CGSize = .zero
    private var audioStartUptime = 0.0
    private var audioStartBufferCount: UInt64 = 0
    private var didLogAudioOutput = false

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
            NSLog(
                "[FFplayDisplay] resize %@",
                """
                view=\(Int(bounds.width))x\(Int(bounds.height)) scale=\(contentScaleFactor) \
                pixels=\(Int(size.width))x\(Int(size.height))
                """
            )
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
        guard activatePlaybackAudioSession() else { return }
        lastSize = CGSize(width: max(1, bounds.width * contentScaleFactor),
                          height: max(1, bounds.height * contentScaleFactor))
        #if DEBUG
            NSLog(
                "[FFplayDisplay] attach %@",
                """
                view=\(Int(bounds.width))x\(Int(bounds.height)) contentScale=\(contentScaleFactor) \
                screenScale=\(window?.screen.scale ?? 0) pixels=\(Int(lastSize.width))x\(Int(lastSize.height))
                """
            )
        #endif
        viewID = maiGraphicsPresenterAttach(
            Unmanaged.passUnretained(layer).toOpaque(), UInt32(lastSize.width),
            UInt32(lastSize.height), maiFfplayRetainLayer, maiFfplayReleaseLayer
        )
        guard viewID != 0 else {
            onError?("Metal 视频视图无法创建")
            return
        }
        resetAudioObservation()
        session = startPlayback(at: startPositionUs)
        if session == nil {
            maiGraphicsPresenterDetach(viewID)
            viewID = 0
            onError?("FFplay 无法启动")
        }
    }

    private func activatePlaybackAudioSession() -> Bool {
        do {
            let audioSession = AVAudioSession.sharedInstance()
            try audioSession.setCategory(.playback, mode: .moviePlayback)
            try audioSession.setActive(true)
            AppDiagnosticLog.shared.record(level: .info, category: "ffplay",
                event: "audio-session-active", fields: [
                    "category": audioSession.category.rawValue,
                    "route": audioSession.currentRoute.outputs.map { $0.portType.rawValue }
                        .joined(separator: ","),
                    "volume": String(format: "%.2f", audioSession.outputVolume)
                ])
            return true
        } catch {
            onError?("无法启动视频声音：\(error.localizedDescription)")
            return false
        }
    }

    func restorePlaybackAudioSession() -> Bool {
        activatePlaybackAudioSession()
    }

    private func resetAudioObservation() {
        audioStartUptime = ProcessInfo.processInfo.systemUptime
        audioStartBufferCount = maiFfplayAppleNonSilentBufferCount()
        didLogAudioOutput = false
    }

    private func startPlayback(at position: Int64) -> OpaquePointer? {
        if let vod {
            return maiFfplayIosStartVodAtSpeed(viewID, vod.pointer,
                                               playbackSpeed, position)
        }
        return videoPath.withCString {
            maiFfplayIosStartAtSpeed(viewID, $0, playbackSpeed, position)
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

    func seekAbsoluteUs(_ position: Int64) -> Bool {
        guard let session else { return false }
        return maiFfplayIosSeekAbsoluteUs(session, position) == 1
    }

    func replayFromBeginning() -> Bool {
        guard viewID != 0 else { return false }
        if let previous = session {
            session = nil
            maiFfplayIosStop(previous)
        }
        guard activatePlaybackAudioSession() else { return false }
        startPositionUs = 0
        resetAudioObservation()
        session = startPlayback(at: 0)
        return session != nil
    }

    func setSpeed(_ speed: Double) -> Bool {
        guard [0.25, 0.5, 1.0, 1.5, 2.0].contains(speed), let previous = session,
              viewID != 0 else { return false }
        if speed == playbackSpeed { return true }
        let position = max(0, playbackStatus()?.position_us ?? 0)
        session = nil
        maiFfplayIosStop(previous)
        guard activatePlaybackAudioSession() else { return false }
        playbackSpeed = speed
        resetAudioObservation()
        session = startPlayback(at: position)
        return session != nil
    }

    func checkPlayback() {
        guard let session, maiFfplayIosHasFinished(session) == 1,
              maiFfplayIosExitCode(session) != 0 else { return }
        onError?("FFplay 无法播放此视频")
    }

    func playbackStatus() -> MaiFfplayPlaybackStatus? {
        guard session != nil else { return nil }
        var status = MaiFfplayPlaybackStatus()
        guard maiFfplayGetPlaybackStatus(&status) == 1 else { return nil }
        if !didLogAudioOutput &&
            ProcessInfo.processInfo.systemUptime - audioStartUptime >= 3 {
            didLogAudioOutput = true
            let audioSession = AVAudioSession.sharedInstance()
            AppDiagnosticLog.shared.record(level: .info, category: "ffplay",
                event: "audio-output-observed", fields: [
                    "non_silent_buffers": String(
                        maiFfplayAppleNonSilentBufferCount() - audioStartBufferCount),
                    "category": audioSession.category.rawValue,
                    "route": audioSession.currentRoute.outputs.map { $0.portType.rawValue }
                        .joined(separator: ","),
                    "volume": String(format: "%.2f", audioSession.outputVolume)
                ])
        }
        return status
    }

    var hasFirstFrame: Bool {
        guard let session else { return false }
        return maiFfplayIosHasFirstFrame(session) == 1
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
    private let vod: MaiCloudVodSession?
    private let startSeconds: Double
    private let entryDirection: Int
    private let embeddedInMediaPager: Bool
    private let onClose: () -> Void
    private var onNavigate: ((Int) -> Bool)?
    private var onPlaybackReady: (() -> Void)?
    var onPlaybackFailure: ((String) -> Void)?
    var controlsInteractionChanged: ((Bool) -> Void)?
    private let metalView = MaiFfplayMetalView()
    private let pauseButton = UIButton(type: .system)
    private let seekSlider = UISlider()
    private let timeLabel = UILabel()
    private let errorLabel = UILabel()
    private let saveButton = UIButton(type: .system)
    private let speedButton = UIButton(type: .system)
    private let speedChoices = UIStackView()
    private let saveStatusLabel = UILabel()
    private var playbackTimer: Timer?
    private var playing = true
    private var playbackSpeed = 1.0
    private var saving = false
    private var didAnimateEntry = false
    private var didReportPlaybackReady = false

    init(path: String, startSeconds: Double = 0,
         entryDirection: Int = 0, navigate: ((Int) -> Bool)? = nil,
         onPlaybackReady: (() -> Void)? = nil,
         vod: MaiCloudVodSession? = nil,
         embeddedInMediaPager: Bool = false,
         close: @escaping () -> Void) {
        self.path = path
        self.vod = vod
        self.startSeconds = startSeconds
        self.entryDirection = entryDirection
        self.embeddedInMediaPager = embeddedInMediaPager
        onNavigate = navigate
        self.onPlaybackReady = onPlaybackReady
        onClose = close
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { nil }

    func setNavigation(_ navigate: ((Int) -> Bool)?) { onNavigate = navigate }

    /// 初始化播放器承载视图、播放控件与进度观察。
    /// 视图仅创建一次；重播和跳转复用同一播放实例，避免重建导致音频状态丢失。
    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        metalView.videoPath = path
        metalView.vod = vod
        metalView.startPositionUs = Int64((startSeconds * 1_000_000).rounded())
        metalView.onError = { [weak self] message in
            self?.errorLabel.text = message
            self?.errorLabel.isHidden = false
            self?.onPlaybackFailure?(message)
        }
        metalView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(metalView)
        if onNavigate != nil {
            for direction in [UISwipeGestureRecognizer.Direction.left,
                              UISwipeGestureRecognizer.Direction.right] {
                let gesture = UISwipeGestureRecognizer(target: self,
                                                       action: #selector(videoSwiped(_:)))
                gesture.direction = direction
                metalView.addGestureRecognizer(gesture)
            }
        }

        errorLabel.textColor = .white
        errorLabel.textAlignment = .center
        errorLabel.isHidden = true
        errorLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(errorLabel)

        pauseButton.setImage(UIImage(systemName: "pause.fill"), for: .normal)
        pauseButton.accessibilityLabel = "暂停视频"
        pauseButton.accessibilityIdentifier = "ffplay-pause"
        pauseButton.tintColor = .white
        pauseButton.contentEdgeInsets = UIEdgeInsets(top: 14, left: 0, bottom: 0, right: 0)
        pauseButton.addTarget(self, action: #selector(togglePause), for: .touchUpInside)
        pauseButton.translatesAutoresizingMaskIntoConstraints = false
        pauseButton.widthAnchor.constraint(equalToConstant: 54).isActive = true
        pauseButton.heightAnchor.constraint(equalToConstant: 54).isActive = true
        seekSlider.minimumTrackTintColor = .white
        seekSlider.maximumTrackTintColor = UIColor.white.withAlphaComponent(0.35)
        seekSlider.thumbTintColor = .white
        seekSlider.addTarget(self, action: #selector(seekReleased), for: .touchUpInside)
        seekSlider.accessibilityIdentifier = "ffplay-seek"
        seekSlider.addTarget(self, action: #selector(seekReleased), for: .touchUpOutside)
        timeLabel.text = "0:00 / 0:00"
        timeLabel.accessibilityIdentifier = "ffplay-time"
        timeLabel.textColor = .white
        timeLabel.font = .monospacedDigitSystemFont(ofSize: 15, weight: .medium)
        let timeline = UIStackView(arrangedSubviews: [timeLabel, seekSlider])
        timeline.axis = .vertical
        timeline.spacing = 1
        let controls = UIStackView(arrangedSubviews: [pauseButton, timeline])
        controls.axis = .horizontal
        controls.alignment = .center
        controls.spacing = 12
        controls.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(controls)

        let shareButton = actionButton("square.and.arrow.up", label: "转发或分享",
                                       identifier: "ffplay-share", action: #selector(shareTapped))
        saveButton.setImage(UIImage(systemName: "square.and.arrow.down"), for: .normal)
        saveButton.accessibilityLabel = "保存到相册"
        saveButton.accessibilityIdentifier = "ffplay-save"
        saveButton.tintColor = .white
        saveButton.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        saveButton.layer.cornerRadius = 25
        saveButton.addTarget(self, action: #selector(saveTapped), for: .touchUpInside)
        saveButton.translatesAutoresizingMaskIntoConstraints = false
        saveButton.widthAnchor.constraint(equalToConstant: 50).isActive = true
        saveButton.heightAnchor.constraint(equalToConstant: 50).isActive = true
        speedButton.setTitle("1×", for: .normal)
        speedButton.setTitleColor(.white, for: .normal)
        speedButton.titleLabel?.font = .systemFont(ofSize: 16, weight: .semibold)
        speedButton.accessibilityLabel = "播放倍速 1.0×"
        speedButton.accessibilityIdentifier = "ffplay-speed"
        speedButton.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        speedButton.layer.cornerRadius = 25
        speedButton.addTarget(self, action: #selector(toggleSpeedChoices), for: .touchUpInside)
        speedButton.translatesAutoresizingMaskIntoConstraints = false
        speedButton.widthAnchor.constraint(equalToConstant: 56).isActive = true
        speedButton.heightAnchor.constraint(equalToConstant: 50).isActive = true

        let copyButton = actionButton("doc.on.doc", label: "复制文件路径",
                                      identifier: "ffplay-copy-path", action: #selector(copyPathTapped))
        if vod != nil {
            shareButton.isHidden = true
            saveButton.isHidden = true
            copyButton.isHidden = true
        }
        let actions = UIStackView(arrangedSubviews: [shareButton, saveButton, speedButton, copyButton])
        actions.axis = .horizontal
        actions.spacing = 12
        actions.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(actions)

        // 倍速直接在播放画面内选择，避免两层系统弹窗遮住视频。
        speedChoices.axis = .horizontal
        speedChoices.alignment = .fill
        speedChoices.distribution = .fillEqually
        speedChoices.spacing = 4
        speedChoices.backgroundColor = UIColor.black.withAlphaComponent(0.82)
        speedChoices.layer.cornerRadius = 18
        speedChoices.layoutMargins = UIEdgeInsets(top: 6, left: 6, bottom: 6, right: 6)
        speedChoices.isLayoutMarginsRelativeArrangement = true
        speedChoices.isHidden = true
        speedChoices.translatesAutoresizingMaskIntoConstraints = false
        for rate in [0.25, 0.5, 1.0, 1.5, 2.0] {
            let choice = UIButton(type: .system)
            choice.setTitle("\(rate)×", for: .normal)
            choice.setTitleColor(.white, for: .normal)
            choice.titleLabel?.font = .systemFont(ofSize: 14, weight: .semibold)
            choice.accessibilityIdentifier = "ffplay-speed-\(rate)"
            choice.tag = Int(rate * 100)
            choice.addTarget(self, action: #selector(speedChoiceTapped(_:)), for: .touchUpInside)
            speedChoices.addArrangedSubview(choice)
        }
        view.addSubview(speedChoices)

        saveStatusLabel.textColor = .white
        saveStatusLabel.font = .systemFont(ofSize: 14, weight: .medium)
        saveStatusLabel.textAlignment = .center
        saveStatusLabel.numberOfLines = 2
        saveStatusLabel.accessibilityIdentifier = "ffplay-save-status"
        saveStatusLabel.backgroundColor = UIColor.black.withAlphaComponent(0.7)
        saveStatusLabel.layer.cornerRadius = 10
        saveStatusLabel.clipsToBounds = true
        saveStatusLabel.isHidden = true
        saveStatusLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(saveStatusLabel)

        let closeButton = UIButton(type: .system)
        closeButton.setImage(UIImage(systemName: "xmark"), for: .normal)
        closeButton.accessibilityIdentifier = "ffplay-close"
        closeButton.accessibilityLabel = "关闭视频"
        closeButton.isHidden = embeddedInMediaPager
        closeButton.tintColor = .white
        closeButton.backgroundColor = UIColor.white.withAlphaComponent(0.2)
        closeButton.layer.cornerRadius = 25
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
            controls.bottomAnchor.constraint(equalTo: actions.topAnchor, constant: -4),
            actions.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -18),
            actions.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor,
                                           constant: -4),
            speedChoices.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 16),
            speedChoices.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -18),
            speedChoices.heightAnchor.constraint(equalToConstant: 48),
            speedChoices.bottomAnchor.constraint(equalTo: controls.topAnchor, constant: -10),
            saveStatusLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            saveStatusLabel.bottomAnchor.constraint(equalTo: controls.topAnchor, constant: -18),
            saveStatusLabel.widthAnchor.constraint(greaterThanOrEqualToConstant: 160),
            saveStatusLabel.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor,
                                                  constant: -32),
            saveStatusLabel.heightAnchor.constraint(equalToConstant: 40),
            closeButton.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 18),
            closeButton.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor,
                                             constant: 12),
            closeButton.widthAnchor.constraint(equalToConstant: 50),
            closeButton.heightAnchor.constraint(equalToConstant: 50),
        ])
        if embeddedInMediaPager { observeControlTouches(in: view) }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if !didAnimateEntry && entryDirection != 0 {
            didAnimateEntry = true
            view.transform = CGAffineTransform(translationX: CGFloat(entryDirection * 48), y: 0)
            view.alpha = 0.48
            UIView.animate(withDuration: 0.22, delay: 0,
                           options: [.curveEaseOut, .allowUserInteraction]) {
                self.view.transform = .identity
                self.view.alpha = 1
            }
        }
        activateForVisibleHost()
    }

    // A selected page can embed this controller after its parent appeared.
    // Match the standalone SwiftUI host's ownership without starting neighbors.
    func activateForVisibleHost() {
        MaiLocalMusicPlayer.shared.stop()
        if let previous = MaiFfplayMobilePlayer.active, previous !== self {
            previous.stop()
        }
        MaiFfplayMobilePlayer.active = self
        if playbackTimer == nil {
            playbackTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) {
                [weak self] _ in
                Task { @MainActor [weak self] in
                    self?.refreshPlayback()
                }
            }
        }
        refreshPlayback()
    }

    @objc private func videoSwiped(_ gesture: UISwipeGestureRecognizer) {
        let direction = gesture.direction == .left ? 1 : -1
        if onNavigate?(direction) == false {
            showStatus(direction > 0 ? "已经是最后一个视频" : "已经是第一个视频")
        }
    }

    private func refreshPlayback() {
        metalView.checkPlayback()
        if !didReportPlaybackReady && metalView.hasFirstFrame {
            didReportPlaybackReady = true
            onPlaybackReady?()
        }
        guard let status = metalView.playbackStatus(), status.duration_us > 0 else { return }
        let position = min(max(status.position_us, 0), status.duration_us)
        if !seekSlider.isTracking {
            seekSlider.value = Float(Double(position) / Double(status.duration_us))
        }
        timeLabel.text = "\(Self.timestamp(position)) / \(Self.timestamp(status.duration_us))"
        playing = status.paused == 0
        playbackSpeed = Double(status.speed_percent) / 100.0
        speedButton.setTitle("\(playbackSpeed)×", for: .normal)
        speedButton.accessibilityLabel = "播放倍速 \(playbackSpeed)×"
        refreshPlayButton()
    }

    private static func timestamp(_ microseconds: Int64) -> String {
        let seconds = max(0, microseconds / 1_000_000)
        return String(format: "%lld:%02lld", seconds / 60, seconds % 60)
    }

    func stop() {
        playbackTimer?.invalidate()
        playbackTimer = nil
        metalView.stop()
        controlsInteractionChanged?(false)
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
            refreshPlayButton()
        }
        return accepted
    }

    func seekPercent(_ fraction: Double) -> Bool {
        metalView.seekPercent(fraction)
    }

    func seekSeconds(_ seconds: Double) -> Bool {
        guard seconds.isFinite, seconds >= 0,
              let status = metalView.playbackStatus(), status.duration_us > 0,
              seconds * 1_000_000 <= Double(status.duration_us) else { return false }
        return metalView.seekAbsoluteUs(Int64((seconds * 1_000_000).rounded()))
    }

    func replay() -> Bool {
        guard metalView.replayFromBeginning() else { return false }
        playing = true
        seekSlider.value = 0
        refreshPlayButton()
        return true
    }

    func restoreAudioForAgentCommand() -> Bool {
        metalView.restorePlaybackAudioSession()
    }

    func setSpeed(_ speed: Double) -> Bool {
        guard metalView.setSpeed(speed) else { return false }
        playbackSpeed = speed
        speedButton.setTitle("\(speed)×", for: .normal)
        speedButton.accessibilityLabel = "播放倍速 \(speed)×"
        playing = true
        refreshPlayButton()
        return true
    }

    func close() { onClose() }

    private func observeControlTouches(in parent: UIView) {
        if let control = parent as? UIControl {
            control.addTarget(self, action: #selector(controlTouchBegan), for: .touchDown)
            control.addTarget(self, action: #selector(controlTouchEnded),
                              for: [.touchUpInside, .touchUpOutside, .touchCancel])
        }
        parent.subviews.forEach { observeControlTouches(in: $0) }
    }

    @objc private func controlTouchBegan() { controlsInteractionChanged?(true) }
    @objc private func controlTouchEnded() { controlsInteractionChanged?(false) }

    @objc private func togglePause() { _ = sendCommand("toggle_pause") }
    @objc private func seekReleased() { _ = seekPercent(Double(seekSlider.value)) }
    @objc private func closeTapped() { close() }

    private func refreshPlayButton() {
        pauseButton.setImage(UIImage(systemName: playing ? "pause.fill" : "play.fill"), for: .normal)
        pauseButton.accessibilityLabel = playing ? "暂停视频" : "播放视频"
    }

    private func actionButton(_ symbol: String, label: String, identifier: String,
                              action: Selector) -> UIButton {
        let button = UIButton(type: .system)
        button.setImage(UIImage(systemName: symbol), for: .normal)
        button.accessibilityLabel = label
        button.accessibilityIdentifier = identifier
        button.tintColor = .white
        button.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        button.layer.cornerRadius = 25
        button.addTarget(self, action: action, for: .touchUpInside)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.widthAnchor.constraint(equalToConstant: 50).isActive = true
        button.heightAnchor.constraint(equalToConstant: 50).isActive = true
        return button
    }

    private func showStatus(_ message: String) {
        saveStatusLabel.text = "  \(message)  "
        saveStatusLabel.accessibilityLabel = message
        saveStatusLabel.isHidden = false
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { [weak self] in
            self?.saveStatusLabel.isHidden = true
        }
    }

    @objc private func shareTapped() {
        if vod != nil { showStatus("云盘视频正在点播，无法直接转发") ; return }
        let sheet = UIActivityViewController(activityItems: [URL(fileURLWithPath: path)],
                                             applicationActivities: nil)
        sheet.popoverPresentationController?.sourceView = view
        sheet.popoverPresentationController?.sourceRect = CGRect(
            x: view.bounds.midX, y: view.bounds.maxY - 50, width: 1, height: 1)
        present(sheet, animated: true)
    }

    @objc private func saveTapped() {
        if vod != nil { showStatus("云盘视频正在点播，未下载到相册") ; return }
        guard !saving else { return }
        saving = true
        saveButton.isEnabled = false
        let requestedAt = Date()
        let fileURL = URL(fileURLWithPath: path)
        Task { @MainActor in
            defer {
                saving = false
                saveButton.isEnabled = true
            }
            do {
                // 先检查真实文件和视频轨道；无效文件不应进入 Photos 的变更块。
                let values = try fileURL.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
                guard values.isRegularFile == true, (values.fileSize ?? 0) > 0,
                      !(try await AVURLAsset(url: fileURL).loadTracks(withMediaType: .video)).isEmpty else {
                    showStatus("视频文件不可用，无法保存")
                    return
                }
                // 保存只需要向相册添加作品，无需为播放器请求整个图库的读取权限。
                guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
                    showStatus("请允许添加视频到相册")
                    return
                }
                try await MaiPhotoLibraryMediaWriter.saveVideo(at: fileURL, createdAt: requestedAt)
                showStatus("已保存到相册")
            } catch {
                showStatus("保存失败：\(error.localizedDescription)")
            }
        }
    }

    @objc private func toggleSpeedChoices() {
        speedChoices.isHidden.toggle()
    }

    @objc private func speedChoiceTapped(_ sender: UIButton) {
        speedChoices.isHidden = true
        if !setSpeed(Double(sender.tag) / 100) { showStatus("倍速切换失败") }
    }

    @objc private func copyPathTapped() {
        if vod != nil { showStatus("云盘视频没有本地文件路径") ; return }
        UIPasteboard.general.string = path
        showStatus("已复制文件路径")
    }
}

struct MaiFfplayVideoScreen: UIViewControllerRepresentable {
    let path: String
    let vod: MaiCloudVodSession?
    let startSeconds: Double
    let entryDirection: Int
    let navigate: ((Int) -> Bool)?
    let onPlaybackReady: (() -> Void)?
    let close: () -> Void

    init(path: String, startSeconds: Double = 0,
         entryDirection: Int = 0, navigate: ((Int) -> Bool)? = nil,
         onPlaybackReady: (() -> Void)? = nil,
         vod: MaiCloudVodSession? = nil,
         close: @escaping () -> Void) {
        self.path = path
        self.vod = vod
        self.startSeconds = startSeconds
        self.entryDirection = entryDirection
        self.navigate = navigate
        self.onPlaybackReady = onPlaybackReady
        self.close = close
    }

    func makeUIViewController(context _: Context) -> MaiFfplayVideoController {
        MaiLocalMusicPlayer.shared.stop()
        let controller = MaiFfplayVideoController(
            path: path, startSeconds: startSeconds,
            entryDirection: entryDirection, navigate: navigate,
            onPlaybackReady: onPlaybackReady, vod: vod, close: close)
        // 嵌入式播放器不一定触发全屏控制器的 viewDidAppear；创建时先绑定 Agent 工具。
        if let previous = MaiFfplayMobilePlayer.active, previous !== controller {
            previous.stop()
        }
        MaiFfplayMobilePlayer.active = controller
        return controller
    }

    func updateUIViewController(_ controller: MaiFfplayVideoController, context _: Context) {
        controller.setNavigation(navigate)
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
                        .ignoresSafeArea()
                        .statusBarHidden()
                }
            }
            .task { showingVideo = true }
    }
}

struct MaiFfplaySwipeUITestRoot: View {
    @State private var showingVideo = false
    @State private var index = 0

    var body: some View {
        Color.black
            .fullScreenCover(isPresented: $showingVideo) {
                if let first = Bundle.main.url(forResource: "ffplay-sample",
                                               withExtension: "mp4"),
                   let second = Bundle.main.url(forResource: "ffplay-seek-sample",
                                                withExtension: "mp4") {
                    let paths = [first.path, second.path]
                    MaiFfplayVideoScreen(path: paths[index], navigate: { direction in
                        guard paths.indices.contains(index + direction) else { return false }
                        index += direction
                        return true
                    }) {
                        showingVideo = false
                    }
                    .id(index)
                    .ignoresSafeArea()
                    .statusBarHidden()
                    .overlay(alignment: .top) {
                        Text(URL(fileURLWithPath: paths[index]).lastPathComponent)
                            .accessibilityIdentifier("ffplay-current-video")
                            .foregroundStyle(.white)
                            .padding(.top, 70)
                    }
                }
            }
            .task { showingVideo = true }
    }
}

struct MaiMixedGalleryUITestRoot: View {
    @State private var video: MaiFfplayPresentation?
    @State private var image: MaiMediaSequence?
    @State private var direction = 0

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let video {
                MaiFfplayVideoScreen(path: video.path, navigate: navigate) {
                    self.video = nil
                }
                .id(video.id)
                .ignoresSafeArea()
                .overlay(alignment: .top) {
                    Text("\(video.index + 1)/\(video.sequence.items.count) · " +
                         URL(fileURLWithPath: video.path).lastPathComponent)
                        .foregroundStyle(.white).padding(.top, 70)
                }
            }
            if let image {
                MaiGalleryImageScreen(sequence: image, entryDirection: direction,
                                      navigate: navigate) {
                    self.image = nil
                }
                .id(image.current.path)
            }
        }
        .statusBarHidden()
        .task {
            guard let first = Bundle.main.url(forResource: "ffplay-seek-sample",
                                                  withExtension: "mp4"),
                  let last = Bundle.main.url(forResource: "ffplay-sample",
                                                 withExtension: "mp4") else { return }
            let sample = UIGraphicsImageRenderer(size: CGSize(width: 300, height: 500))
                .image { context in
                    UIColor.systemOrange.setFill()
                    context.fill(CGRect(x: 0, y: 0, width: 300, height: 500))
                }
            let imageURL = FileManager.default.temporaryDirectory
                .appendingPathComponent("mixed-gallery-sample.png")
            guard let data = sample.pngData(),
                  (try? data.write(to: imageURL)) != nil else { return }
            let selected = MaiMediaItem(kind: .video, path: first.path)
            let sequence = MaiMediaSequence(selected: selected, items: [
                selected,
                MaiMediaItem(kind: .image, path: imageURL.path),
                MaiMediaItem(kind: .video, path: last.path)
            ])
            video = MaiFfplayPresentation(sequence: sequence)
        }
    }

    private func navigate(_ direction: Int) -> Bool {
        guard let next = (video?.sequence ?? image)?.adjacent(direction) else { return false }
        self.direction = direction
        if next.current.kind == .video {
            image = nil
            video = MaiFfplayPresentation(sequence: next, entryDirection: direction)
        } else {
            video = nil
            image = next
        }
        return true
    }
}

struct MaiFfplaySeekUITestRoot: View {
    let startSeconds: Double
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var showingVideo = false
    @State private var commandResult = "尚未定位"

    var body: some View {
        Color.black
            .fullScreenCover(isPresented: $showingVideo) {
                if let file = Bundle.main.url(forResource: "ffplay-seek-sample", withExtension: "mp4") {
                    MaiFfplayVideoScreen(path: file.path, startSeconds: startSeconds) {
                        showingVideo = false
                    }
                    .ignoresSafeArea()
                    .statusBarHidden()
                    .overlay(alignment: .topTrailing) {
                        Button(commandResult) {
                            Task { @MainActor in
                                let result = await AIMobileHostToolProvider.shared.execute(
                                    name: "maichat_video_command",
                                    argumentsJSON: #"{"action":"seek_seconds","seconds":6}"#)
                                commandResult = result.errorCode == nil ? "定位已受理" : "定位失败"
                            }
                        }
                        .accessibilityIdentifier("agent-seek-six-seconds")
                        .padding(.top, 72)
                        .padding(.trailing, 16)
                    }
                }
            }
            .task {
                AIMobileHostToolProvider.shared.appState = appState
                showingVideo = true
            }
    }
}
#endif

#if DEBUG
struct MaiFfplayAgentDockUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var commandResult = "准备打开视频"

    var body: some View {
        GeometryReader { geometry in
            VStack(spacing: 16) {
                if let presentation = appState.agentVideoPresentation {
                    MaiFfplayVideoScreen(path: presentation.path,
                                         startSeconds: presentation.startSeconds) {
                        appState.agentVideoPresentation = nil
                    }
                    .frame(height: geometry.size.height * 0.57)
                }
                HStack {
                    Button("Agent 重播") {
                        Task { await command(#"{"action":"replay"}"#) }
                    }
                    .accessibilityIdentifier("agent-replay")
                    Button("Agent 从开头播放") {
                        Task { await command(#"{"action":"seek_seconds","seconds":0}"#) }
                    }
                    .accessibilityIdentifier("agent-start-over")
                }
                HStack {
                    Button("Agent 0.5×") {
                        Task { await command(#"{"action":"set_speed","speed":0.5}"#) }
                    }
                    .accessibilityIdentifier("agent-speed-half")
                    Button("Agent 2×") {
                        Task { await command(#"{"action":"set_speed","speed":2}"#) }
                    }
                    .accessibilityIdentifier("agent-speed-double")
                }
                Text(commandResult).accessibilityIdentifier("agent-command-result")
                Spacer()
            }
            .frame(maxWidth: .infinity)
        }
        .background(.black)
        .foregroundStyle(.white)
        .task { await openVideoThroughAgentTool() }
    }

    private func command(_ arguments: String) async {
        let result = await AIMobileHostToolProvider.shared.execute(
            name: "maichat_video_command", argumentsJSON: arguments)
        commandResult = result.errorCode == nil ? "Agent 指令已执行" :
            "Agent 指令失败：\(result.errorCode ?? "unknown")"
    }

    private func openVideoThroughAgentTool() async {
        AppDiagnosticLog.shared.install()
        AIMobileHostToolProvider.shared.appState = appState
        if ProcessInfo.processInfo.arguments.contains("--ffplay-agent-real-video-test") {
            let support = FileManager.default.urls(for: .applicationSupportDirectory,
                                                   in: .userDomainMask)[0]
            let workspace = support.appendingPathComponent("AIAssistant/Workspace",
                                                           isDirectory: true)
            let video = workspace.appendingPathComponent("out5-nowm.mp4")
            guard FileManager.default.fileExists(atPath: video.path) else {
                commandResult = "实际测试视频不存在"
                return
            }
            AIAssistantModel.shared.installVideoToolTestWorkspace(workspace.path)
            let result = await AIMobileHostToolProvider.shared.execute(
                name: "maichat_play_video", argumentsJSON: #"{"path":"out5-nowm.mp4"}"#)
            commandResult = result.errorCode == nil ? "Agent 已打开实际视频" :
                "Agent 打开失败：\(result.errorCode ?? "unknown")"
            return
        }
        guard let sample = Bundle.main.url(forResource: "ffplay-seek-sample",
                                            withExtension: "mp4") else {
            commandResult = "测试视频不存在"
            return
        }
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("ffplay-agent-dock-test", isDirectory: true)
        let destination = workspace.appendingPathComponent(sample.lastPathComponent)
        do {
            try FileManager.default.createDirectory(at: workspace,
                                                    withIntermediateDirectories: true)
            if !FileManager.default.fileExists(atPath: destination.path) {
                try FileManager.default.copyItem(at: sample, to: destination)
            }
        } catch {
            commandResult = "无法准备测试视频：\(error.localizedDescription)"
            return
        }
        AIAssistantModel.shared.installVideoToolTestWorkspace(workspace.path)
        let result = await AIMobileHostToolProvider.shared.execute(
            name: "maichat_play_video",
            argumentsJSON: #"{"path":"ffplay-seek-sample.mp4"}"#)
        commandResult = result.errorCode == nil ? "Agent 已打开视频" :
            "Agent 打开失败：\(result.errorCode ?? "unknown")"
    }
}
#endif
