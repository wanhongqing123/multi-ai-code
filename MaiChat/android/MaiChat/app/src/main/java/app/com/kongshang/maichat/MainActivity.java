// 文件作用：Android 应用启动、账户与主导航；MainActivity 负责页面生命周期、事件分发和界面呈现，本文件给出其实现。
// 类型关系：MainActivity extends Activity implements RemoteIMSessionController.Listener；审阅时可据此追踪父类、接口或回调契约。
// 主要入口：GalleryItem、onCreate、onResume、onPause。
// 直接依赖：com.kongshang.maichat.tools.MaiChatHostTools；实现与这些接口协作。
package com.kongshang.maichat;

import com.kongshang.maichat.tools.MaiChatHostTools;
import com.kongshang.maichat.tools.MobileScreenCaptureService;
import com.google.zxing.BarcodeFormat;
import com.google.zxing.qrcode.QRCodeWriter;
import com.google.zxing.common.BitMatrix;

import android.annotation.SuppressLint;
import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.media.MediaScannerConnection;
import android.media.projection.MediaProjectionManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.MediaStore;
import android.text.Editable;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.util.Log;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.webkit.WebView;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.ScrollView;
import android.widget.Space;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.FileProvider;
import androidx.core.app.NotificationCompat;
import androidx.core.app.NotificationManagerCompat;

import com.tencent.rtmp.ui.TXCloudVideoView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

public final class MainActivity extends Activity implements RemoteIMSessionController.Listener {
    public interface SocialCredentialCallback {
        void onCredential(String provider, String credential);
        void onCancelled(String provider);
        void onFailure(String provider, String message);
    }

    public static final class GalleryItem {
        public final String path;
        public final boolean video;
        public GalleryItem(String path, boolean video) {
            this.path = path;
            this.video = video;
        }
    }
    private static final int REQUEST_PICK_IMAGE = 1001;
    private static final int REQUEST_RECORD_AUDIO = 1002;
    private static final int REQUEST_CAMERA_PERMISSION = 1003;
    private static final int REQUEST_TAKE_PHOTO = 1004;
    private static final int REQUEST_PICK_FILE = 1005;
    private static final int REQUEST_POST_NOTIFICATIONS = 1006;
    private static final int REQUEST_GALLERY_ACCESS = 1008;
    private static final int REQUEST_SAVE_VIDEO_PERMISSION = 1009;
    private static final int REQUEST_SAVE_IMAGE_PERMISSION = 1010;
    private static final int REQUEST_FRIEND_SCAN = 1011;
    private static final int REQUEST_SCREEN_SHARE = 1012;
    private static final int REQUEST_CLOUD_DRIVE_MEDIA = 1013;
    private static final String TAG = "MaiChat.notify";
    private static final String MESSAGE_CHANNEL_ID = "maichat-new-messages";
    private static final String MESSAGE_GROUP_KEY = "maichat-private-messages";
    private static final String EXTRA_NOTIFICATION_PEER = "notification-peer-user-id";

    private RemoteIMSessionController session;
    private MaiChatHostTools maiChatHostTools;
    private Consumer<Boolean> pendingScreenShare;
    private RemoteIMMediaStore mediaStore;
    private AIAssistantPanel aiAssistant;
    private Dialog videoDialog;
    private final Map<Dialog, MaiCloudVod.Session> cloudPlayerStreams = new HashMap<>();
    private String cloudPlayerOwner = "";
    private final Map<Dialog, File> cloudImageFiles = new HashMap<>();
    private String cloudImageOwner = "";
    private MaiCloudDriveDialog cloudDriveDialog;
    private TextView videoTranscriptOverlay;
    private boolean videoExpanded;
    private Dialog imageDialog;
    private MaiGraphicsImageView galleryImageView;
    private String galleryImagePath;
    private String pendingVideoSavePath;
    private Runnable pendingVideoSaveCompletion;
    private String pendingImageSavePath;
    private Runnable pendingGalleryAction;
    private RemoteIMTab activeTab = RemoteIMTab.MESSAGES;
    private LinearLayout root;
    private LinearLayout content;
    private LinearLayout landscapeConversationPane;
    private View landscapeConversationDivider;
    private boolean renderedLandscapeIm;
    private View chatBackdrop;
    private RemoteIMTab returnTab = RemoteIMTab.MESSAGES;
    private ChatSwipeBack swipeBack;
    private FrameLayout insetsHost;
    private boolean imeAnimating;
    private androidx.core.view.WindowInsetsCompat latestInsets;
    private MainThreadMonitor diagnostics;
    private boolean pendingStateRefresh;
    private GrowingMessageEditText messageInput;
    private View attachmentButton;
    private TextView sendButton;
    private final Map<String, String> draftsByPeer = new HashMap<>();
    private String displayedOwner = "";
    private String activeChatUserId;
    private String activeFriendProfileUserId;
    private String draftText = "";
    private List<String> aiReplySuggestions = new ArrayList<>();
    private boolean aiReplySuggestionsLoading;
    private String aiReplySuggestionsPeer = "";
    private String aiReplySuggestionsLatestId = "";
    private RemoteIMQuote pendingQuote;
    private String messageSearchTargetId;
    private boolean stickToLatestMessage = true;
    private ChatMessageList currentMessageList;
    private String renderedChatUserId;
    private View chatEmptyState;
    private View chatComposer;
    private boolean renderedVoiceMode;
    private RemoteIMQuote renderedQuote;
    private boolean keyboardVisible;
    private boolean allowKeyboardLocation = true;
    private boolean browsingHistory;
    private boolean waitingForChatEntry;
    private Map<String, RemoteIMApprovalDisplayPolicy.State> visibleApprovalStates = Collections.emptyMap();
    private volatile long presentationRequest;
    private int messageWindowSize = 20;
    private String messageWindowStartId;

    private boolean voiceMode;
    private boolean holdingVoice;
    private long voiceUiGeneration;
    private String speechText = "";
    private TextView speechPreview;
    private boolean assistantVoiceActive;
    private String assistantVoiceText = "";
    private VoiceRecordingController voiceRecorder;
    private final ExecutorService mediaExecutor = Executors.newSingleThreadExecutor();
    private SendTarget pendingAttachmentTarget;
    private SpeechRecognizer speechRecognizer;
    private boolean cancelRecording;
    private VoicePlaybackController voicePlayback;
    private String playingMessageId;
    private File pendingCameraFile;
    private volatile boolean destroyed;
    private boolean showInitialLogin;
    private boolean loginSubmitting;
    private String loginError = "";
    private String loginUserDraft = "";
    private String loginCodeDraft = "";
    private String loginPasswordDraft = "";
    private String loginChallengeId = "";
    private boolean loginUsesPassword;
    private boolean loginAcceptedTerms;
    private boolean loginConfirmedAge;
    private boolean socialAuthInFlight;
    private boolean socialBindingVisible;
    private boolean socialSubmitting;
    private String socialProvider = "";
    private String socialBindingToken = "";
    private String socialPhoneDraft = "";
    private String socialCodeDraft = "";
    private String socialChallengeId = "";
    private String socialBindingError = "";
    private long socialBindingExpiresAt;
    private TextView socialBindingCountdown;
    private final Handler socialUiHandler = new Handler(Looper.getMainLooper());
    private final Runnable socialBindingTick = new Runnable() {
        @Override public void run() {
            if (destroyed || !socialBindingVisible) return;
            long seconds = Math.max(0,
                (socialBindingExpiresAt - SystemClock.elapsedRealtime() + 999) / 1000);
            if (seconds == 0) {
                socialBindingError = "授权已过期，请返回重新授权";
                render();
                return;
            }
            if (socialBindingCountdown != null)
                socialBindingCountdown.setText(String.format(Locale.ROOT,
                    "授权剩余 %02d:%02d", seconds / 60, seconds % 60));
            socialUiHandler.postDelayed(this, 1000);
        }
    };
    private static final String EXTRA_LOCAL_SOCIAL_PROVIDER = "maichat.local.social_provider";
    private static final String EXTRA_LOCAL_SOCIAL_CREDENTIAL = "maichat.local.social_credential";
    private final SocialCredentialCallback socialCredentialCallback = new SocialCredentialCallback() {
        @Override public void onCredential(String provider, String credential) {
            runOnUiThread(() -> beginSocialLogin(provider, credential));
        }

        @Override public void onCancelled(String provider) {
            runOnUiThread(() -> {
                loginError = "已取消" + socialProviderName(provider) + "授权";
                render();
            });
        }

        @Override public void onFailure(String provider, String message) {
            runOnUiThread(() -> {
                loginError = message == null || message.isEmpty()
                    ? socialProviderName(provider) + "授权失败，请重试" : message;
                render();
            });
        }
    };
    private static final String TERMS_URL = "https://ichat.life/legal/terms.html";
    private static final String PRIVACY_URL = "https://ichat.life/legal/privacy.html";
    private static final String CHILDREN_URL = "https://ichat.life/legal/children.html";
    private LinearLayout currentConversationRows;
    private long renderedConversationFingerprint;
    private String pendingNotificationPeerUserId = "";
    private volatile boolean activityInForeground;
    private final Set<String> collapsedContactGroups = new HashSet<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        MarkdownRenderer.initialize(getApplicationContext());
        diagnostics = new MainThreadMonitor(getWindow(), new File(getFilesDir(), "diagnostics"));
        getWindow().setStatusBarColor(Color.WHITE);
        getWindow().setNavigationBarColor(Color.WHITE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        androidx.core.view.WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        new androidx.core.view.WindowInsetsControllerCompat(getWindow(), getWindow().getDecorView())
                .setAppearanceLightNavigationBars(true);
        // 发出的媒体同样落持久目录，不放缓存：路径会进聊天记录并被长期引用。
        mediaStore = new RemoteIMMediaStore(RemoteIMMediaPaths.forApp(this));
        voiceRecorder = new VoiceRecordingController(mediaStore, diagnostics::record);
        voicePlayback = new VoicePlaybackController();
        // 普通 IM 录音保留语音消息回退；创作中心使用账号签发的实时 ASR 地址。
        speechRecognizer = null;
        session = new RemoteIMSessionController(this, this);
        maiChatHostTools = new MaiChatHostTools(this, session);
        AIAssistantController.shared(this).setHostToolHandler(maiChatHostTools::execute);
        createMessageNotificationChannel();
        showInitialLogin = session.requiresLogin();
        render();
        handleNotificationIntent(getIntent());
        handleLocalSocialIntent(getIntent());
        if (!session.requiresLogin()) requestNotificationPermissionIfNeeded();
    }

    @Override
    protected void onResume() {
        super.onResume();
        MobileScreenCaptureService.setAppForeground(true);
        activityInForeground = true;
        if (session != null) session.setAppForeground(true);
        if (socialBindingVisible && socialBindingExpired()) {
            socialBindingError = "授权已过期，请返回重新授权";
            render();
        } else if (socialBindingVisible) {
            socialUiHandler.removeCallbacks(socialBindingTick);
            socialUiHandler.post(socialBindingTick);
        }
        if (aiAssistant != null && aiAssistant.isAttachedToWindow()) aiAssistant.setForeground(true);
        if (diagnostics != null) diagnostics.foreground(true);
        if (pendingStateRefresh) { pendingStateRefresh = false; onStateChanged(); }
    }

    @Override
    public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        if (session != null && !destroyed) {
            ChatMessageList previous = currentMessageList;
            String anchor = previous == null ? null : previous.firstVisibleMessageId();
            int offset = previous == null ? 0 : previous.firstVisibleOffset();
            rememberDraft();
            render();
            if (previous != currentMessageList && currentMessageList != null && anchor != null)
                currentMessageList.restoreMessage(anchor, offset);
        }
        if (insetsHost != null) insetsHost.post(this::updateVideoDockLayout);
    }

    @Override
    protected void onPause() {
        MobileScreenCaptureService.setAppForeground(false);
        activityInForeground = false;
        socialUiHandler.removeCallbacks(socialBindingTick);
        if (session != null) session.setAppForeground(false);
        if (aiAssistant != null) aiAssistant.setForeground(false);
        allowKeyboardLocation = false;
        if (diagnostics != null) diagnostics.foreground(false);
        if (session != null) session.stopHumanTyping();
        cancelVoiceRecording();
        super.onPause();
    }

    public boolean isHostToolForeground() {
        return activityInForeground && !destroyed;
    }

    public boolean playLocalMusicFileForAgent(File file) {
        return isHostToolForeground() && activeTab == RemoteIMTab.ASSISTANT &&
            aiAssistant != null && aiAssistant.playLocalMusic(file);
    }

    public void selectLocalMusicForAgent(Consumer<String> completed) {
        if (!isHostToolForeground() || activeTab != RemoteIMTab.ASSISTANT || aiAssistant == null) {
            completed.accept(null); return;
        }
        aiAssistant.selectMusicForAgent(completed);
    }

    public void takePhotoForAgent(Consumer<String> completed) {
        if (!isHostToolForeground() || activeTab != RemoteIMTab.ASSISTANT || aiAssistant == null) {
            completed.accept(null); return;
        }
        aiAssistant.takePhotoForAgent(completed);
    }

    public void cancelAgentPhotoRequest() {
        if (aiAssistant != null) aiAssistant.cancelAgentPhotoRequest();
    }

    public void cancelAgentMusicRequest() {
        if (aiAssistant != null) aiAssistant.cancelAgentMusicRequest();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleNotificationIntent(intent);
        handleLocalSocialIntent(intent);
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        if (cloudDriveDialog != null && cloudDriveDialog.isShowing()) cloudDriveDialog.dismiss();
        socialUiHandler.removeCallbacks(socialBindingTick);
        pendingGalleryAction = null;
        if (videoDialog != null) videoDialog.dismiss();
        if (imageDialog != null) imageDialog.dismiss();
        if (aiAssistant != null) aiAssistant.cancelPendingAgentLocalMedia();
        if (aiAssistant != null) aiAssistant.closeLocalMusic();
        if (maiChatHostTools != null) maiChatHostTools.onDestroy();
        AIAssistantController.shared(this).setHostToolHandler(null);
        if (swipeBack != null) swipeBack.dispose();
        if (diagnostics != null) diagnostics.close();
        stopAudioPlayback();
        cancelVoiceRecording();
        mediaExecutor.shutdown();
        if (voiceRecorder != null) voiceRecorder.close();
        if (voicePlayback != null) voicePlayback.close();
        if (speechRecognizer != null) speechRecognizer.close();
        if (session != null) session.destroy();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (videoDialog != null) {
            videoDialog.dismiss();
            return;
        }
        if (socialBindingVisible) {
            clearSocialBinding();
            render();
            return;
        }
        if (activeFriendProfileUserId != null) {
            activeFriendProfileUserId = null;
            render();
            return;
        }
        if (activeChatUserId != null) {
            if (renderedLandscapeIm) finishChatBack();
            else if (swipeBack != null) swipeBack.goBack();
            return;
        }
        if (activeTab == RemoteIMTab.REMOTE && session.remoteDesktop().isActive()) {
            session.remoteDesktop().stop();
            render();
            return;
        }
        if (activeTab != RemoteIMTab.MESSAGES && !session.requiresLogin()) {
            activeTab = RemoteIMTab.MESSAGES;
            render();
            return;
        }
        super.onBackPressed();
    }

    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        return swipeBack == null ? super.dispatchTouchEvent(event)
            : swipeBack.dispatch(event, value -> super.dispatchTouchEvent(value));
    }

    private void finishChatBack() {
        if (activeChatUserId == null) return;
        rememberDraft();
        session.setConversationVisible(activeChatUserId, false);
        activeChatUserId = null; messageSearchTargetId = null; pendingQuote = null;
        activeTab = returnTab; chatBackdrop = null; presentationRequest++;
        render();
    }

    @Override
    public void onStateChanged() {
        if (destroyed) return;
        if (!activityInForeground) { pendingStateRefresh = true; return; }
        runOnUiThread(() -> {
            if (loginSubmitting) {
                if (session.connectionState() == TencentIMClient.ConnectionState.CONNECTED) {
                    loginSubmitting = false;
                    showInitialLogin = false;
                    loginError = "";
                    requestNotificationPermissionIfNeeded();
                    openPendingNotificationConversationIfPossible();
                } else if (session.connectionState() == TencentIMClient.ConnectionState.FAILED) {
                    loginSubmitting = false;
                    showInitialLogin = true;
                    loginError = session.connectionDetail();
                }
            }
            if (!session.isRestoringAccount() && session.requiresLogin()) {
                if (!showInitialLogin) {
                    loginAcceptedTerms = false;
                    loginConfirmedAge = false;
                }
                showInitialLogin = true;
            } else if (!session.isRestoringAccount() && !loginSubmitting) {
                showInitialLogin = false;
            }
            renderPreservingInput();
        });
    }

    @Override
    public void onError(String message) {
        if (destroyed) return;
        runOnUiThread(() -> {
            if (session.requiresLogin() && message != null
                && (message.contains("设备替换") || message.equals("请更新 App 后重新登录"))) {
                loginError = message;
                showInitialLogin = true;
                activeTab = RemoteIMTab.MESSAGES;
            }
            Toast.makeText(this,
                message == null || message.trim().isEmpty() ? "操作失败" : message,
                Toast.LENGTH_LONG).show();
        });
    }

    @Override
    public void onNewIncomingMessage(
        RemoteIMMessage message,
        boolean conversationVisible
    ) {
        if (destroyed || message == null) return;
        runOnUiThread(() -> showNewMessageNotification(message));
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_SCREEN_SHARE) {
            Consumer<Boolean> completion = pendingScreenShare;
            pendingScreenShare = null;
            boolean approved = resultCode == RESULT_OK && data != null;
            if (approved)
                MobileScreenCaptureService.start(this, resultCode, data);
            if (completion != null) completion.accept(approved);
            return;
        }
        if (requestCode == REQUEST_CLOUD_DRIVE_MEDIA) {
            if (cloudDriveDialog == null || !cloudDriveDialog.isShowing()) return;
            if (resultCode != RESULT_OK || data == null || data.getData() == null) {
                cloudDriveDialog.onPickCanceled();
                return;
            }
            Uri uri = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException unavailable) {
                cloudDriveDialog.onPickPermissionFailure();
                return;
            }
            cloudDriveDialog.onPicked(uri);
            return;
        }
        if (resultCode != RESULT_OK) {
            if (aiAssistant != null) aiAssistant.handleActivityCancelled(requestCode);
            return;
        }
        if (requestCode == REQUEST_FRIEND_SCAN) {
            String userId = data == null ? "" : data.getStringExtra("friend_user_id");
            if (userId != null && !userId.isEmpty()) showAddContactDialog(userId);
            return;
        }
        if (aiAssistant != null && aiAssistant.handleActivityResult(requestCode, data)) return;
        if (requestCode == REQUEST_PICK_IMAGE && data != null && data.getData() != null) {
            sendPickedImage(data.getData());
        } else if (requestCode == REQUEST_PICK_FILE && data != null && data.getData() != null) {
            sendPickedFile(data.getData());
        } else if (requestCode == REQUEST_TAKE_PHOTO && pendingCameraFile != null) {
            sendImageFile(pendingCameraFile);
            pendingCameraFile = null;
        }
    }

    public void startScreenShareForAgent(Consumer<Boolean> completion) {
        if (MobileScreenCaptureService.isCapturing()) {
            completion.accept(true);
            return;
        }
        if (pendingScreenShare != null) {
            completion.accept(false);
            return;
        }
        MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
        if (manager == null) {
            completion.accept(false);
            return;
        }
        pendingScreenShare = completion;
        try {
            Toast.makeText(this, "请在系统弹窗选择「共享整个屏幕」", Toast.LENGTH_LONG).show();
            startActivityForResult(manager.createScreenCaptureIntent(), REQUEST_SCREEN_SHARE);
        } catch (RuntimeException error) {
            pendingScreenShare = null;
            completion.accept(false);
        }
    }

    @Override
    public void onRequestPermissionsResult(
        int requestCode,
        String[] permissions,
        int[] grantResults
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
        if (maiChatHostTools != null && maiChatHostTools.onPermissionResult(requestCode)) return;
        if (requestCode == REQUEST_GALLERY_ACCESS) {
            Runnable action = pendingGalleryAction;
            pendingGalleryAction = null;
            if (action != null && !destroyed) action.run();
            return;
        }
        if (requestCode == REQUEST_SAVE_VIDEO_PERMISSION) {
            String path = pendingVideoSavePath;
            Runnable completion = pendingVideoSaveCompletion;
            pendingVideoSavePath = null;
            pendingVideoSaveCompletion = null;
            if (granted && path != null) savePreviewVideo(path, completion);
            else {
                if (!granted) toast("没有存储权限，无法保存视频");
                if (completion != null) completion.run();
            }
            return;
        }
        if (requestCode == REQUEST_SAVE_IMAGE_PERMISSION) {
            String path = pendingImageSavePath;
            pendingImageSavePath = null;
            if (granted && path != null) savePreviewImage(path);
            else if (!granted) toast("没有存储权限，无法保存图片");
            return;
        }
        if (requestCode == REQUEST_RECORD_AUDIO) {
            if (granted) toast("麦克风已启用，请按住说话");
            else toast("没有麦克风权限，无法发送语音");
        } else if (requestCode == REQUEST_CAMERA_PERMISSION) {
            if (granted) openCamera();
            else toast("没有相机权限，无法拍照");
        } else if (requestCode == AIAssistantPanel.REQUEST_CAMERA_PERMISSION && aiAssistant != null) {
            aiAssistant.onCameraPermission(granted);
        } else if (requestCode == AIAssistantPanel.REQUEST_MUSIC_PERMISSION && aiAssistant != null) {
            aiAssistant.handleMusicPermission(granted);
        } else if (requestCode == AIAssistantPanel.REQUEST_AUDIO_PERMISSION && aiAssistant != null) {
            aiAssistant.onAudioPermission(granted);
        } else if (requestCode == REQUEST_POST_NOTIFICATIONS && !granted) {
            Log.i(TAG, "notification-suppressed reason=permission-denied");
        }
    }

    VoiceRecordingController voiceRecorderForAssistant() {
        return voiceRecorder;
    }

    SpeechRecognizer speechRecognizerForAssistant() {
        return speechRecognizer;
    }

    private void createMessageNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationChannel channel = new NotificationChannel(
            MESSAGE_CHANNEL_ID,
            "新消息通知",
            NotificationManager.IMPORTANCE_HIGH
        );
        channel.setDescription("收到新的 MaiChat 私聊消息时提醒");
        channel.enableVibration(true);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
    }

    private void requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return;
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
            == PackageManager.PERMISSION_GRANTED) return;
        requestPermissions(
            new String[]{Manifest.permission.POST_NOTIFICATIONS},
            REQUEST_POST_NOTIFICATIONS
        );
    }

    private void showNewMessageNotification(RemoteIMMessage message) {
        if (!RemoteIMNewMessageNotificationPolicy.shouldNotify(
            true,
            activityInForeground
        )) {
            Log.d(TAG, "notification-suppressed reason=app-foreground"
                + " peer=" + message.fromUserId());
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
            && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            Log.i(TAG, "notification-suppressed reason=permission-denied"
                + " peer=" + message.fromUserId());
            return;
        }

        String peerUserId = message.fromUserId();
        RemoteIMContact sender = contact(peerUserId);
        String title = sender == null ? peerUserId : sender.displayName();
        Intent openIntent = new Intent(this, MainActivity.class)
            .putExtra(EXTRA_NOTIFICATION_PEER, peerUserId)
            .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pendingIntent = PendingIntent.getActivity(
            this,
            notificationId(peerUserId),
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );
        int unreadCount = Math.max(1, session.unreadCount(peerUserId));
        String preview = RemoteIMNewMessageNotificationPolicy.aggregatedPreview(
            message,
            unreadCount
        );
        NotificationCompat.Builder builder = new NotificationCompat.Builder(this, MESSAGE_CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(title)
            .setContentText(preview)
            .setStyle(new NotificationCompat.BigTextStyle().bigText(preview))
            .setContentIntent(pendingIntent)
            .setAutoCancel(true)
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setGroup(MESSAGE_GROUP_KEY)
            .setNumber(unreadCount);
        NotificationManagerCompat.from(this).notify(notificationId(peerUserId), builder.build());
        Log.i(TAG, "notification-requested peer=" + peerUserId
            + " unread=" + session.unreadCount(peerUserId));
    }

    private void handleNotificationIntent(Intent intent) {
        if (intent == null) return;
        String peerUserId = intent.getStringExtra(EXTRA_NOTIFICATION_PEER);
        intent.removeExtra(EXTRA_NOTIFICATION_PEER);
        if (peerUserId == null || peerUserId.trim().isEmpty()) return;
        pendingNotificationPeerUserId = peerUserId.trim();
        openPendingNotificationConversationIfPossible();
    }

    private void openPendingNotificationConversationIfPossible() {
        if (pendingNotificationPeerUserId.isEmpty() || session == null || session.requiresLogin()) return;
        String peerUserId = pendingNotificationPeerUserId;
        pendingNotificationPeerUserId = "";
        openChat(peerUserId);
        NotificationManagerCompat.from(this).cancel(notificationId(peerUserId));
        Log.i(TAG, "notification-clicked peer=" + peerUserId);
    }

    private static int notificationId(String peerUserId) {
        return 0x4d430000 ^ (peerUserId == null ? 0 : peerUserId.hashCode());
    }

    private void renderPreservingInput() {
        EditText previousInput = messageInput;
        boolean restoreFocus = messageInput != null && messageInput.hasFocus();
        if (messageInput != null) draftText = messageInput.getText().toString();
        render();
        if (restoreFocus && messageInput != null && messageInput != previousInput) {
            messageInput.requestFocus();
            messageInput.setSelection(messageInput.length());
            messageInput.post(() -> {
                InputMethodManager manager = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                manager.showSoftInput(messageInput, InputMethodManager.SHOW_IMPLICIT);
            });
        }
    }

    @Override public void onActivityChanged(String peerId) {
        if (!activityInForeground) { pendingStateRefresh = true; return; }
        if (!destroyed && peerId.equals(activeChatUserId) && currentMessageList != null) refreshChatMessages();
    }

    private void rememberDraft() {
        if (activeChatUserId == null || displayedOwner.isEmpty()) return;
        String text = messageInput != null ? messageInput.getText().toString() : draftText;
        String key = displayedOwner + "\0" + activeChatUserId;
        if (text.isEmpty()) draftsByPeer.remove(key); else draftsByPeer.put(key, text);
    }

    private void updateComposerActions() {
        if (sendButton == null || attachmentButton == null) return;
        boolean canSend = !voiceMode && !draftText.trim().isEmpty();
        sendButton.setVisibility(canSend ? View.VISIBLE : View.GONE);
        attachmentButton.setVisibility(canSend ? View.GONE : View.VISIBLE);
    }

    private boolean useLandscapeImLayout() {
        Configuration configuration = getResources().getConfiguration();
        return configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
            && configuration.screenWidthDp >= 700 && activeTab == RemoteIMTab.MESSAGES
            && !showInitialLogin && session != null && !session.requiresLogin();
    }

    private View createPortraitConversationBackdrop() {
        LinearLayout previousContent = content;
        LinearLayout backdrop = new LinearLayout(this);
        backdrop.setOrientation(LinearLayout.VERTICAL);
        backdrop.setBackgroundColor(MaiChatTheme.PAGE);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        backdrop.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
        content = list;
        renderConversationList();
        content = previousContent;
        backdrop.addView(bottomTabBar(), match(dp(64)));
        return backdrop;
    }

    // 作用：根据账号、当前页签和正在打开的聊天状态更新 Android 主页面。
    // 切换页面时同步 Agent 前后台状态与输入草稿，避免重建页面时丢失原有会话内容。
    private void render() {
        if (destroyed) return;
        if (videoDialog != null && cloudPlayerStreams.containsKey(videoDialog) &&
            !cloudPlayerOwner.equals(MaiChatAuthClient.shared(this).accountId()))
            videoDialog.dismiss();
        if (imageDialog != null && cloudImageFiles.containsKey(imageDialog) &&
            !cloudImageOwner.equals(MaiChatAuthClient.shared(this).accountId()))
            imageDialog.dismiss();
        boolean landscapeIm = useLandscapeImLayout();
        // Agent 页只在当前页签可见时消费实时事件；换账号先撤销旧页状态，再配置新工作区。
        boolean showingAssistant = activityInForeground && activeTab == RemoteIMTab.ASSISTANT
            && activeChatUserId == null && !showInitialLogin && !session.requiresLogin();
        if (aiAssistant != null) aiAssistant.setForeground(showingAssistant);
        String owner = session.requiresLogin() ? "" : session.settings().loginUserId();
        if (!owner.equals(displayedOwner)) {
            if (videoDialog != null) videoDialog.dismiss();
            if (cloudDriveDialog != null && cloudDriveDialog.isShowing())
                cloudDriveDialog.dismiss();
            assistantVoiceActive = false;
            assistantVoiceText = "";
            if (aiAssistant != null) {
                aiAssistant.setForeground(false);
                aiAssistant = null;
            }
            AIAssistantController.configureAccount(owner);
            if (maiChatHostTools != null)
                AIAssistantController.shared(this).setHostToolHandler(maiChatHostTools::execute);
            rememberDraft();
            if (!displayedOwner.isEmpty()) {
                hideKeyboard(); activeChatUserId = null; activeFriendProfileUserId = null;
                renderedChatUserId = null;
                chatBackdrop = null; draftText = ""; pendingQuote = null; presentationRequest++;
            }
            displayedOwner = owner;
        }
        if (activeChatUserId != null && contact(activeChatUserId) == null) {
            activeChatUserId = null; chatBackdrop = null;
        }
        if (activeFriendProfileUserId != null && profileContact(activeFriendProfileUserId) == null)
            activeFriendProfileUserId = null;
        if (activeChatUserId != null && landscapeIm == renderedLandscapeIm
            && activeChatUserId.equals(renderedChatUserId)
            && currentMessageList != null && !showInitialLogin && !session.requiresLogin()) {
            // 同一会话的消息更新保留原列表实例、滚动位置和输入草稿。
            refreshChatMessages();
            if (landscapeIm && currentConversationRows != null) {
                long fingerprint = conversationFingerprint();
                if (fingerprint != renderedConversationFingerprint) {
                    renderConversationRows(currentConversationRows, "", Collections.emptyList(), false);
                    renderedConversationFingerprint = fingerprint;
                }
            }
            if (renderedVoiceMode != voiceMode || !java.util.Objects.equals(renderedQuote, pendingQuote)) {
                content.removeView(chatComposer);
                session.stopHumanTyping();
                messageInput = null;
                chatComposer = composer();
                content.addView(chatComposer, matchWrap());
                renderedVoiceMode = voiceMode; renderedQuote = pendingQuote;
            }
            return;
        }
        if (activeTab == RemoteIMTab.ASSISTANT && activeChatUserId == null && aiAssistant != null
                && aiAssistant.isAttachedToWindow() && !showInitialLogin && !session.requiresLogin()) return;
        if (activeTab == RemoteIMTab.CLOUD && activeChatUserId == null
                && cloudDriveDialog != null && cloudDriveDialog.isEmbedded()
                && !showInitialLogin && !session.requiresLogin()) return;
        if (cloudDriveDialog != null && cloudDriveDialog.isEmbedded())
            cloudDriveDialog.dismiss();
        // 只有页面身份真的改变才重建导航容器；不能把每条新消息都当作换页。
        currentMessageList = null;
        renderedChatUserId = null;
        messageInput = null;
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(MaiChatTheme.PAGE);
        landscapeConversationPane = null;
        landscapeConversationDivider = null;
        if (swipeBack != null) swipeBack.dispose();
        FrameLayout navigation = new FrameLayout(this);
        if (!landscapeIm && activeChatUserId != null && chatBackdrop == null)
            chatBackdrop = createPortraitConversationBackdrop();
        if (activeChatUserId != null && chatBackdrop != null) {
            if (chatBackdrop.getParent() instanceof ViewGroup)
                ((ViewGroup) chatBackdrop.getParent()).removeView(chatBackdrop);
            navigation.addView(chatBackdrop, new FrameLayout.LayoutParams(-1, -1));
        }
        navigation.addView(root, new FrameLayout.LayoutParams(-1, -1));
        setContentView(navigation);
        renderedLandscapeIm = landscapeIm;
        navigation.post(this::updateVideoDockLayout);
        swipeBack = new ChatSwipeBack(new ChatSwipeBack.Host() {
            @Override public View page() { return root; }
            @Override public boolean canGoBack() {
                return !destroyed && !renderedLandscapeIm
                    && (activeChatUserId != null || activeFriendProfileUserId != null);
            }
            @Override public boolean keyboardVisible() { return keyboardVisible; }
            @Override public void began() {
                session.stopHumanTyping(); cancelVoiceRecording();
                if (currentMessageList != null) currentMessageList.cancelPositionIntent();
                stickToLatestMessage = false;
            }
            @Override public void hideKeyboard() { MainActivity.this.hideKeyboard(); }
            @Override public void completed() {
                if (activeFriendProfileUserId != null) {
                    activeFriendProfileUserId = null;
                    render();
                } else finishChatBack();
            }
            @Override public void cancelled(boolean shown) { keyboardVisible = shown; }
        });
        insetsHost = navigation;
        imeAnimating = false;
        // 键盘动画期间只更新布局边距，结束后再执行一次滚动定位，避免消息跳到空白区域。
        androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(navigation, (view, insets) -> {
            if (insetsHost != navigation) return insets;
            latestInsets = insets;
            boolean visible = insets.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime());
            if (visible && !keyboardVisible && allowKeyboardLocation && (swipeBack == null || !swipeBack.active())
                && currentMessageList != null && messageInput != null && messageInput.hasFocus()) {
                if (imeAnimating) currentMessageList.beginKeyboardReveal(); else currentMessageList.showLatest();
            }
            if (keyboardVisible != visible) navigation.post(this::updateVideoDockLayout);
            keyboardVisible = visible;
            if (!imeAnimating) applyChatInsets(navigation, insets);
            return insets;
        });
        androidx.core.view.ViewCompat.setWindowInsetsAnimationCallback(navigation,
                new androidx.core.view.WindowInsetsAnimationCompat.Callback(
                        androidx.core.view.WindowInsetsAnimationCompat.Callback.DISPATCH_MODE_CONTINUE_ON_SUBTREE) {
                    @Override
                    public void onPrepare(androidx.core.view.WindowInsetsAnimationCompat animation) {
                        if (insetsHost == navigation
                                && (animation.getTypeMask() & androidx.core.view.WindowInsetsCompat.Type.ime()) != 0)
                            imeAnimating = true;
                    }
                    @Override
                    public androidx.core.view.WindowInsetsCompat onProgress(
                            androidx.core.view.WindowInsetsCompat insets,
                            List<androidx.core.view.WindowInsetsAnimationCompat> running) {
                        if (insetsHost == navigation)
                            applyChatInsets(navigation, insets);
                        return insets;
                    }
                    @Override
                    public void onEnd(androidx.core.view.WindowInsetsAnimationCompat animation) {
                        if (insetsHost != navigation
                                || (animation.getTypeMask() & androidx.core.view.WindowInsetsCompat.Type.ime()) == 0)
                            return;
                        imeAnimating = false;
                        if (latestInsets != null)
                            applyChatInsets(navigation, latestInsets);
                        if (currentMessageList != null)
                            currentMessageList.endKeyboardReveal();
                    }
                });
        androidx.core.view.ViewCompat.requestApplyInsets(navigation);

        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(MaiChatTheme.PAGE);
        root.addView(content, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));

        if (session.isRestoringAccount()) {
            FrameLayout loading = new FrameLayout(this);
            loading.addView(
                    new android.widget.ProgressBar(this), new FrameLayout.LayoutParams(dp(32), dp(32), Gravity.CENTER));
            content.addView(loading, matchMatch());
            return;
        }
        if (showInitialLogin || session.requiresLogin()) {
            if (socialBindingVisible) renderSocialBinding();
            else renderLogin();
            return;
        }

        if (activeFriendProfileUserId != null) {
            renderFriendProfile(activeFriendProfileUserId);
            return;
        }

        if (landscapeIm) {
            renderLandscapeIm();
            return;
        }
        if (activeChatUserId != null) {
            // 页面容器建好以后再选择具体内容；聊天页不显示底部主导航。
            renderChatDetail(activeChatUserId);
        } else {
            switch (activeTab) {
                case CLOUD:
                    cloudDriveDialog = new MaiCloudDriveDialog(this);
                    cloudDriveDialog.showEmbedded(content);
                    break;
                case ASSISTANT:
                    if (aiAssistant == null) aiAssistant = new AIAssistantPanel(this);
                    if (aiAssistant.getParent() instanceof ViewGroup)
                        ((ViewGroup) aiAssistant.getParent()).removeView(aiAssistant);
                    content.addView(aiAssistant, matchMatch());
                    break;
                case CONTACTS:
                    renderContacts();
                    break;
                case REMOTE:
                    renderRemoteDesktop();
                    break;
                case ME:
                    renderSettings();
                    break;
                case MESSAGES:
                default:
                    renderConversationList();
                    break;
            }
        }

        boolean hideTabs = activeChatUserId != null
            || (activeTab == RemoteIMTab.REMOTE && session.remoteDesktop().isActive());
        if (!hideTabs) root.addView(bottomTabBar(), match(dp(64)));
    }

    private void applyChatInsets(FrameLayout target, androidx.core.view.WindowInsetsCompat insets) {
        androidx.core.graphics.Insets bars = insets.getInsets(androidx.core.view.WindowInsetsCompat.Type.systemBars()
                | androidx.core.view.WindowInsetsCompat.Type.displayCutout());
        int bottom = Math.max(bars.bottom, insets.getInsets(androidx.core.view.WindowInsetsCompat.Type.ime()).bottom);
        if (target.getPaddingLeft() != bars.left || target.getPaddingTop() != bars.top
                || target.getPaddingRight() != bars.right || target.getPaddingBottom() != bottom) {
            target.setPadding(bars.left, bars.top, bars.right, bottom);
        }
    }

    // 作用：组装手机号登录界面、协议状态和提交反馈。
    // 页面状态变化和用户操作在这里衔接；退出或失败时维持已显示内容。
    private void renderLogin() {
        FrameLayout page = new FrameLayout(this);
        page.setBackgroundColor(Color.WHITE);
        page.setFocusableInTouchMode(true);

        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setGravity(Gravity.CENTER_HORIZONTAL);
        int formWidth = Math.min(
            dp(420),
            getResources().getDisplayMetrics().widthPixels - dp(56)
        );
        FrameLayout.LayoutParams formParams = new FrameLayout.LayoutParams(
            formWidth,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.CENTER_HORIZONTAL
        );
        formParams.topMargin = Math.max(dp(80),
            Math.round(getResources().getDisplayMetrics().heightPixels * 0.18f));
        page.addView(form, formParams);

        // 图标、手机号和验证码属于同一表单；手机号输入框占满输入行的可编辑部分。
        ImageView icon = new ImageView(this);
        icon.setImageResource(getApplicationInfo().icon);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        LinearLayout.LayoutParams iconParams = new LinearLayout.LayoutParams(dp(92), dp(92));
        iconParams.setMargins(0, 0, 0, dp(70));
        form.addView(icon, iconParams);

        LinearLayout phoneRow = new LinearLayout(this);
        phoneRow.setGravity(Gravity.CENTER_VERTICAL);
        phoneRow.setPadding(dp(18), 0, dp(18), 0);
        phoneRow.setBackground(MaiChatTheme.rounded(Color.rgb(247, 248, 250), 30, this));
        if (!loginUsesPassword) {
            TextView prefix = MaiChatTheme.label(this, "+86", 18, MaiChatTheme.SECONDARY);
            phoneRow.addView(prefix, wrapWrap());
            View divider = new View(this);
            divider.setBackgroundColor(MaiChatTheme.BORDER);
            LinearLayout.LayoutParams dividerParams = new LinearLayout.LayoutParams(dp(1), dp(24));
            dividerParams.setMargins(dp(15), 0, dp(15), 0);
            phoneRow.addView(divider, dividerParams);
        }
        EditText account = new EditText(this);
        account.setSingleLine(true);
        account.setTextSize(17);
        account.setHint(loginUsesPassword ? "MaiChat 账号或手机号" : "输入手机号");
        account.setInputType(loginUsesPassword ? android.text.InputType.TYPE_CLASS_TEXT :
            android.text.InputType.TYPE_CLASS_PHONE);
        account.setTextColor(MaiChatTheme.TEXT);
        account.setHintTextColor(Color.rgb(100, 116, 139));
        account.setPadding(0, 0, 0, 0);
        account.setBackgroundColor(Color.TRANSPARENT);
        account.setImeOptions(EditorInfo.IME_ACTION_DONE);
        account.setText(loginUserDraft);
        account.setSelection(account.length());
        phoneRow.addView(account, new LinearLayout.LayoutParams(0, dp(58), 1));
        form.addView(phoneRow, match(dp(58)));

        EditText challengeInput = null;
        if (!loginChallengeId.isEmpty() || loginUsesPassword) {
            challengeInput = new EditText(this);
            challengeInput.setSingleLine(true);
            challengeInput.setTextSize(16);
            challengeInput.setHint(loginUsesPassword ? "输入登录密码" : "输入验证码");
            challengeInput.setInputType(loginUsesPassword
                ? android.text.InputType.TYPE_CLASS_TEXT
                    | android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
                : android.text.InputType.TYPE_CLASS_NUMBER);
            challengeInput.setPadding(dp(18), 0, dp(18), 0);
            challengeInput.setBackground(MaiChatTheme.rounded(
                Color.rgb(247, 248, 250), 30, this));
            challengeInput.setText(loginUsesPassword ? loginPasswordDraft : loginCodeDraft);
            LinearLayout.LayoutParams inputParams = match(dp(58));
            inputParams.setMargins(0, dp(16), 0, 0);
            form.addView(challengeInput, inputParams);
        }

        TextView hint = MaiChatTheme.text(this, loginUsesPassword ?
            "测试账号可不绑定手机号" : "目前仅支持中国大陆地区号码", 13,
            MaiChatTheme.SECONDARY);
        LinearLayout.LayoutParams hintParams = matchWrap();
        hintParams.setMargins(dp(12), dp(14), 0, 0);
        form.addView(hint, hintParams);

        TextView errorText = MaiChatTheme.label(this, loginError, 13, MaiChatTheme.RED);
        errorText.setGravity(Gravity.CENTER);
        errorText.setVisibility(loginError.isEmpty() ? View.GONE : View.VISIBLE);
        LinearLayout.LayoutParams errorParams = matchWrap();
        errorParams.setMargins(0, dp(12), 0, 0);
        form.addView(errorText, errorParams);

        // 首次点击获取验证码；拿到 challenge 后同一主按钮切换为登录，错误仍留在表单内。
        Button login = new Button(this);
        login.setText(loginSubmitting ? "请稍候..." :
            loginChallengeId.isEmpty() && !loginUsesPassword ? "获取验证码" : "登录");
        login.setTextSize(18);
        login.setAllCaps(false);
        login.setGravity(Gravity.CENTER);
        login.setPadding(dp(12), 0, dp(12), 0);
        LinearLayout.LayoutParams loginParams = match(dp(56));
        loginParams.setMargins(0, dp(22), 0, 0);
        form.addView(login, loginParams);

        CheckBox minimumAge = new CheckBox(this);
        minimumAge.setText("我已年满 14 周岁");
        minimumAge.setTextSize(12);
        minimumAge.setTextColor(MaiChatTheme.SECONDARY);
        minimumAge.setButtonTintList(android.content.res.ColorStateList.valueOf(
            Color.rgb(42, 211, 142)));
        minimumAge.setChecked(loginConfirmedAge);
        LinearLayout.LayoutParams minimumAgeParams = matchWrap();
        minimumAgeParams.setMargins(0, dp(16), 0, 0);
        form.addView(minimumAge, minimumAgeParams);

        CheckBox agreement = new CheckBox(this);
        agreement.setText("已阅读并同意");
        agreement.setTextSize(12);
        agreement.setTextColor(MaiChatTheme.SECONDARY);
        agreement.setButtonTintList(android.content.res.ColorStateList.valueOf(
            Color.rgb(42, 211, 142)));
        agreement.setChecked(loginAcceptedTerms);
        LinearLayout legalRow = new LinearLayout(this);
        legalRow.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams agreementParams = matchWrap();
        agreementParams.setMargins(0, dp(4), 0, 0);
        form.addView(legalRow, agreementParams);
        legalRow.addView(agreement, wrapWrap());
        TextView termsLink = MaiChatTheme.text(this, "《用户协议》", 12,
            Color.rgb(42, 211, 142));
        termsLink.setContentDescription("查看用户协议");
        termsLink.setOnClickListener(view -> openLoginDocument(TERMS_URL));
        legalRow.addView(termsLink, wrapWrap());
        TextView privacyLink = MaiChatTheme.text(this, "《隐私政策》", 12,
            Color.rgb(42, 211, 142));
        privacyLink.setContentDescription("查看隐私政策");
        privacyLink.setOnClickListener(view -> openLoginDocument(PRIVACY_URL));
        legalRow.addView(privacyLink, wrapWrap());
        TextView childrenLink = MaiChatTheme.text(this, "《儿童隐私保护声明》", 12,
            Color.rgb(42, 211, 142));
        childrenLink.setContentDescription("查看儿童隐私保护声明");
        childrenLink.setOnClickListener(view -> openLoginDocument(CHILDREN_URL));
        LinearLayout.LayoutParams childrenParams = matchWrap();
        childrenParams.setMargins(dp(28), dp(2), 0, 0);
        form.addView(childrenLink, childrenParams);

        // 密码与短信两种入口共享手机号和协议状态，切换只清除当前挑战而不丢手机号。
        Button alternate = new Button(this);
        alternate.setAllCaps(false);
        alternate.setText(loginUsesPassword ? "验证码登录" :
            loginChallengeId.isEmpty() ? "密码登录" : "重新获取");
        alternate.setTextColor(Color.rgb(42, 211, 142));
        alternate.setBackgroundColor(Color.TRANSPARENT);
        LinearLayout.LayoutParams alternateParams = match(dp(42));
        alternateParams.setMargins(0, dp(14), 0, 0);
        form.addView(alternate, alternateParams);

        // 主按钮以手机号、验证码或密码以及两项确认共同决定可用性，避免触发无效请求。
        Runnable updateLoginAppearance = () -> {
            boolean validAccount = loginUsesPassword ? !loginUserDraft.trim().isEmpty() :
                loginUserDraft.replaceAll("\\D", "").length() == 11;
            boolean enabled = !loginSubmitting && validAccount
                && loginAcceptedTerms && loginConfirmedAge
                && (!loginUsesPassword || !loginPasswordDraft.isEmpty())
                && (loginUsesPassword || loginChallengeId.isEmpty() || loginCodeDraft.length() == 6);
            login.setEnabled(enabled);
            login.setTextColor(Color.WHITE);
            login.setBackground(MaiChatTheme.rounded(
                enabled ? Color.rgb(42, 211, 142) : Color.rgb(162, 233, 207),
                30,
                this
            ));
        };
        updateLoginAppearance.run();
        minimumAge.setOnCheckedChangeListener((button, checked) -> {
            loginConfirmedAge = checked;
            updateLoginAppearance.run();
        });
        agreement.setOnCheckedChangeListener((button, checked) -> {
            loginAcceptedTerms = checked;
            updateLoginAppearance.run();
        });
        account.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable editable) {
                loginUserDraft = editable.toString();
                loginError = "";
                updateLoginAppearance.run();
            }
        });
        if (challengeInput != null) {
            challengeInput.addTextChangedListener(new SimpleTextWatcher() {
                @Override public void afterTextChanged(Editable editable) {
                    if (loginUsesPassword) loginPasswordDraft = editable.toString();
                    else loginCodeDraft = editable.toString();
                    updateLoginAppearance.run();
                }
            });
        }
        View.OnClickListener submit = view -> {
            // 入队前再次校验，防止键盘提交绕过按钮禁用态；失败后保留用户已输入的内容。
            String enteredAccount = account.getText().toString().trim();
            if ((!loginUsesPassword && enteredAccount.replaceAll("\\D", "").length() != 11)
                || (loginUsesPassword && enteredAccount.isEmpty()) || loginSubmitting
                || !loginAcceptedTerms || !loginConfirmedAge) return;
            loginSubmitting = true;
            loginError = "";
            login.setText("请稍候...");
            updateLoginAppearance.run();
            if (loginUsesPassword) {
                session.loginWithAccountPassword(enteredAccount, loginPasswordDraft,
                    loginConfirmedAge, error -> {
                    loginSubmitting = false;
                    loginError = error == null ? "" : error;
                    if (error == null) showInitialLogin = false;
                    render();
                });
            } else if (loginChallengeId.isEmpty()) {
                session.requestAccountCode(enteredAccount, loginConfirmedAge, (challengeId, error) -> {
                    loginSubmitting = false;
                    loginError = error == null ? "" : error;
                    if (error == null) loginChallengeId = challengeId;
                    render();
                });
            } else {
                session.loginWithAccountCode(enteredAccount, loginChallengeId, loginCodeDraft,
                    loginConfirmedAge, error -> {
                    loginSubmitting = false;
                    loginError = error == null ? "" : error;
                    if (error == null) showInitialLogin = false;
                    render();
                });
            }
        };
        login.setOnClickListener(submit);
        alternate.setOnClickListener(view -> {
            if (loginSubmitting) return;
            if (!loginUsesPassword && !loginChallengeId.isEmpty()) {
                loginChallengeId = "";
            } else {
                loginUsesPassword = !loginUsesPassword;
                loginChallengeId = "";
            }
            loginError = "";
            render();
        });
        account.setOnEditorActionListener((view, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                submit.onClick(view);
                return true;
            }
            return false;
        });
        page.requestFocus();
        content.addView(page, matchMatch());
    }

    public SocialCredentialCallback socialCredentialCallback() {
        return socialCredentialCallback;
    }

    private void handleLocalSocialIntent(Intent intent) {
        if (intent == null || !BuildConfig.DEBUG || !BuildConfig.USE_LOCAL_UI_AUTH
            || !(Build.FINGERPRINT.contains("sdk_gphone")
                || Build.FINGERPRINT.contains("generic"))) return;
        String provider = intent.getStringExtra(EXTRA_LOCAL_SOCIAL_PROVIDER);
        String credential = intent.getStringExtra(EXTRA_LOCAL_SOCIAL_CREDENTIAL);
        intent.removeExtra(EXTRA_LOCAL_SOCIAL_PROVIDER);
        intent.removeExtra(EXTRA_LOCAL_SOCIAL_CREDENTIAL);
        if (provider != null && credential != null)
            socialCredentialCallback.onCredential(provider, credential);
    }

    private static String socialProviderName(String provider) {
        return "wechat".equals(provider) ? "微信" : "qq".equals(provider) ? "QQ" : "社交账号";
    }

    private void beginSocialLogin(String provider, String credential) {
        if (destroyed || loginSubmitting || socialBindingVisible || !session.requiresLogin()) return;
        if (!"wechat".equals(provider) && !"qq".equals(provider)) {
            loginError = "授权来源无效，请重试";
            render();
            return;
        }
        if (credential == null || credential.isEmpty()) {
            loginError = socialProviderName(provider) + "授权失败，请重试";
            render();
            return;
        }
        if (!loginAcceptedTerms || !loginConfirmedAge) {
            loginError = "请先确认年龄并同意协议";
            render();
            return;
        }
        loginSubmitting = true;
        socialAuthInFlight = true;
        loginError = "";
        render();
        session.loginWithSocialCredential(provider, credential, loginConfirmedAge,
            (result, error) -> {
                if (destroyed) return;
                loginSubmitting = false;
                socialAuthInFlight = false;
                if (error != null) {
                    loginError = error;
                    render();
                    return;
                }
                if (result != null && result.needsPhoneBinding()) {
                    socialProvider = provider;
                    socialBindingToken = result.bindingToken;
                    socialBindingExpiresAt = SystemClock.elapsedRealtime()
                        + result.expiresIn * 1000L;
                    socialPhoneDraft = loginUserDraft;
                    socialCodeDraft = "";
                    socialChallengeId = "";
                    socialBindingError = "";
                    socialBindingVisible = true;
                    render();
                } else if (result != null && result.isSignedIn()) {
                    clearSocialBinding();
                    showInitialLogin = false;
                    render();
                } else {
                    loginError = "登录服务返回的账号数据无效";
                    render();
                }
            });
    }

    private boolean socialBindingExpired() {
        return socialBindingExpiresAt <= SystemClock.elapsedRealtime();
    }

    private void clearSocialBinding() {
        socialUiHandler.removeCallbacks(socialBindingTick);
        socialBindingCountdown = null;
        socialBindingVisible = false;
        socialSubmitting = false;
        socialProvider = "";
        socialBindingToken = "";
        socialPhoneDraft = "";
        socialCodeDraft = "";
        socialChallengeId = "";
        socialBindingError = "";
        socialBindingExpiresAt = 0;
    }

    private void renderSocialBinding() {
        socialUiHandler.removeCallbacks(socialBindingTick);
        socialBindingCountdown = null;
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.WHITE);
        LinearLayout outer = new LinearLayout(this);
        outer.setGravity(Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(0, dp(8), 0, dp(32));
        int width = Math.min(dp(420), getResources().getDisplayMetrics().widthPixels - dp(56));
        outer.addView(form, new LinearLayout.LayoutParams(width, -2));
        scroll.addView(outer, new ScrollView.LayoutParams(-1, -2));
        content.addView(scroll, matchMatch());

        LinearLayout back = new LinearLayout(this);
        back.setGravity(Gravity.CENTER_VERTICAL);
        back.setContentDescription("返回手机号登录");
        TextView backArrow = MaiChatTheme.label(this, "‹", 30, MaiChatTheme.TEXT);
        backArrow.setGravity(Gravity.CENTER);
        back.addView(backArrow, new LinearLayout.LayoutParams(dp(30), dp(44)));
        TextView backLabel = MaiChatTheme.text(this, "返回", 15, MaiChatTheme.SECONDARY);
        back.addView(backLabel, new LinearLayout.LayoutParams(dp(44), dp(44)));
        form.addView(back, new LinearLayout.LayoutParams(dp(88), dp(44)));
        back.setOnClickListener(view -> {
            clearSocialBinding();
            render();
        });
        TextView title = MaiChatTheme.label(this, "验证手机号", 24, MaiChatTheme.TEXT);
        LinearLayout.LayoutParams titleParams = match(dp(48));
        titleParams.setMargins(0, dp(8), 0, 0);
        form.addView(title, titleParams);
        TextView explanation = MaiChatTheme.text(this,
            "首次使用" + socialProviderName(socialProvider) + "登录，需要验证手机号并绑定账号。",
            14, MaiChatTheme.SECONDARY);
        explanation.setLineSpacing(dp(4), 1);
        form.addView(explanation, matchWrap());

        if (socialBindingExpired()) {
            TextView expired = MaiChatTheme.label(this,
                "授权已过期，请返回登录页重新授权。", 15, MaiChatTheme.RED);
            LinearLayout.LayoutParams expiredParams = matchWrap();
            expiredParams.setMargins(0, dp(24), 0, 0);
            form.addView(expired, expiredParams);
            return;
        }

        socialBindingCountdown = MaiChatTheme.text(this, "", 13, MaiChatTheme.SECONDARY);
        LinearLayout.LayoutParams countdownParams = match(dp(28));
        countdownParams.setMargins(0, dp(10), 0, 0);
        form.addView(socialBindingCountdown, countdownParams);
        socialUiHandler.post(socialBindingTick);

        EditText phone = new EditText(this);
        phone.setSingleLine(true);
        phone.setTextSize(17);
        phone.setHint("输入手机号");
        phone.setInputType(android.text.InputType.TYPE_CLASS_PHONE);
        phone.setPadding(dp(16), 0, dp(16), 0);
        phone.setBackground(MaiChatTheme.rounded(Color.rgb(247, 248, 250), 30, this));
        phone.setText(socialPhoneDraft);
        phone.setSelection(phone.length());
        LinearLayout.LayoutParams phoneParams = match(dp(56));
        phoneParams.setMargins(0, dp(14), 0, 0);
        form.addView(phone, phoneParams);

        EditText code = null;
        if (!socialChallengeId.isEmpty()) {
            code = new EditText(this);
            code.setSingleLine(true);
            code.setTextSize(17);
            code.setHint("输入六位验证码");
            code.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
            code.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(6)});
            code.setPadding(dp(16), 0, dp(16), 0);
            code.setBackground(MaiChatTheme.rounded(Color.rgb(247, 248, 250), 30, this));
            code.setText(socialCodeDraft);
            LinearLayout.LayoutParams codeParams = match(dp(56));
            codeParams.setMargins(0, dp(14), 0, 0);
            form.addView(code, codeParams);
        }

        TextView errorText = MaiChatTheme.label(this, socialBindingError, 14, MaiChatTheme.RED);
        errorText.setVisibility(socialBindingError.isEmpty() ? View.GONE : View.VISIBLE);
        LinearLayout.LayoutParams errorParams = matchWrap();
        errorParams.setMargins(0, dp(14), 0, 0);
        form.addView(errorText, errorParams);
        Button submit = new Button(this);
        submit.setText(socialSubmitting ? "请稍候..." :
            socialChallengeId.isEmpty() ? "获取验证码" : "绑定并登录");
        submit.setTextSize(18);
        submit.setTextColor(Color.WHITE);
        submit.setAllCaps(false);
        submit.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams submitParams = match(dp(54));
        submitParams.setMargins(0, dp(24), 0, 0);
        form.addView(submit, submitParams);
        Runnable updateSubmit = () -> {
            String number = socialPhoneDraft.replaceAll("\\D", "");
            boolean valid = number.length() == 11 && !socialSubmitting
                && (socialChallengeId.isEmpty() || socialCodeDraft.matches("[0-9]{6}"));
            submit.setEnabled(valid);
            submit.setBackground(MaiChatTheme.rounded(
                valid ? Color.rgb(42, 211, 142) : Color.rgb(162, 233, 207), 30, this));
        };
        EditText codeInput = code;
        Button[] resendButton = new Button[1];
        phone.addTextChangedListener(new SimpleTextWatcher() {
            @Override public void afterTextChanged(Editable value) {
                String updated = value.toString();
                if (!updated.equals(socialPhoneDraft)) {
                    socialPhoneDraft = updated;
                    if (!socialChallengeId.isEmpty()) {
                        socialChallengeId = "";
                        socialCodeDraft = "";
                        submit.setText("获取验证码");
                        if (codeInput != null) codeInput.setVisibility(View.GONE);
                        if (resendButton[0] != null) resendButton[0].setVisibility(View.GONE);
                    }
                }
                updateSubmit.run();
            }
        });
        if (code != null) {
            code.addTextChangedListener(new SimpleTextWatcher() {
                @Override public void afterTextChanged(Editable value) {
                    socialCodeDraft = value.toString();
                    updateSubmit.run();
                }
            });
        }
        updateSubmit.run();
        submit.setOnClickListener(view -> submitSocialBinding());
        if (!socialChallengeId.isEmpty()) {
            Button resend = new Button(this);
            resend.setText("重新获取验证码");
            resend.setTextSize(15);
            resend.setTextColor(Color.rgb(42, 211, 142));
            resend.setAllCaps(false);
            resend.setBackgroundColor(Color.TRANSPARENT);
            resendButton[0] = resend;
            LinearLayout.LayoutParams resendParams = match(dp(44));
            resendParams.setMargins(0, dp(8), 0, 0);
            form.addView(resend, resendParams);
            resend.setOnClickListener(view -> {
                if (socialSubmitting) return;
                socialChallengeId = "";
                socialCodeDraft = "";
                socialBindingError = "";
                render();
            });
        }
        TextView notice = MaiChatTheme.text(this,
            "验证码用于绑定" + socialProviderName(socialProvider)
                + "身份；已有手机号沿用原账号。", 13, MaiChatTheme.SECONDARY);
        notice.setLineSpacing(dp(4), 1);
        LinearLayout.LayoutParams noticeParams = matchWrap();
        noticeParams.setMargins(0, dp(22), 0, 0);
        form.addView(notice, noticeParams);
    }

    private void submitSocialBinding() {
        if (socialSubmitting || !socialBindingVisible) return;
        if (socialBindingExpired()) {
            socialBindingError = "授权已过期，请返回重新授权";
            render();
            return;
        }
        String phone = socialPhoneDraft.trim();
        if (phone.replaceAll("\\D", "").length() != 11) return;
        String token = socialBindingToken;
        socialSubmitting = true;
        socialBindingError = "";
        hideKeyboard();
        render();
        if (socialChallengeId.isEmpty()) {
            session.requestSocialBindCode(phone, loginConfirmedAge, (challengeId, error) -> {
                if (destroyed || !socialBindingVisible || !token.equals(socialBindingToken)) return;
                socialSubmitting = false;
                socialBindingError = error == null ? "" : error;
                if (error == null) socialChallengeId = challengeId;
                render();
            });
        } else {
            String challengeId = socialChallengeId;
            String code = socialCodeDraft;
            if (!code.matches("[0-9]{6}")) {
                socialSubmitting = false;
                render();
                return;
            }
            session.bindSocialPhone(token, phone, challengeId, code, loginConfirmedAge,
                error -> {
                    if (destroyed || !socialBindingVisible || !token.equals(socialBindingToken))
                        return;
                    socialSubmitting = false;
                    if (error != null) {
                        socialBindingError = error;
                        render();
                        return;
                    }
                    clearSocialBinding();
                    showInitialLogin = false;
                    render();
                });
        }
    }

    private void openLoginDocument(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (android.content.ActivityNotFoundException error) {
            toast("无法打开协议页面");
        }
    }

    private View bottomTabBar() {
        LinearLayout outside = new LinearLayout(this);
        outside.setOrientation(LinearLayout.VERTICAL);
        outside.setBackgroundColor(Color.WHITE);
        View divider = new View(this);
        divider.setBackgroundColor(MaiChatTheme.BORDER);
        outside.addView(divider, match(dp(1)));

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER);
        bar.setPadding(dp(16), dp(10), dp(16), dp(12));
        outside.addView(bar, new LinearLayout.LayoutParams(-1, 0, 1));
        bar.addView(tabButton(RemoteIMTab.MESSAGES), weightMatch());
        bar.addView(tabButton(RemoteIMTab.CLOUD), weightMatch());
        bar.addView(tabButton(RemoteIMTab.ASSISTANT), weightMatch());
        bar.addView(tabButton(RemoteIMTab.CONTACTS), weightMatch());
        bar.addView(tabButton(RemoteIMTab.ME), weightMatch());
        return outside;
    }

    private View tabButton(RemoteIMTab tab) {
        FrameLayout button = new FrameLayout(this);
        boolean selected = activeTab == tab;
        button.setContentDescription(tab.title());
        button.setSelected(selected);
        button.setClickable(true);
        button.setFocusable(true);

        ImageView icon = new ImageView(this);
        icon.setImageResource(tabIconResource(tab));
        icon.setColorFilter(selected ? tabAccentColor(tab) : MaiChatTheme.SECONDARY);
        FrameLayout.LayoutParams iconParams = new FrameLayout.LayoutParams(
            dp(23), dp(23), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        iconParams.topMargin = dp(1);
        button.addView(icon, iconParams);

        TextView title = new TextView(this);
        title.setText(tab.title());
        title.setTextSize(10);
        title.setSingleLine(true);
        title.setGravity(Gravity.CENTER);
        title.setTextColor(selected ? tabAccentColor(tab) : MaiChatTheme.SECONDARY);
        FrameLayout.LayoutParams titleParams = new FrameLayout.LayoutParams(-1, dp(17), Gravity.BOTTOM);
        button.addView(title, titleParams);

        if (tab == RemoteIMTab.MESSAGES && session.totalUnreadCount() > 0) {
            View badge = new View(this);
            badge.setBackground(MaiChatTheme.rounded(Color.rgb(245, 60, 48), 4, this));
            FrameLayout.LayoutParams badgeParams =
                    new FrameLayout.LayoutParams(dp(8), dp(8), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
            badgeParams.leftMargin = dp(13);
            badgeParams.topMargin = dp(5);
            button.addView(badge, badgeParams);
        }
        button.setOnClickListener(view -> {
            cancelVoiceRecording();
            activeTab = tab;
            activeChatUserId = null;
            render();
            if (tab == RemoteIMTab.CONTACTS) session.refreshAcceptedFriends();
        });
        return button;
    }

    private int tabIconResource(RemoteIMTab tab) {
        switch (tab) {
            case MESSAGES: return R.drawable.ic_nav_messages;
            case CLOUD: return R.drawable.ic_nav_cloud;
            case ASSISTANT: return R.drawable.ic_nav_assistant;
            case CONTACTS: return R.drawable.ic_nav_contacts;
            case REMOTE: return R.drawable.ic_nav_remote;
            case ME: return R.drawable.ic_nav_profile;
        }
        return R.drawable.ic_nav_messages;
    }

    private int tabAccentColor(RemoteIMTab tab) {
        switch (tab) {
            case MESSAGES: return MaiChatTheme.NAV_MESSAGES;
            case CLOUD: return MaiChatTheme.NAV_MESSAGES;
            case ASSISTANT: return MaiChatTheme.NAV_ASSISTANT;
            case CONTACTS: return MaiChatTheme.NAV_CONTACTS;
            case REMOTE: return MaiChatTheme.NAV_REMOTE;
            case ME: return MaiChatTheme.NAV_PROFILE;
        }
        return MaiChatTheme.NAV_MESSAGES;
    }

    private void renderConversationList() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout rows = new LinearLayout(this);
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setBackgroundColor(Color.WHITE);
        currentConversationRows = rows;
        scroll.addView(rows, matchWrap());
        content.addView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));
        renderConversationRows(rows, "", Collections.emptyList(), false);
        renderedConversationFingerprint = conversationFingerprint();
    }

    private long conversationFingerprint() {
        long hash = 1;
        for (RemoteIMContact contact : session.chatState().contacts()) {
            hash = hash * 31 + contact.userId().hashCode();
            hash = hash * 31 + contact.displayName().hashCode();
            RemoteIMMessage latest = latestMessage(contact.userId());
            if (latest != null) {
                hash = hash * 31 + latest.id().hashCode();
                hash = hash * 31 + latest.text().hashCode();
            }
            hash = hash * 31 + session.unreadCount(contact.userId());
        }
        return hash * 31 + session.mediaRevision();
    }

    private void renderLandscapeIm() {
        LinearLayout columns = new LinearLayout(this);
        columns.setOrientation(LinearLayout.HORIZONTAL);
        content.addView(columns, matchMatch());

        LinearLayout sidebar = new LinearLayout(this);
        sidebar.setOrientation(LinearLayout.VERTICAL);
        sidebar.setBackgroundColor(MaiChatTheme.PAGE);
        int widthDp = getResources().getConfiguration().screenWidthDp;
        int sidebarDp = Math.min(Math.max((int) (widthDp * 0.32f), 280), 360);
        columns.addView(sidebar, new LinearLayout.LayoutParams(dp(sidebarDp), -1));
        View divider = new View(this);
        divider.setBackgroundColor(MaiChatTheme.BORDER);
        columns.addView(divider, new LinearLayout.LayoutParams(dp(1), -1));
        LinearLayout detail = new LinearLayout(this);
        detail.setOrientation(LinearLayout.VERTICAL);
        detail.setBackgroundColor(Color.WHITE);
        columns.addView(detail, new LinearLayout.LayoutParams(0, -1, 1));
        landscapeConversationPane = sidebar;
        landscapeConversationDivider = divider;

        content = sidebar;
        renderConversationList();
        sidebar.addView(bottomTabBar(), match(dp(64)));
        content = detail;
        if (activeChatUserId != null) renderChatDetail(activeChatUserId);
        else detail.addView(emptyState("◇", "选择一个对话", "从左侧选择联系人开始聊天。"), matchMatch());
        columns.post(this::updateVideoDockLayout);
    }

    // 用当前搜索条件生成会话列表行；搜索命中消息时保留其所属会话，避免打开错误联系人。
    // 每次重绘以稳定的会话标识绑定点击事件，不依赖行在列表中的当前位置。
    private void renderConversationRows(
        LinearLayout rows,
        String queryValue,
        List<RemoteIMMessageSearchHit> messageHits,
        boolean searchingMessages
    ) {
        rows.removeAllViews();
        String query = queryValue == null ? "" : queryValue.trim().toLowerCase(Locale.ROOT);
        List<RemoteIMContact> contacts = new ArrayList<>(session.chatState().contacts());
        contacts.removeIf(contact -> {
            if (query.isEmpty()) return false;
            RemoteIMMessage latest = latestMessage(contact.userId());
            return !contact.userId().toLowerCase(Locale.ROOT).contains(query)
                && !contact.displayName().toLowerCase(Locale.ROOT).contains(query)
                && (latest == null || !latest.text().toLowerCase(Locale.ROOT).contains(query));
        });
        contacts.sort((left, right) -> {
            RemoteIMMessage leftMessage = latestMessage(left.userId());
            RemoteIMMessage rightMessage = latestMessage(right.userId());
            long leftTime = leftMessage == null ? 0 : leftMessage.createdAtMillis();
            long rightTime = rightMessage == null ? 0 : rightMessage.createdAtMillis();
            if (leftTime == rightTime) return left.displayName().compareToIgnoreCase(right.displayName());
            return Long.compare(rightTime, leftTime);
        });

        if (!query.isEmpty() && !contacts.isEmpty()) {
            rows.addView(searchSectionLabel("联系人"), match(dp(34)));
        }
        for (RemoteIMContact contact : contacts) {
            View rowContent = conversationRow(contact);
            SwipeActionRow row = new SwipeActionRow(
                this,
                rowContent,
                "清空消息",
                Color.rgb(245, 158, 11),
                () -> confirm(
                    "清空聊天记录？",
                    "将清空与 " + contact.displayName() + " 的消息，但保留该好友。",
                    "清空",
                    () -> session.clearHistory(contact.userId())
                )
            );
            rows.addView(row, match(dp(72)));
        }

        if (searchingMessages) {
            TextView loading = MaiChatTheme.text(this, "正在搜索全部消息…", 13, MaiChatTheme.SECONDARY);
            loading.setGravity(Gravity.CENTER_VERTICAL);
            loading.setPadding(dp(16), 0, dp(16), 0);
            rows.addView(loading, match(dp(48)));
        } else if (!query.isEmpty() && messageHits != null && !messageHits.isEmpty()) {
            rows.addView(searchSectionLabel("消息"), match(dp(34)));
            for (RemoteIMMessageSearchHit hit : messageHits) {
                rows.addView(messageSearchResultRow(hit), match(dp(72)));
            }
        }

        if (contacts.isEmpty()
            && !searchingMessages
            && (messageHits == null || messageHits.isEmpty())) {
            String title = query.isEmpty() ? "暂无会话" : "没有搜索结果";
            String detail = query.isEmpty()
                ? "到通讯录添加好友账号后即可开始聊天。"
                : "换个关键词再试试。";
            rows.addView(emptyState("○", title, detail), match(dp(260)));
        }
    }

    private TextView searchSectionLabel(String title) {
        TextView label = MaiChatTheme.label(this, title, 12, MaiChatTheme.SECONDARY);
        label.setGravity(Gravity.BOTTOM);
        label.setPadding(dp(16), 0, dp(16), dp(6));
        label.setBackgroundColor(MaiChatTheme.PAGE);
        return label;
    }

    private View messageSearchResultRow(RemoteIMMessageSearchHit hit) {
        RemoteIMContact contact = contact(hit.peerUserId());
        RemoteIMMessage message = hit.message();
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(16), dp(7), dp(16), dp(7));
        row.setBackgroundColor(Color.WHITE);

        LinearLayout heading = new LinearLayout(this);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = MaiChatTheme.label(
            this,
            contact == null ? hit.peerUserId() : contact.displayName(),
            14,
            MaiChatTheme.TEXT
        );
        heading.addView(name, new LinearLayout.LayoutParams(0, dp(24), 1));
        heading.addView(
            MaiChatTheme.text(this, timestamp(message.createdAtMillis()), 11, MaiChatTheme.SECONDARY),
            wrapWrap()
        );
        row.addView(heading, match(dp(24)));

        TextView digest = MaiChatTheme.text(this, message.text(), 13, MaiChatTheme.SECONDARY);
        digest.setSingleLine(true);
        digest.setEllipsize(TextUtils.TruncateAt.END);
        row.addView(digest, match(dp(26)));
        row.setOnClickListener(view -> openMessageSearchHit(hit));
        return row;
    }

    private View conversationRow(RemoteIMContact contact) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(16), 0, dp(16), 0);
        row.setBackgroundColor(renderedLandscapeIm && contact.userId().equals(activeChatUserId)
            ? MaiChatTheme.NAV_MESSAGES_SOFT : Color.WHITE);
        row.addView(avatar(contact, true, 42), new LinearLayout.LayoutParams(dp(42), dp(42)));

        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams textParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        textParams.setMargins(dp(12), 0, 0, 0);
        row.addView(text, textParams);

        LinearLayout titleLine = new LinearLayout(this);
        titleLine.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = MaiChatTheme.label(this, contact.displayName(), 16, MaiChatTheme.TEXT);
        title.setSingleLine(true);
        title.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        titleLine.addView(title, new LinearLayout.LayoutParams(0, dp(24), 1));
        RemoteIMMessage latest = latestMessage(contact.userId());
        if (latest != null) {
            TextView time = MaiChatTheme.text(this, timestamp(latest.createdAtMillis()), 11, MaiChatTheme.SECONDARY);
            titleLine.addView(time, wrapWrap());
        }
        text.addView(titleLine, match(dp(24)));

        LinearLayout subtitleLine = new LinearLayout(this);
        subtitleLine.setGravity(Gravity.CENTER_VERTICAL);
        TextView subtitle = MaiChatTheme.text(
            this,
            latest == null ? "暂无消息" : latest.text(),
            13,
            MaiChatTheme.SECONDARY
        );
        subtitle.setSingleLine(true);
        subtitle.setEllipsize(TextUtils.TruncateAt.END);
        subtitleLine.addView(subtitle, new LinearLayout.LayoutParams(0, dp(22), 1));
        int unread = session.unreadCount(contact.userId());
        if (unread > 0) subtitleLine.addView(unreadBadge(unread), wrapWrap());
        text.addView(subtitleLine, match(dp(22)));

        row.setOnClickListener(view -> openChat(contact.userId()));
        return row;
    }

    private void openChat(String userId) {
        allowKeyboardLocation = true;
        rememberDraft();
        draftText = draftsByPeer.getOrDefault(displayedOwner + "\0" + userId, "");
        if (activeChatUserId == null) {
            chatBackdrop = renderedLandscapeIm ? null : root;
            returnTab = activeTab;
        }
        session.selectContact(userId);
        session.setConversationVisible(userId, true);
        session.loadInitialMessages(userId);
        messageWindowSize = 20;
        messageWindowStartId = null;
        activeChatUserId = userId;
        activeTab = RemoteIMTab.MESSAGES;
        stickToLatestMessage = true;
        messageSearchTargetId = null;
        pendingQuote = null;
        render();
    }

    private void openMessageSearchHit(RemoteIMMessageSearchHit hit) {
        rememberDraft();
        draftText = draftsByPeer.getOrDefault(displayedOwner + "\0" + hit.peerUserId(), "");
        if (activeChatUserId == null) {
            chatBackdrop = renderedLandscapeIm ? null : root;
            returnTab = activeTab;
        }
        RemoteIMContact opened = session.openMessageSearchHit(hit);
        if (opened == null) return;
        session.setConversationVisible(opened.userId(), true);
        activeChatUserId = opened.userId();
        activeTab = RemoteIMTab.MESSAGES;
        stickToLatestMessage = false;
        messageSearchTargetId = hit.message().id();
        render();
    }

    private void renderChatDetail(String userId) {
        RemoteIMContact peer = contact(userId);
        if (peer == null) { activeChatUserId = null; renderConversationList(); return; }
        renderedChatUserId = userId;
        waitingForChatEntry = true;
        content.addView(chatDetailHeader(peer), match(dp(52)));
        currentMessageList = new ChatMessageList(this, message -> messageBubble(message, contact(userId),
            RemoteIMApprovalDisplayPolicy.stateFor(message.approvalRequest(), visibleApprovalStates)));
        currentMessageList.setPadding(dp(12), dp(8), dp(12), dp(8));
        currentMessageList.setClipToPadding(false);
        if (messageSearchTargetId != null) currentMessageList.showMessage(messageSearchTargetId);
        else if (stickToLatestMessage) currentMessageList.requestLatestAfterUpdate();
        stickToLatestMessage = false;
        FrameLayout stage = new FrameLayout(this);
        stage.setBackgroundColor(Color.WHITE);
        stage.addView(currentMessageList, new FrameLayout.LayoutParams(-1, -1));
        chatEmptyState = emptyState("◇", "暂无消息", "发送一条消息开始对话。");
        stage.addView(chatEmptyState, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER));
        content.addView(stage, new LinearLayout.LayoutParams(-1, 0, 1));
        chatComposer = composer();
        renderedVoiceMode = voiceMode; renderedQuote = pendingQuote;
        content.addView(chatComposer, matchWrap());
        currentMessageList.setOnScrollListener(new android.widget.AbsListView.OnScrollListener() {
            @Override public void onScrollStateChanged(android.widget.AbsListView view, int state) {
                browsingHistory = state == SCROLL_STATE_TOUCH_SCROLL;
                if (browsingHistory) { currentMessageList.cancelPositionIntent(); stickToLatestMessage = false; }
            }
            @Override public void onScroll(android.widget.AbsListView view, int first, int visible, int total) {
                if (browsingHistory && first == 0 && session.hasEarlierMessages(userId)
                        && !session.isLoadingMessages(userId)) {
                    messageWindowSize += 20;
                    session.loadEarlierMessages(userId);
                }
            }
        });
        refreshChatMessages();
    }

    private void refreshChatMessages() {
        if (currentMessageList == null || activeChatUserId == null) return;
        if (waitingForChatEntry && session.isLoadingMessages(activeChatUserId)) {
            currentMessageList.setVisibility(View.INVISIBLE); chatEmptyState.setVisibility(View.GONE); return;
        }
        String peer = activeChatUserId;
        ChatMessageList list = currentMessageList;
        long request = ++presentationRequest;
        List<RemoteIMMessage> all = session.chatState().messagesWith(peer);
        List<RemoteIMMessage> values = new ArrayList<>();
        int first = messageSearchTargetId != null ? 0 : Math.max(0, all.size() - messageWindowSize);
        if (messageWindowStartId != null) {
            for (int index = 0; index < all.size(); index++) if (messageWindowStartId.equals(all.get(index).id())) {
                first = Math.min(first, index); break;
            }
        }
        for (int index = first; index < all.size(); index++) values.add(all.get(index).snapshot());
        int visible = Math.max(0, values.size() - 20);
        String prepareTarget = messageSearchTargetId != null ? messageSearchTargetId : list.firstVisibleMessageId();
        if (!stickToLatestMessage && prepareTarget != null) {
            for (int index = 0; index < values.size(); index++)
                if (values.get(index).id().equals(prepareTarget)) {
                    visible = index;
                    break;
                }
        }
        // 只准备当前视口及少量预取消息，不把累计历史一次性交给列表。
        java.util.Set<String> needed = new java.util.LinkedHashSet<>();
        for (int index = Math.max(0, visible - 20); index < Math.min(values.size(), visible + 24); index++)
            needed.add(values.get(index).text());
        for (int index = Math.max(0, values.size() - 20); index < values.size(); index++)
            needed.add(values.get(index).text());
        List<MarkdownRenderer.Presentation> sources = new ArrayList<>();
        for (RemoteIMMessage message : values) if (needed.contains(message.text())) {
                sources.add(new MarkdownRenderer.Presentation(
                        message.text(), usesInlineDate(message) ? message.createdAtMillis() : null));
        }
        MarkdownRenderer.preparePresentations(sources, () -> !destroyed && request == presentationRequest, () -> {
            if (list != currentMessageList || !peer.equals(activeChatUserId)) return;
            RemoteIMActivitySignal activity = session.activity(peer);
            visibleApprovalStates = RemoteIMApprovalDisplayPolicy.statesFor(values);
            list.update(values, activity,
                    java.util.Objects.hash(visibleApprovalStates, playingMessageId, session.mediaRevision()));
            list.setVisibility(View.VISIBLE);
            if (diagnostics != null)
                diagnostics.context("chat rows=" + list.getCount() + " peerHash=" + peer.hashCode());
            chatEmptyState.setVisibility(values.isEmpty() && activity == null ? View.VISIBLE : View.GONE);
            waitingForChatEntry = false;
            if (!values.isEmpty()) messageWindowStartId = values.get(0).id();
            messageWindowSize = Math.max(messageWindowSize, values.size());
            if (!session.isLoadingMessages(peer)) stickToLatestMessage = false;
            if (messageSearchTargetId != null) {
                messageSearchTargetId = null;
                messageWindowSize = Math.max(messageWindowSize, all.size());
            }
        });
    }

    // 作用：构建聊天详情标题栏与当前联系人的操作入口。
    private View chatDetailHeader(RemoteIMContact contact) {
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(8), dp(5), dp(12), dp(5));
        header.setBackgroundColor(Color.WHITE);

        View back = symbolButton(
            MaiChatSymbolView.Symbol.CHEVRON_LEFT,
            MaiChatTheme.TEXT,
            24,
            Color.TRANSPARENT,
            0
        );
        back.setContentDescription("返回会话列表");
        back.setOnClickListener(view -> {
            if (renderedLandscapeIm) finishChatBack();
            else if (swipeBack != null) swipeBack.goBack();
        });
        header.addView(back, new LinearLayout.LayoutParams(dp(38), dp(42)));

        TextView title = MaiChatTheme.label(this, contact.displayName(), 18, MaiChatTheme.TEXT);
        title.setSingleLine(true);
        title.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        header.addView(title, new LinearLayout.LayoutParams(0, dp(42), 1));

        View remote = symbolButton(
            MaiChatSymbolView.Symbol.REMOTE,
            MaiChatTheme.BLUE_DARK,
            25,
            Color.TRANSPARENT,
            0
        );
        remote.setContentDescription("查看远程桌面");
        remote.setOnClickListener(view -> {
            RemoteDesktopController controller = session.remoteDesktop();
            if (!controller.isActive()) {
                if (session.currentSdkAppId() <= 0 || session.currentUserSig().isEmpty()) {
                    session.retryConnection();
                    toast("IM 凭证正在更新，请稍后重试");
                    return;
                }
                controller.requestView(contact.userId(), session.settings().loginUserId(),
                    session.currentSdkAppId(), session.currentUserSig());
            }
            activeChatUserId = null;
            activeTab = RemoteIMTab.REMOTE;
            render();
        });
        header.addView(remote, new LinearLayout.LayoutParams(dp(42), dp(42)));
        return header;
    }

    // 按消息类型拼装同一气泡内的正文、媒体与审批状态；附件缺失时仍保留消息外壳。
    // 点击和长按动作只绑定当前消息标识，避免列表重绘后操作到相邻消息。
    private View messageBubble(
        RemoteIMMessage message,
        RemoteIMContact peer,
        RemoteIMApprovalDisplayPolicy.State approvalState
    ) {
        boolean outgoing = message.direction() == RemoteIMMessage.Direction.OUTGOING;
        LinearLayout outer = new LinearLayout(this);
        outer.setOrientation(LinearLayout.HORIZONTAL);
        outer.setGravity((outgoing ? Gravity.END : Gravity.START) | Gravity.BOTTOM);
        outer.setPadding(0, dp(7), 0, dp(7));

        LinearLayout bubble = new LinearLayout(this);
        bubble.setOrientation(LinearLayout.VERTICAL);
        bubble.setPadding(dp(13), dp(11), dp(13), dp(11));
        bubble.setBackground(MaiChatTheme.rounded(outgoing ? Color.rgb(234, 244, 255) : Color.TRANSPARENT, 10, this));

        if (message.quote() != null) {
            bubble.addView(quoteBlock(message.quote(), peer), matchWrap());
        }

        String attachmentCaption = RemoteIMAttachmentCaptionPolicy.caption(message);
        RemoteIMAttachmentCaptionPolicy.Placement captionPlacement =
            RemoteIMAttachmentCaptionPolicy.placement(message);
        if (captionPlacement == RemoteIMAttachmentCaptionPolicy.Placement.ABOVE) {
            bubble.addView(attachmentCaptionView(attachmentCaption), matchWrap());
        }

        if (message.imageAttachment() != null) {
            bubble.addView(imageMessageContent(message.imageAttachment()), matchWrap());
        } else if (message.voiceAttachment() != null) {
            TextView voice = MaiChatTheme.label(
                this,
                (message.id().equals(playingMessageId) ? "■" : "▶")
                    + "  " + message.voiceAttachment().durationSeconds() + " 秒",
                15,
                MaiChatTheme.BLUE_DARK
            );
            voice.setPadding(0, dp(8), dp(18), dp(4));
            voice.setOnClickListener(view -> toggleVoicePlayback(message));
            bubble.addView(voice, matchWrap());
        } else if (message.videoAttachment() != null) {
            bubble.addView(videoMessageContent(message.videoAttachment()), matchWrap());
        } else if (message.fileAttachment() != null) {
            bubble.addView(fileMessageContent(message.fileAttachment()), matchWrap());
        } else {
            TextView body = MaiChatTypography.body(this);
            MarkdownRenderer.bind(body, message.text(), usesInlineDate(message) ? message.createdAtMillis() : null);
            bubble.addView(body, matchWrap());
        }

        if (captionPlacement == RemoteIMAttachmentCaptionPolicy.Placement.BELOW) {
            bubble.addView(attachmentCaptionView(attachmentCaption), matchWrap());
        }

        if (message.approvalRequest() != null) {
            bubble.addView(
                approvalActions(message, peer, approvalState),
                matchWrap()
            );
        }

        bubble.setOnLongClickListener(view -> { showMessageCopyDialog(message); return true; });
        boolean compact =
                outgoing && usesInlineDate(message) && !message.text().contains("\n") && message.text().length() < 100;
        LinearLayout.LayoutParams bubbleParams = compact
            ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            : new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        outer.addView(bubble, bubbleParams);
        if (!usesInlineDate(message)) {
            TextView date = MaiChatTheme.text(this, "· " + timestamp(message.createdAtMillis()), 11, MaiChatTheme.BLUE);
            date.setTypeface(MaiChatTypography.semibold());
            LinearLayout.LayoutParams dateParams = wrapWrap(); dateParams.setMargins(dp(6), 0, 0, dp(11));
            outer.addView(date, dateParams);
        }
        if (outgoing) {
            String icon = message.status() == RemoteIMMessage.Status.SENT ? "✓"
                    : message.status() == RemoteIMMessage.Status.FAILED   ? "!"
                                                                          : "◷";
            TextView status = MaiChatTheme.label(this, icon, 13, statusColor(message.status()));
            status.setContentDescription(statusText(message.status()));
            LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(dp(18), dp(24));
            statusParams.setMargins(dp(8), 0, 0, dp(6));
            outer.addView(status, statusParams);
        }
        LinearLayout row = new LinearLayout(this); row.setOrientation(LinearLayout.VERTICAL);
        row.addView(outer, matchWrap());
        View separator = new View(this); separator.setBackgroundColor(MaiChatTheme.BORDER);
        row.addView(separator, match(Math.max(1, dp(.5f))));
        return row;
    }

    private static boolean usesInlineDate(RemoteIMMessage message) {
        return message.imageAttachment() == null && message.videoAttachment() == null
                && message.voiceAttachment() == null && message.fileAttachment() == null
                && message.approvalRequest() == null;
    }

    private TextView attachmentCaptionView(String caption) {
        TextView body = MaiChatTypography.body(this);
        body.setPadding(0, dp(4), 0, dp(4));
        MarkdownRenderer.bind(body, caption);
        return body;
    }

    private View quoteBlock(RemoteIMQuote quote, RemoteIMContact peer) {
        LinearLayout block = new LinearLayout(this);
        block.setOrientation(LinearLayout.HORIZONTAL);
        block.setGravity(Gravity.CENTER_VERTICAL);
        block.setPadding(dp(8), dp(7), dp(10), dp(7));
        block.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 8, this));

        View accent = new View(this);
        accent.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 2, this));
        LinearLayout.LayoutParams accentParams = new LinearLayout.LayoutParams(dp(3), dp(30));
        accentParams.setMargins(0, 0, dp(8), 0);
        block.addView(accent, accentParams);

        String sender = quoteSenderName(quote.senderId(), peer);
        TextView text = MaiChatTheme.text(
            this,
            sender.isEmpty() ? quote.digest() : sender + "：" + quote.digest(),
            12,
            MaiChatTheme.SECONDARY
        );
        text.setMaxLines(2);
        text.setEllipsize(TextUtils.TruncateAt.END);
        block.addView(text, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        if (!quote.messageId().isEmpty()) {
            block.setClickable(true);
            block.setFocusable(true);
            block.setOnClickListener(view -> jumpToQuotedMessage(quote));
            block.setContentDescription("跳到引用的消息：" + text.getText());
        }
        LinearLayout.LayoutParams params = matchWrap();
        params.setMargins(0, dp(5), 0, dp(5));
        block.setLayoutParams(params);
        return block;
    }

    private View pendingQuoteBar(RemoteIMQuote quote) {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(10), 0, dp(4), 0);
        bar.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 9, this));

        String sender = quoteSenderName(quote.senderId(), contact(activeChatUserId));
        TextView text = MaiChatTheme.text(
            this,
            sender.isEmpty() ? quote.digest() : "回复 " + sender + "：" + quote.digest(),
            12,
            MaiChatTheme.SECONDARY
        );
        text.setSingleLine(true);
        text.setEllipsize(TextUtils.TruncateAt.END);
        bar.addView(text, new LinearLayout.LayoutParams(0, dp(42), 1));

        TextView close = iconButton("×", 18, MaiChatTheme.SECONDARY);
        close.setGravity(Gravity.CENTER);
        close.setContentDescription("取消引用回复");
        close.setOnClickListener(view -> {
            pendingQuote = null;
            render();
        });
        bar.addView(close, new LinearLayout.LayoutParams(dp(38), dp(38)));
        return bar;
    }

    private String quoteSenderName(String senderId, RemoteIMContact peer) {
        String cleanSender = senderId == null ? "" : senderId.trim();
        if (cleanSender.isEmpty()) return "";
        if (cleanSender.equals(session.chatState().ownerUserId())) return "我";
        if (peer != null && cleanSender.equals(peer.userId())) return peer.displayName();
        RemoteIMContact contact = contact(cleanSender);
        return contact == null ? cleanSender : contact.displayName();
    }

    private void jumpToQuotedMessage(RemoteIMQuote quote) {
        if (activeChatUserId == null || quote == null || quote.messageId().isEmpty()) return;
        String peer = activeChatUserId;
        ChatMessageList list = currentMessageList;
        long intent = list.cancelPositionIntent();
        session.findQuotedMessage(peer, quote.messageId(), target -> {
            if (destroyed || list != currentMessageList || !peer.equals(activeChatUserId)
                    || !list.acceptsIntent(intent))
                return;
            if (target == null) {
                toast("原消息不在本地记录中");
                return;
            }
            messageSearchTargetId = target.id();
            list.showMessage(target.id());
            stickToLatestMessage = false;
            render();
        });
    }

    // 根据审批状态展示当前有效的操作；已经处理的请求只能显示结果，不能再次决策。
    // 提交时携带原请求标识，服务端回执晚到也不会误改另一条审批消息。
    private View approvalActions(
        RemoteIMMessage message,
        RemoteIMContact peer,
        RemoteIMApprovalDisplayPolicy.State state
    ) {
        LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.VERTICAL);

        View divider = new View(this);
        divider.setBackgroundColor(MaiChatTheme.BORDER);
        LinearLayout.LayoutParams dividerParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(1)
        );
        dividerParams.setMargins(0, dp(8), 0, dp(9));
        wrapper.addView(divider, dividerParams);

        if (state == RemoteIMApprovalDisplayPolicy.State.SENT) {
            TextView sent = MaiChatTheme.label(this, "✓  审批选择已发送", 13, MaiChatTheme.GREEN);
            sent.setContentDescription("审批选择已发送");
            wrapper.addView(sent, match(dp(34)));
            return wrapper;
        }
        if (state == RemoteIMApprovalDisplayPolicy.State.AUTO_DECLINED) {
            TextView resolved = MaiChatTheme.label(this, "✕  审批已因新消息自动拒绝", 13, Color.rgb(217, 119, 6));
            resolved.setContentDescription("审批已因新消息自动拒绝");
            wrapper.addView(resolved, match(dp(34)));
            return wrapper;
        }
        if (state == RemoteIMApprovalDisplayPolicy.State.RESOLVED) {
            TextView resolved = MaiChatTheme.label(this, "✓  审批已处理", 13, MaiChatTheme.SECONDARY);
            resolved.setContentDescription("审批已处理");
            wrapper.addView(resolved, match(dp(34)));
            return wrapper;
        }
        if (state == RemoteIMApprovalDisplayPolicy.State.SENDING) {
            TextView sending = MaiChatTheme.label(this, "◷  审批选择正在发送…", 13, MaiChatTheme.BLUE_DARK);
            sending.setContentDescription("审批选择正在发送");
            wrapper.addView(sending, match(dp(34)));
            return wrapper;
        }

        RemoteIMApprovalRequest request = message.approvalRequest();
        for (RemoteIMApprovalAction action : request.actions()) {
            TextView button = MaiChatTheme.label(
                this,
                action.title(),
                14,
                action == RemoteIMApprovalAction.REJECT ? MaiChatTheme.RED : Color.WHITE
            );
            button.setGravity(Gravity.CENTER_VERTICAL);
            button.setPadding(dp(12), 0, dp(12), 0);
            button.setContentDescription("审批操作：" + action.title());
            int background = action == RemoteIMApprovalAction.APPROVE_PREFIX
                ? MaiChatTheme.GREEN
                : action == RemoteIMApprovalAction.REJECT
                    ? Color.TRANSPARENT
                    : MaiChatTheme.BLUE;
            int border = action == RemoteIMApprovalAction.REJECT
                ? MaiChatTheme.RED
                : background;
            button.setBackground(MaiChatTheme.bordered(background, border, 9, this));
            button.setOnClickListener(view -> sendApprovalDecision(
                wrapper,
                peer.userId(),
                request,
                action
            ));
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(40)
            );
            params.setMargins(0, 0, 0, dp(8));
            wrapper.addView(button, params);
        }
        return wrapper;
    }

    private void sendApprovalDecision(
        LinearLayout actionsView,
        String peerId,
        RemoteIMApprovalRequest request,
        RemoteIMApprovalAction action
    ) {
        // 立即反馈，但不调用会重建整棵 Activity 视图树的 render()。
        // 控制器会在 PENDING/SENT/FAILED 状态落库后合并通知，只执行一次正式重绘。
        actionsView.removeAllViews();
        TextView sending = MaiChatTheme.label(
            this,
            "◷  审批选择正在发送…",
            13,
            MaiChatTheme.BLUE_DARK
        );
        sending.setContentDescription("审批选择正在发送");
        actionsView.addView(sending, match(dp(34)));
        try {
            session.sendApprovalDecision(peerId, request, action);
        } catch (IOException | RuntimeException error) {
            Toast.makeText(
                this,
                error.getMessage() == null ? "审批发送失败" : error.getMessage(),
                Toast.LENGTH_LONG
            ).show();
            render();
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    // 作用：组装聊天输入栏、附件入口和语音手势。
    // 页面状态变化和用户操作在这里衔接；退出或失败时维持已显示内容。
    private View composer() {
        LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.VERTICAL);
        wrapper.setPadding(dp(16), dp(9), dp(16), dp(10));
        wrapper.setBackgroundColor(Color.WHITE);

        if (pendingQuote != null) {
            wrapper.addView(pendingQuoteBar(pendingQuote), match(dp(42)));
        }

        speechPreview = MaiChatTheme.text(this, speechText, 14, MaiChatTheme.BLUE_DARK);
        speechPreview.setMaxLines(4);
        speechPreview.setPadding(dp(12), dp(8), dp(12), dp(8));
        speechPreview.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 12, this));
        speechPreview.setVisibility(holdingVoice ? View.VISIBLE : View.GONE);
        wrapper.addView(speechPreview, matchWrap());

        LinearLayout replySuggestions = new LinearLayout(this);
        replySuggestions.setOrientation(LinearLayout.VERTICAL);
        renderAiReplySuggestions(replySuggestions);
        wrapper.addView(replySuggestions, matchWrap());

        LinearLayout suggestions = new LinearLayout(this);
        suggestions.setOrientation(LinearLayout.VERTICAL);
        wrapper.addView(suggestions, matchWrap());

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.BOTTOM);

        View voiceToggle = symbolButton(
            voiceMode ? MaiChatSymbolView.Symbol.KEYBOARD : MaiChatSymbolView.Symbol.SPEAKER,
            MaiChatTheme.BLUE_DARK,
            28,
            MaiChatTheme.BLUE_SOFT,
            14
        );
        voiceToggle.setBackground(MaiChatTheme.bordered(
            MaiChatTheme.BLUE_SOFT,
            MaiChatTheme.BORDER,
            14,
            this
        ));
        voiceToggle.setContentDescription(voiceMode ? "切换键盘" : "切换语音");
        voiceToggle.setOnClickListener(view -> {
            voiceMode = !voiceMode;
            if (!voiceMode) allowKeyboardLocation = true;
            session.stopHumanTyping();
            if (voiceMode) hideKeyboard();
            render();
            if (!voiceMode && messageInput != null) {
                messageInput.requestFocus();
                ((InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
                        .showSoftInput(messageInput, InputMethodManager.SHOW_IMPLICIT);
            }
        });
        bar.addView(voiceToggle, new LinearLayout.LayoutParams(dp(44), dp(44)));

        if (voiceMode) {
            TextView press = MaiChatTheme.label(this, "按住说话", 15, MaiChatTheme.TEXT);
            press.setGravity(Gravity.CENTER);
            press.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 14, this));
            LinearLayout.LayoutParams pressParams = new LinearLayout.LayoutParams(0, dp(44), 1);
            pressParams.setMargins(dp(8), 0, dp(8), 0);
            bar.addView(press, pressParams);
            press.setOnTouchListener((view, event) -> handleVoiceTouch(view, event));
        } else {
            messageInput = new GrowingMessageEditText(this);
            messageInput.setText(draftText);
            messageInput.setSelection(messageInput.length());
            messageInput.setHint("输入要发送给当前联系人的消息...");
            messageInput.setTextSize(14);
            messageInput.setTextColor(MaiChatTheme.TEXT);
            messageInput.setHintTextColor(MaiChatTheme.SECONDARY);
            messageInput.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 14, this));
            GrowingMessageEditText editor = messageInput;
            editor.setOnTouchListener((view, event) -> {
                if (event.getActionMasked() == MotionEvent.ACTION_DOWN) allowKeyboardLocation = true;
                return false;
            });
            String editingPeer = activeChatUserId, editingOwner = displayedOwner;
            messageInput.setOnEditorActionListener((view, actionId, event) -> {
                if (messageInput != editor || !java.util.Objects.equals(editingPeer, activeChatUserId)
                        || !editingOwner.equals(displayedOwner))
                    return false;
                if (event != null
                        && (event.isShiftPressed()
                                || android.view.inputmethod.BaseInputConnection.getComposingSpanStart(editor.getText())
                                        >= 0))
                    return false;
                if (actionId == EditorInfo.IME_ACTION_SEND
                        || (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                                && event.getAction() == KeyEvent.ACTION_DOWN)) {
                    sendText();
                    return true;
                }
                return false;
            });
            messageInput.setOnFocusChangeListener((view, focused) -> {
                if (!focused && messageInput == editor)
                    session.stopHumanTyping();
            });
            messageInput.addTextChangedListener(new SimpleTextWatcher() {
                @Override
                public void afterTextChanged(Editable editable) {
                    if (messageInput != editor || !java.util.Objects.equals(editingPeer, activeChatUserId)
                            || !editingOwner.equals(displayedOwner))
                        return;
                    draftText = editable.toString();
                    rememberDraft(); updateComposerActions();
                    session.updateHumanTyping(activeChatUserId, messageInput.hasFocus() && !draftText.isEmpty());
                    renderCommandSuggestions(suggestions, draftText);
                }
            });
            LinearLayout.LayoutParams inputParams =
                    new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
            inputParams.setMargins(dp(8), 0, dp(8), 0);
            bar.addView(messageInput, inputParams);
            renderCommandSuggestions(suggestions, draftText);
        }

        View plus = symbolButton(
            MaiChatSymbolView.Symbol.PLUS,
            MaiChatTheme.TEXT,
            28,
            Color.WHITE,
            14
        );
        plus.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 14, this));
        plus.setContentDescription("添加图片或文件");
        plus.setOnClickListener(this::showAttachmentMenu);
        attachmentButton = plus;
        FrameLayout action = new FrameLayout(this);
        action.addView(plus, new FrameLayout.LayoutParams(-1, -1));
        sendButton = MaiChatTheme.label(this, "↑", 24, Color.WHITE);
        sendButton.setGravity(Gravity.CENTER);
        sendButton.setContentDescription("发送消息");
        sendButton.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 22, this));
        sendButton.setOnClickListener(view -> sendText());
        action.addView(sendButton, new FrameLayout.LayoutParams(-1, -1));
        bar.addView(action, new LinearLayout.LayoutParams(dp(44), dp(44)));
        updateComposerActions();
        wrapper.addView(bar, matchWrap());
        return wrapper;
    }

    private void renderAiReplySuggestions(LinearLayout container) {
        String latestId = latestMessageId(activeChatUserId);
        if (!java.util.Objects.equals(aiReplySuggestionsPeer, activeChatUserId)
            || !java.util.Objects.equals(aiReplySuggestionsLatestId, latestId)) {
            aiReplySuggestions = new ArrayList<>();
            aiReplySuggestionsLoading = false;
        }
        if (!aiReplySuggestionsLoading && aiReplySuggestions.isEmpty()) return;

        TextView title = MaiChatTheme.text(
            this,
            aiReplySuggestionsLoading ? "正在生成自然、轻松和专业回复…" : "AI 回复建议 · 点击后仍需手动发送",
            12,
            MaiChatTheme.SECONDARY
        );
        title.setPadding(dp(4), dp(4), dp(4), dp(5));
        container.addView(title, matchWrap());
        String[] labels = {"自然", "轻松", "专业"};
        for (int index = 0; index < aiReplySuggestions.size(); index += 1) {
            String suggestion = aiReplySuggestions.get(index);
            TextView row = MaiChatTheme.text(
                this,
                labels[Math.min(index, labels.length - 1)] + " · " + suggestion,
                14,
                MaiChatTheme.TEXT
            );
            row.setPadding(dp(12), dp(8), dp(12), dp(8));
            row.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 10, this));
            row.setOnClickListener(view -> {
                draftText = suggestion;
                rememberDraft();
                aiReplySuggestions = new ArrayList<>();
                aiReplySuggestionsLoading = false;
                if (messageInput != null) {
                    messageInput.setText(draftText);
                    messageInput.setSelection(messageInput.length());
                    messageInput.requestFocus();
                }
                render();
            });
            LinearLayout.LayoutParams params = matchWrap();
            params.setMargins(0, 0, 0, dp(6));
            container.addView(row, params);
        }
    }

    private String latestMessageId(String peerId) {
        if (session == null || peerId == null) return "";
        List<RemoteIMMessage> messages = session.chatState().messagesWith(peerId);
        return messages.isEmpty() ? "" : messages.get(messages.size() - 1).id();
    }

    private void requestAiReplySuggestions() {
        if (session == null || activeChatUserId == null) return;
        List<RemoteIMMessage> messages = session.chatState().recentMessagesWith(activeChatUserId, 24);
        if (messages.isEmpty()) {
            toast("当前会话还没有可参考的消息");
            return;
        }
        JSONArray context = new JSONArray();
        for (RemoteIMMessage message : messages) {
            String text = message.text().trim();
            if (text.isEmpty() || message.approvalRequest() != null ||
                message.approvalDecision() != null) continue;
            try {
                context.put(new JSONObject()
                    .put("speaker", message.direction() == RemoteIMMessage.Direction.OUTGOING ? "me" : "friend")
                    .put("text", text));
            } catch (JSONException ignored) {
                // 键和值都是本地字符串，此分支仅作防御性处理。
            }
        }
        if (context.length() == 0) {
            toast("当前会话没有可用于生成回复的文字");
            return;
        }
        String peer = activeChatUserId;
        String latestId = latestMessageId(peer);
        aiReplySuggestionsPeer = peer;
        aiReplySuggestionsLatestId = latestId;
        aiReplySuggestions = new ArrayList<>();
        aiReplySuggestionsLoading = true;
        render();
        AIAssistantController.shared(this).suggestReplies(context, (result, error) -> {
            if (destroyed || !java.util.Objects.equals(peer, activeChatUserId)
                || !java.util.Objects.equals(latestId, latestMessageId(peer))) return;
            aiReplySuggestionsLoading = false;
            if (result == null) {
                toast(error == null || error.isEmpty() ? "AI 回复建议生成失败" : error);
            } else {
                aiReplySuggestions = new ArrayList<>(java.util.Arrays.asList(
                    result.optString("natural"),
                    result.optString("casual"),
                    result.optString("professional")
                ));
                aiReplySuggestions.removeIf(String::isEmpty);
            }
            render();
        });
    }

    private void renderCommandSuggestions(LinearLayout container, String value) {
        List<RemoteIMSlashCommand> commands = RemoteIMSlashCommandCatalog.suggestions(value);
        if (commands.equals(container.getTag())) return;
        container.setTag(new ArrayList<>(commands));
        container.removeAllViews();
        if (commands.isEmpty()) return;
        int maximum = Math.min(commands.size(), 6);
        for (int index = 0; index < maximum; index += 1) {
            RemoteIMSlashCommand command = commands.get(index);
            TextView row = MaiChatTheme.text(
                this,
                command.command() + "   " + command.label(),
                14,
                MaiChatTheme.TEXT
            );
            row.setTypeface(Typeface.MONOSPACE);
            row.setPadding(dp(12), 0, dp(12), 0);
            row.setBackground(MaiChatTheme.bordered(MaiChatTheme.BLUE_SOFT, MaiChatTheme.BORDER, 9, this));
            row.setOnClickListener(view -> {
                draftText = command.command();
                if (messageInput != null) {
                    messageInput.setText(draftText);
                    messageInput.setSelection(messageInput.length());
                }
            });
            LinearLayout.LayoutParams params = match(dp(46));
            params.setMargins(0, 0, 0, dp(5));
            container.addView(row, params);
        }
    }

    private void renderContacts() {
        LinearLayout rows = new LinearLayout(this);
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setPadding(dp(16), dp(8), dp(16), dp(16));
        content.addView(contactToolbar(rows), match(dp(54)));
        ScrollView scroll = new ScrollView(this);
        scroll.addView(rows, matchWrap());
        content.addView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));

        renderContactRows(rows);
    }

    private void renderContactRows(LinearLayout rows) {
        rows.removeAllViews();
        List<RemoteIMContact> contacts = session.acceptedFriends();
        List<String> groups = session.chatState().contactGroups();
        if (contacts.isEmpty() && groups.isEmpty()) {
            rows.addView(emptyState("◎", "暂无联系人", "添加好友账号后即可开始聊天。"), match(dp(280)));
            return;
        }

        for (ContactGroupDisplayPolicy.Row row : ContactGroupDisplayPolicy.rows(
            groups, contacts, collapsedContactGroups, ""
        )) {
            if (row.kind() == ContactGroupDisplayPolicy.Kind.GROUP_HEADER) {
                rows.addView(contactGroupHeader(
                    rows, row.groupName(), row.memberCount()
                ), match(dp(42)));
            } else {
                addContactRow(rows, row.contact(), row.isIndented());
            }
        }
    }

    private void addContactRow(LinearLayout rows, RemoteIMContact contact, boolean grouped) {
        View card = contactRow(contact);
        card.setOnLongClickListener(view -> {
            showContactGroupPicker(contact);
            return true;
        });
        SwipeActionRow row = new SwipeActionRow(
            this,
            card,
            "删除",
            MaiChatTheme.RED,
            () -> confirm(
                "删除好友？",
                "将从好友列表移除 " + contact.displayName() + "，聊天记录仍会保留。",
                "删除",
                () -> session.deleteContact(contact.userId())
            )
        );
        LinearLayout.LayoutParams params = match(dp(76));
        params.setMargins(grouped ? dp(14) : 0, 0, 0, dp(8));
        rows.addView(row, params);
    }

    private View contactToolbar(LinearLayout rows) {
        LinearLayout toolbar = new LinearLayout(this);
        toolbar.setOrientation(LinearLayout.HORIZONTAL);
        toolbar.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        toolbar.setPadding(dp(12), dp(4), dp(12), dp(4));
        toolbar.setBackgroundColor(Color.WHITE);
        TextView requests = iconButton("申请", 15, MaiChatTheme.BLUE_DARK);
        requests.setContentDescription("查看好友申请");
        requests.setOnClickListener(view -> showFriendRequestsDialog());
        toolbar.addView(requests, new LinearLayout.LayoutParams(dp(52), dp(42)));
        TextView plus = iconButton("＋", 24, MaiChatTheme.BLUE_DARK);
        plus.setContentDescription("添加好友");
        plus.setOnClickListener(view -> showAddContactDialog());
        LinearLayout.LayoutParams plusParams = new LinearLayout.LayoutParams(dp(44), dp(42));
        plusParams.leftMargin = dp(8);
        toolbar.addView(plus, plusParams);
        return toolbar;
    }

    private View contactGroupHeader(LinearLayout rows, String group, int count) {
        TextView header = MaiChatTheme.label(
            this,
            (!collapsedContactGroups.contains(group) ? "⌄ " : "› ")
                + group + "  " + count,
            14,
            MaiChatTheme.TEXT
        );
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(4), 0, dp(8), 0);
        header.setOnClickListener(view -> {
            if (collapsedContactGroups.contains(group)) collapsedContactGroups.remove(group);
            else collapsedContactGroups.add(group);
            renderContactRows(rows);
        });
        header.setOnLongClickListener(view -> {
            showContactGroupActions(group);
            return true;
        });
        return header;
    }

    private void showContactGroupPicker(RemoteIMContact contact) {
        List<String> groups = session.chatState().contactGroups();
        List<String> labels = new ArrayList<>(groups);
        if (!contact.groupName().isEmpty()) labels.add("移出分组");
        if (labels.isEmpty()) return;
        new AlertDialog.Builder(this)
            .setTitle("移动到分组")
            .setItems(labels.toArray(new String[0]), (dialog, index) -> {
                if (index < groups.size()) {
                    session.setContactGroup(contact.userId(), groups.get(index));
                } else if (!contact.groupName().isEmpty() && index == groups.size()) {
                    session.setContactGroup(contact.userId(), "");
                }
            })
            .show();
    }

    private void showContactGroupActions(String group) {
        new AlertDialog.Builder(this)
            .setTitle(group)
            .setItems(new String[]{"重命名分组", "删除分组"}, (dialog, index) -> {
                if (index == 0) {
                    showContactGroupNameDialog("重命名分组", group, null);
                    return;
                }
                int members = 0;
                for (RemoteIMContact contact : session.acceptedFriends()) {
                    if (group.equals(contact.groupName())) members += 1;
                }
                String detail = members == 0
                    ? "这个分组是空的，删除后不影响任何联系人。"
                    : "组里的 " + members + " 位联系人会直接列在通讯录里，好友本身不会被删除。";
                confirm("删除分组？", detail, "删除分组", () -> session.deleteContactGroup(group));
            })
            .show();
    }

    private void showContactGroupNameDialog(String title, String originalName, String moveUserId) {
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setText(originalName);
        input.setSelection(input.length());
        input.setHint("分组名");
        int horizontalPadding = dp(20);
        FrameLayout wrapper = new FrameLayout(this);
        wrapper.setPadding(horizontalPadding, dp(8), horizontalPadding, 0);
        wrapper.addView(input, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(48)
        ));
        AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle(title)
            .setView(wrapper)
            .setNegativeButton("取消", null)
            .setPositiveButton("确定", null)
            .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE)
            .setOnClickListener(view -> {
                String name = ContactGroups.normalize(input.getText().toString());
                if (!ContactGroups.isAcceptableName(name)) {
                    input.setError("分组名不能为空");
                    return;
                }
                boolean changed = originalName.isEmpty()
                    ? session.createContactGroup(name)
                    : session.renameContactGroup(originalName, name);
                if (!changed) {
                    input.setError("已经有同名分组");
                    return;
                }
                if (moveUserId != null) session.setContactGroup(moveUserId, name);
                dialog.dismiss();
            }));
        allowKeyboardLocation = false;
        dialog.show();
    }

    private View contactRow(RemoteIMContact contact) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(12), dp(10));
        boolean selected = contact.userId().equals(session.chatState().selectedPeerId());
        row.setBackground(MaiChatTheme.bordered(
            selected ? MaiChatTheme.BLUE_SOFT : Color.WHITE,
            selected ? Color.rgb(55, 185, 255) : MaiChatTheme.BORDER,
            8,
            this
        ));
        row.addView(avatar(contact, true, 42), new LinearLayout.LayoutParams(dp(42), dp(42)));
        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        boolean syncPending = session.isFriendSyncPending(contact.userId());
        LinearLayout.LayoutParams textParams = new LinearLayout.LayoutParams(
            0, dp(syncPending ? 68 : 50), 1);
        textParams.setMargins(dp(12), 0, dp(8), 0);
        row.addView(text, textParams);
        TextView name = MaiChatTheme.label(this, contact.displayName(), 15, MaiChatTheme.TEXT);
        name.setSingleLine(true);
        name.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        text.addView(name, match(dp(25)));
        String accountId = session.friendAccountId(contact.userId());
        TextView userId = MaiChatTheme.text(this, accountId, 12, MaiChatTheme.SECONDARY);
        userId.setTypeface(Typeface.MONOSPACE);
        userId.setSingleLine(true);
        userId.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        text.addView(userId, match(dp(23)));
        if (syncPending) {
            TextView pending = MaiChatTheme.text(this, "聊天服务同步中", 11,
                MaiChatTheme.SECONDARY);
            text.addView(pending, match(dp(18)));
        }
        row.addView(presenceBadge(contact.userId()), wrapWrap());
        row.setOnClickListener(view -> openFriendProfile(contact.userId()));
        return row;
    }

    private RemoteIMContact profileContact(String userId) {
        for (RemoteIMContact contact : session.acceptedFriends())
            if (contact.userId().equals(userId)) return contact;
        return null;
    }

    private void openFriendProfile(String userId) {
        if (profileContact(userId) == null) return;
        hideKeyboard();
        activeFriendProfileUserId = userId;
        render();
    }

    private void renderFriendProfile(String userId) {
        RemoteIMContact contact = profileContact(userId);
        if (contact == null) { activeFriendProfileUserId = null; render(); return; }
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(MaiChatTheme.PAGE);
        content.addView(page, matchMatch());

        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(16), 0, dp(16), 0);
        TextView back = iconButton("‹", 29, MaiChatTheme.TEXT);
        back.setContentDescription("返回通讯录");
        back.setOnClickListener(view -> { activeFriendProfileUserId = null; render(); });
        header.addView(back, new LinearLayout.LayoutParams(dp(44), dp(52)));
        TextView title = MaiChatTheme.label(this, "好友资料", 19, MaiChatTheme.TEXT);
        title.setGravity(Gravity.CENTER);
        header.addView(title, new LinearLayout.LayoutParams(0, dp(52), 1));
        header.addView(new View(this), new LinearLayout.LayoutParams(dp(44), dp(52)));
        page.addView(header, match(dp(60)));

        ScrollView scroll = new ScrollView(this);
        LinearLayout details = new LinearLayout(this);
        details.setOrientation(LinearLayout.VERTICAL);
        details.setPadding(dp(24), dp(38), dp(24), dp(24));
        scroll.addView(details, matchWrap());
        page.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));

        LinearLayout.LayoutParams avatarParams = new LinearLayout.LayoutParams(dp(88), dp(88));
        avatarParams.gravity = Gravity.CENTER_HORIZONTAL;
        details.addView(avatar(contact, false, 88), avatarParams);
        TextView name = MaiChatTheme.label(this, contact.displayName(), 22, MaiChatTheme.TEXT);
        name.setGravity(Gravity.CENTER);
        name.setContentDescription("好友昵称 " + contact.displayName());
        LinearLayout.LayoutParams nameParams = matchWrap();
        nameParams.topMargin = dp(20);
        details.addView(name, nameParams);

        LinearLayout accountCard = new LinearLayout(this);
        accountCard.setOrientation(LinearLayout.VERTICAL);
        accountCard.setPadding(dp(18), dp(16), dp(18), dp(16));
        accountCard.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 14, this));
        LinearLayout.LayoutParams cardParams = matchWrap();
        cardParams.topMargin = dp(28);
        details.addView(accountCard, cardParams);
        accountCard.addView(MaiChatTheme.text(this, "MaiChat ID", 13,
            MaiChatTheme.SECONDARY), match(dp(24)));
        LinearLayout accountLine = new LinearLayout(this);
        accountLine.setGravity(Gravity.CENTER_VERTICAL);
        accountCard.addView(accountLine, matchWrap());
        String accountId = session.friendAccountId(userId);
        TextView id = MaiChatTheme.label(this, accountId.isEmpty() ? "正在读取账号" : accountId,
            15, MaiChatTheme.TEXT);
        id.setTypeface(Typeface.MONOSPACE);
        id.setTextIsSelectable(true);
        id.setContentDescription("MaiChat ID " + accountId);
        accountLine.addView(id, new LinearLayout.LayoutParams(0, dp(42), 1));
        TextView copy = iconButton("复制", 15, MaiChatTheme.BLUE_DARK);
        copy.setEnabled(!accountId.isEmpty());
        copy.setOnClickListener(view -> {
            ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            clipboard.setPrimaryClip(ClipData.newPlainText("MaiChat ID", accountId));
            toast("MaiChat ID 已复制");
        });
        accountLine.addView(copy, wrapWrap());

        Button message = primaryButton("发消息");
        message.setContentDescription("向 " + contact.displayName() + " 发消息");
        message.setOnClickListener(view -> {
            activeFriendProfileUserId = null;
            openChat(userId);
        });
        LinearLayout.LayoutParams messageParams = match(dp(52));
        messageParams.topMargin = dp(28);
        details.addView(message, messageParams);

        Button delete = secondaryButton("删除好友");
        delete.setTextColor(MaiChatTheme.RED);
        delete.setOnClickListener(view -> confirm("删除好友？",
            "删除后不再显示在通讯录，已有聊天记录会保留。", "删除好友",
            () -> session.deleteContact(userId)));
        LinearLayout.LayoutParams deleteParams = match(dp(52));
        deleteParams.topMargin = dp(10);
        details.addView(delete, deleteParams);
    }

    // 作用：展示联系人添加表单并处理提交结果。
    private void showAddContactDialog() {
        showAddContactDialog("");
    }

    private void showAddContactDialog(String presetAccount) {
        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(20), dp(20), dp(20), dp(20));
        card.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 16, this));
        card.addView(MaiChatTheme.label(this, "添加好友", 20, MaiChatTheme.TEXT), match(dp(28)));
        TextView description = MaiChatTheme.text(this, "输入手机号或好友账号 ID", 13,
            MaiChatTheme.SECONDARY);
        LinearLayout.LayoutParams descriptionParams = match(dp(24));
        descriptionParams.setMargins(0, dp(2), 0, dp(12));
        card.addView(description, descriptionParams);
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setTextSize(15);
        input.setHint("手机号或账号 ID");
        input.setImeOptions(EditorInfo.IME_ACTION_DONE);
        input.setPadding(dp(14), 0, dp(14), 0);
        input.setBackground(MaiChatTheme.bordered(MaiChatTheme.PAGE, MaiChatTheme.BORDER, 9, this));
        input.setText(presetAccount);
        input.setSelection(input.length());
        card.addView(input, match(dp(44)));

        LinearLayout qrActions = new LinearLayout(this);
        qrActions.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams qrParams = match(dp(46));
        qrParams.setMargins(0, dp(8), 0, 0);
        card.addView(qrActions, qrParams);
        Button scanFriend = secondaryButton("扫一扫");
        Button myCode = secondaryButton("我的二维码");
        qrActions.addView(scanFriend, new LinearLayout.LayoutParams(0, dp(40), 1));
        LinearLayout.LayoutParams myCodeParams = new LinearLayout.LayoutParams(0, dp(40), 1);
        myCodeParams.setMargins(dp(10), 0, 0, 0);
        qrActions.addView(myCode, myCodeParams);
        scanFriend.setOnClickListener(view -> {
            dialog.dismiss();
            startActivityForResult(new Intent(this, FriendQrScanActivity.class), REQUEST_FRIEND_SCAN);
        });
        myCode.setOnClickListener(view -> showMyFriendCodeDialog());

        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams actionsParams = match(dp(46));
        actionsParams.setMargins(0, dp(16), 0, 0);
        card.addView(actions, actionsParams);
        Button cancel = secondaryButton("取消");
        Button add = primaryButton("发送申请");
        LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(0, dp(42), 1);
        actions.addView(cancel, buttonParams);
        LinearLayout.LayoutParams addParams = new LinearLayout.LayoutParams(0, dp(42), 1);
        addParams.setMargins(dp(10), 0, 0, 0);
        actions.addView(add, addParams);
        cancel.setOnClickListener(view -> dialog.dismiss());
        View.OnClickListener submit = view -> {
            String userId = input.getText().toString().trim();
            if (userId.isEmpty()) return;
            add.setEnabled(false);
            session.addContact(userId, added -> {
                add.setEnabled(true);
                if (added) {
                    dialog.dismiss();
                    toast("好友申请已发送");
                }
            });
        };
        add.setOnClickListener(submit);
        input.setOnEditorActionListener((view, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                submit.onClick(view);
                return true;
            }
            return false;
        });
        dialog.setContentView(card);
        dialog.setCanceledOnTouchOutside(true);
        allowKeyboardLocation = false;
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setDimAmount(0.28f);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * 0.86f),
                    ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        input.requestFocus();
        input.post(() -> ((InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
            .showSoftInput(input, InputMethodManager.SHOW_IMPLICIT));
    }

    private void showMyFriendCodeDialog() {
        String userId = session.accountId();
        String link = FriendQrPolicy.link(userId);
        if (link.isEmpty()) { toast("请先登录账号"); return; }
        try {
            int size = 600;
            BitMatrix bits = new QRCodeWriter().encode(link, BarcodeFormat.QR_CODE, size, size);
            Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++)
                    bitmap.setPixel(x, y, bits.get(x, y) ? Color.BLACK : Color.WHITE);
            LinearLayout content = new LinearLayout(this);
            content.setOrientation(LinearLayout.VERTICAL);
            content.setGravity(Gravity.CENTER_HORIZONTAL);
            content.setPadding(dp(20), dp(12), dp(20), dp(8));
            ImageView image = new ImageView(this);
            image.setImageBitmap(bitmap);
            content.addView(image, match(dp(248)));
            TextView account = MaiChatTheme.text(this, userId, 13, MaiChatTheme.SECONDARY);
            account.setGravity(Gravity.CENTER);
            account.setTextIsSelectable(true);
            content.addView(account, match(dp(45)));
            new AlertDialog.Builder(this).setTitle("我的好友二维码")
                .setMessage("让对方扫一扫，或复制下方账号 ID 添加你")
                .setView(content)
                .setPositiveButton("完成", null).show();
        } catch (Exception failure) { toast("无法生成好友二维码"); }
    }

    private void showFriendRequestsDialog() {
        session.friendRequests((requests, error) -> {
            if (destroyed) return;
            if (error != null) { toast(error); return; }
            if (requests.isEmpty()) {
                new AlertDialog.Builder(this).setTitle("好友申请")
                    .setMessage("暂无待处理的好友申请")
                    .setPositiveButton("完成", null).show();
                return;
            }
            ScrollView scroll = new ScrollView(this);
            LinearLayout list = new LinearLayout(this);
            list.setOrientation(LinearLayout.VERTICAL);
            list.setPadding(dp(18), dp(8), dp(18), dp(8));
            scroll.addView(list);
            AlertDialog dialog = new AlertDialog.Builder(this).setTitle("好友申请")
                .setView(scroll).setNegativeButton("关闭", null).create();
            for (RemoteIMFriendRequest request : requests) {
                LinearLayout row = new LinearLayout(this);
                row.setOrientation(LinearLayout.VERTICAL);
                row.setPadding(0, dp(8), 0, dp(12));
                list.addView(row, matchWrap());
                row.addView(MaiChatTheme.label(this, request.displayName, 16, MaiChatTheme.TEXT));
                row.addView(MaiChatTheme.text(this,
                    request.message == null || request.message.isEmpty()
                        ? "请求添加你为好友" : request.message, 13, MaiChatTheme.SECONDARY));
                LinearLayout actions = new LinearLayout(this);
                actions.setGravity(Gravity.END);
                row.addView(actions, match(dp(44)));
                Button refuse = secondaryButton("拒绝");
                Button accept = primaryButton("同意");
                actions.addView(refuse, new LinearLayout.LayoutParams(dp(76), dp(38)));
                LinearLayout.LayoutParams acceptParams =
                    new LinearLayout.LayoutParams(dp(76), dp(38));
                acceptParams.setMargins(dp(8), 0, 0, 0);
                actions.addView(accept, acceptParams);
                refuse.setOnClickListener(view -> session.decideFriendRequest(
                    request.userId, false, done -> {
                        if (done) { dialog.dismiss(); showFriendRequestsDialog(); }
                    }));
                accept.setOnClickListener(view -> session.decideFriendRequest(
                    request.userId, true, done -> {
                        if (done) { dialog.dismiss(); showFriendRequestsDialog(); }
                    }));
            }
            dialog.show();
        });
    }

    private void showNicknameDialog() {
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setHint("昵称");
        input.setText(session.nickname());
        input.setSelection(input.length());
        FrameLayout wrapper = new FrameLayout(this);
        wrapper.setPadding(dp(20), dp(8), dp(20), 0);
        wrapper.addView(input, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(48)));
        AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle("修改昵称")
            .setView(wrapper)
            .setNegativeButton("取消", null)
            .setPositiveButton("保存", null)
            .create();
        dialog.setOnShowListener(ignored -> {
            Button save = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            save.setOnClickListener(view -> {
                String value = input.getText().toString().trim();
                if (value.codePointCount(0, value.length()) > 32
                    || value.getBytes(StandardCharsets.UTF_8).length > 120) {
                    input.setError("昵称最多 32 个字符，且不能超过 120 字节");
                    return;
                }
                save.setEnabled(false);
                session.setNickname(value, error -> {
                    if (!dialog.isShowing()) return;
                    save.setEnabled(true);
                    if (error != null) { input.setError(error); return; }
                    dialog.dismiss();
                    render();
                });
            });
        });
        allowKeyboardLocation = false;
        dialog.show();
    }

    private void renderSettings() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(16), dp(12), dp(16), dp(22));
        scroll.addView(page, matchWrap());
        content.addView(scroll, matchMatch());

        String accountId = session.accountId();
        String accountLabel = compactAccountId(accountId);
        String nickname = session.nickname();
        LinearLayout accountSection = settingsSection("账号", new String[][]{
            {"昵称", nickname.isEmpty() ? "未设置  ›" : nickname + "  ›"},
            {"登录账号", accountLabel + "  复制"},
            {"消息连接", connectionText()}
        });
        page.addView(accountSection, matchWrap());
        ViewGroup accountCard = (ViewGroup) accountSection.getChildAt(1);
        View nicknameRow = accountCard.getChildAt(0);
        nicknameRow.setContentDescription("昵称 " + (nickname.isEmpty() ? "未设置" : nickname));
        nicknameRow.setOnClickListener(view -> showNicknameDialog());
        View accountRow = accountCard.getChildAt(1);
        accountRow.setContentDescription("登录账号 " + accountLabel + "，点击复制完整账号 ID");
        accountRow.setOnClickListener(view -> {
            ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            clipboard.setPrimaryClip(ClipData.newPlainText("MaiChat account ID", accountId));
            toast("完整账号 ID 已复制");
        });
        ViewGroup connectionRow = (ViewGroup) accountCard.getChildAt(2);
        connectionRow.setContentDescription("消息连接状态：" + connectionText());
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(dp(8), dp(8));
        statusParams.setMargins(dp(7), 0, 0, 0);
        connectionRow.addView(statusDot(), statusParams);
        Button friendCode = secondaryButton("我的好友二维码");
        LinearLayout.LayoutParams friendCodeParams = match(dp(48));
        friendCodeParams.setMargins(0, dp(14), 0, 0);
        page.addView(friendCode, friendCodeParams);
        friendCode.setOnClickListener(view -> showMyFriendCodeDialog());
        Button scanDesktopLogin = secondaryButton("扫码登录桌面端");
        LinearLayout.LayoutParams scanParams = match(dp(48));
        scanParams.setMargins(0, dp(18), 0, 0);
        page.addView(scanDesktopLogin, scanParams);
        scanDesktopLogin.setOnClickListener(view ->
            startActivity(new Intent(this, DesktopLoginApprovalActivity.class)));
        Button logout = secondaryButton("切换账号");
        logout.setTextColor(MaiChatTheme.RED);
        LinearLayout.LayoutParams logoutParams = match(dp(48));
        logoutParams.setMargins(0, dp(22), 0, 0);
        page.addView(logout, logoutParams);
        logout.setOnClickListener(view -> {
            try {
                // 先清除旧账号的本机会话，再展示空白 MaiChat 登录页。
                session.logout();
                loginAcceptedTerms = false;
                loginConfirmedAge = false;
                loginUserDraft = "";
                loginCodeDraft = "";
                loginPasswordDraft = "";
                loginChallengeId = "";
                loginUsesPassword = false;
                showInitialLogin = true;
                activeTab = RemoteIMTab.MESSAGES;
                activeChatUserId = null;
                render();
            } catch (IOException error) {
                toast("切换账号失败");
            }
        });
    }

    private void showCloudDrive() {
        if (cloudDriveDialog != null && cloudDriveDialog.isShowing()) return;
        cloudDriveDialog = new MaiCloudDriveDialog(this);
        cloudDriveDialog.show();
    }

    void showCloudSave(File media) {
        showCloudDrive();
        if (cloudDriveDialog != null && cloudDriveDialog.isShowing())
            cloudDriveDialog.confirmAgentFile(media);
    }

    MaiCloudDriveDialog currentCloudDriveDialog() { return cloudDriveDialog; }

    void clearCloudDriveDialog(MaiCloudDriveDialog value) {
        if (cloudDriveDialog == value) cloudDriveDialog = null;
    }

    void pickCloudDriveMedia(MaiCloudDriveDialog requester) {
        if (requester != cloudDriveDialog || !requester.isShowing()) return;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES,
            new String[]{"video/mp4", "video/quicktime", "image/*"});
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
            Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_CLOUD_DRIVE_MEDIA);
    }

    private LinearLayout settingsSection(String title, String[][] rows) {
        LinearLayout section = new LinearLayout(this);
        section.setOrientation(LinearLayout.VERTICAL);
        TextView heading = MaiChatTheme.label(this, title, 13, MaiChatTheme.SECONDARY);
        heading.setPadding(dp(4), 0, 0, dp(7));
        section.addView(heading, match(dp(26)));
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(12), dp(4), dp(12), dp(4));
        card.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 12, this));
        for (String[] row : rows) {
            LinearLayout line = new LinearLayout(this);
            line.setGravity(Gravity.CENTER_VERTICAL);
            line.addView(MaiChatTheme.text(this, row[0], 14, MaiChatTheme.TEXT),
                    new LinearLayout.LayoutParams(0, dp(46), 1));
            TextView value = MaiChatTheme.text(this, row[1], 14, MaiChatTheme.SECONDARY);
            value.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
            value.setSingleLine(true);
            value.setEllipsize(TextUtils.TruncateAt.END);
            line.addView(value, new LinearLayout.LayoutParams(0, dp(46), 1));
            card.addView(line, match(dp(46)));
        }
        if (rows.length > 0) section.addView(card, matchWrap());
        return section;
    }

    private static String compactAccountId(String accountId) {
        if (accountId == null || accountId.length() <= 12) return accountId == null ? "" : accountId;
        return accountId.substring(0, 4) + "…" + accountId.substring(accountId.length() - 4);
    }

    // 作用：根据远控会话状态展示画面、授权和操作入口。
    private void renderRemoteDesktop() {
        RemoteDesktopController controller = session.remoteDesktop();
        if (!controller.isActive()) {
            content.addView(remoteHeader(false), match(dp(44)));
            content.addView(
                emptyState("▣", "没有进行中的远程桌面", "请从聊天详情右上角发起远程查看。"),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1)
            );
            if (controller.state() == RemoteDesktopController.State.FAILED && !controller.error().isEmpty()) {
                toast(controller.error());
            }
            return;
        }

        FrameLayout stage = new FrameLayout(this);
        stage.setBackgroundColor(Color.BLACK);
        content.addView(stage, matchMatch());
        TXCloudVideoView videoView = new TXCloudVideoView(this);
        videoView.setBackgroundColor(Color.BLACK);
        stage.addView(videoView, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));
        controller.bindRenderView(videoView);
        installRemoteGestures(videoView, controller);

        LinearLayout overlay = new LinearLayout(this);
        overlay.setOrientation(LinearLayout.VERTICAL);
        overlay.setGravity(Gravity.BOTTOM);
        stage.addView(overlay, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));
        overlay.addView(remoteHeader(true), match(dp(44)));
        Space spacer = new Space(this);
        overlay.addView(spacer, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));

        if (controller.state() != RemoteDesktopController.State.VIEWING) {
            TextView status = MaiChatTheme.label(this, controller.statusText(), 15, Color.WHITE);
            status.setGravity(Gravity.CENTER);
            stage.addView(status, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(80),
                Gravity.CENTER
            ));
        }
        if (!controller.notice().isEmpty()) {
            TextView notice = MaiChatTheme.label(this, controller.notice(), 12, Color.WHITE);
            notice.setGravity(Gravity.CENTER);
            notice.setBackground(MaiChatTheme.rounded(Color.rgb(234, 128, 12), 8, this));
            FrameLayout.LayoutParams noticeParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                dp(40),
                Gravity.TOP | Gravity.CENTER_HORIZONTAL
            );
            noticeParams.topMargin = dp(48);
            stage.addView(notice, noticeParams);
        }
        if (controller.state() == RemoteDesktopController.State.VIEWING) {
            overlay.addView(remoteControlBar(controller), matchWrap());
        }
    }

    private View remoteHeader(boolean active) {
        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(12), 0, dp(12), 0);
        header.setBackgroundColor(active ? Color.TRANSPARENT : Color.WHITE);
        View dot = new View(this);
        int dotColor;
        switch (session.remoteDesktop().state()) {
            case VIEWING:
                dotColor = MaiChatTheme.GREEN;
                break;
            case INVITING:
            case CONNECTING:
                dotColor = Color.rgb(245, 158, 11);
                break;
            case FAILED:
                dotColor = MaiChatTheme.RED;
                break;
            case IDLE:
            default:
                dotColor = MaiChatTheme.SECONDARY;
                break;
        }
        dot.setBackground(MaiChatTheme.rounded(dotColor, 4, this));
        header.addView(dot, new LinearLayout.LayoutParams(dp(8), dp(8)));
        header.addView(new Space(this), new LinearLayout.LayoutParams(0, 1, 1));
        if (active) {
            TextView close = iconButton("×", 19, Color.WHITE);
            close.setBackground(MaiChatTheme.rounded(Color.RED, 14, this));
            close.setContentDescription("停止远程查看");
            close.setOnClickListener(view -> {
                session.remoteDesktop().stop();
                render();
            });
            LinearLayout.LayoutParams closeParams = new LinearLayout.LayoutParams(dp(28), dp(28));
            closeParams.setMargins(0, dp(8), dp(8), dp(8));
            header.addView(close, closeParams);
        }
        return header;
    }

    private View remoteControlBar(RemoteDesktopController controller) {
        LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.VERTICAL);
        wrapper.setPadding(dp(8), dp(4), dp(8), dp(6));
        wrapper.setBackgroundColor(Color.argb(120, 0, 0, 0));
        LinearLayout controls = new LinearLayout(this);
        controls.setGravity(Gravity.CENTER);
        TextView control = remoteControlButton("◎", controller.isControlEnabled());
        control.setContentDescription(controller.isControlEnabled() ? "停止控制" : "开始控制");
        control.setOnClickListener(view -> {
            controller.setControlEnabled(!controller.isControlEnabled());
            render();
        });
        controls.addView(control, new LinearLayout.LayoutParams(dp(44), dp(44)));
        if (controller.isControlEnabled()) {
            TextView keyboard = remoteControlButton("⌨", false);
            keyboard.setContentDescription("输入远程文字");
            keyboard.setOnClickListener(view -> showRemoteKeyboard(controller));
            LinearLayout.LayoutParams keyboardParams = new LinearLayout.LayoutParams(dp(44), dp(44));
            keyboardParams.setMargins(dp(4), 0, dp(4), 0);
            controls.addView(keyboard, keyboardParams);
            TextView more = remoteControlButton("⋯", false);
            more.setContentDescription("更多远程按键");
            more.setOnClickListener(view -> showRemoteMoreMenu(more, controller));
            controls.addView(more, new LinearLayout.LayoutParams(dp(44), dp(44)));
        }
        wrapper.addView(controls, match(dp(44)));
        return wrapper;
    }

    private void showRemoteKeyboard(RemoteDesktopController controller) {
        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(16), dp(16), dp(12));
        card.setBackground(MaiChatTheme.rounded(Color.rgb(24, 28, 34), 14, this));
        EditText input = new EditText(this);
        input.setTextColor(Color.WHITE);
        input.setHintTextColor(Color.GRAY);
        input.setHint("输入远程文字");
        input.setMinLines(2);
        input.setMaxLines(5);
        input.setGravity(Gravity.TOP | Gravity.START);
        input.setBackground(MaiChatTheme.bordered(Color.rgb(44, 50, 60), Color.rgb(78, 88, 102), 10, this));
        input.setPadding(dp(12), dp(10), dp(12), dp(10));
        card.addView(input, match(dp(96)));
        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.END);
        Button cancel = secondaryButton("取消");
        Button send = primaryButton("发送到远程");
        cancel.setOnClickListener(view -> dialog.dismiss());
        send.setOnClickListener(view -> {
            controller.sendText(input.getText().toString());
            dialog.dismiss();
        });
        actions.addView(cancel, new LinearLayout.LayoutParams(dp(82), dp(42)));
        LinearLayout.LayoutParams sendParams = new LinearLayout.LayoutParams(dp(130), dp(42));
        sendParams.setMargins(dp(8), 0, 0, 0);
        actions.addView(send, sendParams);
        LinearLayout.LayoutParams actionsParams = match(dp(42));
        actionsParams.setMargins(0, dp(10), 0, 0);
        card.addView(actions, actionsParams);
        dialog.setContentView(card);
        allowKeyboardLocation = false;
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * 0.92f),
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                    | WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        }
        input.requestFocus();
    }

    private void showRemoteMoreMenu(View anchor, RemoteDesktopController controller) {
        LinearLayout menu = new LinearLayout(this);
        menu.setOrientation(LinearLayout.VERTICAL);
        menu.setPadding(dp(8), dp(8), dp(8), dp(8));
        menu.setBackground(MaiChatTheme.bordered(Color.rgb(33, 38, 46), Color.rgb(80, 88, 100), 10, this));
        PopupWindow popup = new PopupWindow(menu, dp(190), ViewGroup.LayoutParams.WRAP_CONTENT, true);
        String[] labels = new String[]{
            "Esc", "Tab", "退格", "回车", "↑", "↓", "←", "→", "右键",
            controller.isLeftButtonHeld() ? "松开左键" : "保持左键"
        };
        int[] codes = new int[]{0x1B, 0x09, 0x08, 0x0D, 0x26, 0x28, 0x25, 0x27, -1, -2};
        for (int index = 0; index < labels.length; index += 1) {
            TextView row = MaiChatTheme.text(this, labels[index], 14, Color.WHITE);
            row.setPadding(dp(12), 0, dp(12), 0);
            int code = codes[index];
            row.setOnClickListener(view -> {
                if (code >= 0) controller.sendKey(code);
                else if (code == -1) controller.click(1, 0.5, 0.5);
                else controller.toggleLeftButtonHeld();
                popup.dismiss();
            });
            menu.addView(row, match(dp(40)));
        }
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setOutsideTouchable(true);
        popup.showAsDropDown(anchor, -dp(146), -dp(390));
    }

    @SuppressLint("ClickableViewAccessibility")
    // 作用：把触摸和缩放手势转换为远程桌面输入事件。
    private void installRemoteGestures(TXCloudVideoView view, RemoteDesktopController controller) {
        final float[] scale = {1f};
        final double[][] lastPoint = {new double[]{0.5, 0.5}};
        final float[][] translation = {new float[]{0f, 0f}};
        ScaleGestureDetector scaleDetector =
                new ScaleGestureDetector(this, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                    @Override
                    public boolean onScale(ScaleGestureDetector detector) {
                        if (controller.isControlEnabled())
                            return false;
                        scale[0] = Math.max(1f, Math.min(4f, scale[0] * detector.getScaleFactor()));
                        view.setScaleX(scale[0]);
                        view.setScaleY(scale[0]);
                        if (scale[0] <= 1.01f) {
                            translation[0][0] = 0;
                            translation[0][1] = 0;
                            view.setTranslationX(0);
                            view.setTranslationY(0);
                        }
                        return true;
                    }
                });
        GestureDetector gestures = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDown(MotionEvent event) {
                return true;
            }

            @Override
            public boolean onSingleTapConfirmed(MotionEvent event) {
                double[] point = controller.mapPoint(event.getX(), event.getY(), view.getWidth(), view.getHeight());
                if (point == null) return false;
                lastPoint[0] = point;
                controller.click(0, point[0], point[1]);
                return true;
            }

            @Override
            public boolean onDoubleTap(MotionEvent event) {
                double[] point = controller.mapPoint(event.getX(), event.getY(), view.getWidth(), view.getHeight());
                if (point == null) return false;
                controller.click(0, point[0], point[1]);
                controller.click(0, point[0], point[1]);
                return true;
            }

            @Override
            public boolean onScroll(MotionEvent first, MotionEvent current, float distanceX, float distanceY) {
                if (!controller.isControlEnabled()
                    && scale[0] > 1.01f
                    && current.getPointerCount() < 2) {
                    float maximumX = (view.getWidth() * (scale[0] - 1f)) / 2f;
                    float maximumY = (view.getHeight() * (scale[0] - 1f)) / 2f;
                    translation[0][0] = Math.max(
                        -maximumX,
                        Math.min(maximumX, translation[0][0] - distanceX)
                    );
                    translation[0][1] = Math.max(
                        -maximumY,
                        Math.min(maximumY, translation[0][1] - distanceY)
                    );
                    view.setTranslationX(translation[0][0]);
                    view.setTranslationY(translation[0][1]);
                    return true;
                }
                double[] point = controller.mapPoint(current.getX(), current.getY(), view.getWidth(), view.getHeight());
                if (point == null) return false;
                lastPoint[0] = point;
                if (current.getPointerCount() >= 2) {
                    controller.scroll((int) Math.max(-120, Math.min(120, distanceY)), point[0], point[1]);
                } else {
                    controller.move(point[0], point[1]);
                }
                return true;
            }
        });
        view.setOnTouchListener((target, event) -> {
            scaleDetector.onTouchEvent(event);
            gestures.onTouchEvent(event);
            if (event.getActionMasked() == MotionEvent.ACTION_UP) target.performClick();
            return true;
        });
    }

    private void sendText() {
        if (messageInput == null) return;
        String text = messageInput.getText().toString().trim();
        if (text.isEmpty()) return;
        try {
            RemoteIMMessage queued = session.sendTextMessage(text, pendingQuote);
            if (currentMessageList != null) currentMessageList.requestMessageBottomAfterUpdate(queued.id());
            draftText = "";
            if (messageInput != null) messageInput.setText("");
            pendingQuote = null;
            render();
        } catch (IOException | IllegalStateException error) {
            toast(error.getMessage() == null ? "文本消息发送失败" : error.getMessage());
        }
    }

    private boolean handleVoiceTouch(View view, MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                cancelRecording = false;
                if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
                    startVoiceRecording();
                } else {
                    requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQUEST_RECORD_AUDIO);
                }
                view.setBackground(MaiChatTheme.bordered(MaiChatTheme.BLUE_SOFT, MaiChatTheme.BLUE, 14, this));
                if (view instanceof TextView) ((TextView) view).setText("松开发送，上滑取消");
                return true;
            case MotionEvent.ACTION_MOVE:
                cancelRecording = event.getY() < -dp(70);
                if (view instanceof TextView) {
                    ((TextView) view).setText(cancelRecording ? "松开取消" : "松开发送，上滑取消");
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                view.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 14, this));
                if (view instanceof TextView) ((TextView) view).setText("按住说话");
                if (cancelRecording || event.getActionMasked() == MotionEvent.ACTION_CANCEL) cancelVoiceRecording();
                else finishVoiceRecording();
                if (event.getActionMasked() == MotionEvent.ACTION_UP) view.performClick();
                return true;
            default:
                return false;
        }
    }

    private static final class SendTarget {
        final String peer, owner;
        final int generation;
        final RemoteIMQuote quote;
        SendTarget(RemoteIMSessionController session, String peer, RemoteIMQuote quote) {
            this.peer = peer; this.owner = session.chatState().ownerUserId();
            this.generation = session.accountGeneration(); this.quote = quote;
        }
    }
    private SendTarget sendTarget() {
        return activeChatUserId == null ? null : new SendTarget(session, activeChatUserId, pendingQuote);
    }
    private boolean canSend(SendTarget target) {
        return !destroyed && target != null && session.acceptsOutgoing(target.generation, target.owner, target.peer);
    }
    private void didSend(SendTarget target, RemoteIMMessage message) {
        if (target != null && target.peer.equals(activeChatUserId)) {
            if (java.util.Objects.equals(pendingQuote, target.quote)) pendingQuote = null;
            if (currentMessageList != null) currentMessageList.requestMessageBottomAfterUpdate(message.id());
            if (activityInForeground) render(); else pendingStateRefresh = true;
        }
    }
    private void updateSpeechPreview() {
        if (speechPreview != null) {
            speechPreview.setText(speechText);
            speechPreview.setVisibility(holdingVoice && videoDialog == null
                ? View.VISIBLE : View.GONE);
        }
        if (videoTranscriptOverlay != null) {
            String text = assistantVoiceActive ? assistantVoiceText : speechText;
            videoTranscriptOverlay.setText(text.isEmpty() ? "正在听，请说话…" : text);
            videoTranscriptOverlay.setVisibility(holdingVoice || assistantVoiceActive
                ? View.VISIBLE : View.GONE);
        }
    }

    void setAssistantVoiceTranscript(boolean active, String text) {
        assistantVoiceActive = active;
        assistantVoiceText = text == null ? "" : text;
        updateSpeechPreview();
    }
    private void startVoiceRecording() {
        SendTarget target = sendTarget();
        if (!canSend(target)) return;
        stopAudioPlayback();
        holdingVoice = true; speechText = "开始说话…";
        long uiGeneration = ++voiceUiGeneration;
        updateSpeechPreview();
        voiceRecorder.start(new VoiceRecordingController.Completion() {
            @Override public void partial(String text) {
                if (uiGeneration != voiceUiGeneration || !holdingVoice || !canSend(target)) return;
                speechText = text.length() > 200 ? text.substring(text.offsetByCodePoints(
                                                           0, Math.max(0, text.codePointCount(0, text.length()) - 160)))
                                                 : text;
                updateSpeechPreview(); session.updateHumanTyping(target.peer, true);
            }
            @Override public void recognized(String text, File file, int seconds) {
                if (!canSend(target)) return;
                try {
                    didSend(target, session.sendTextMessageTo(target.peer, text, target.quote));
                    mediaExecutor.execute(file::delete);
                } catch (IOException | RuntimeException error) { sendVoiceFallback(file, seconds, target); }
            }
            @Override public void finished(File file, int seconds) { transcribeThenSend(file, seconds, target); }
            @Override public void failed() {
                if (destroyed) return;
                if (uiGeneration == voiceUiGeneration) { holdingVoice = false; updateSpeechPreview(); }
                toast("录音失败，请重试");
            }
        });
    }
    private void finishVoiceRecording() {
        holdingVoice = false; updateSpeechPreview(); session.stopHumanTyping();
        if (voiceRecorder != null) voiceRecorder.finish(false);
    }

    /**
     * 录完先转文字发文字；识别不可用或失败时回退成发语音消息——用户说过的话不能因为
     * 识别这一环出问题就凭空消失。
     */
    private void transcribeThenSend(java.io.File audioFile, int duration, SendTarget target) {
        if (!canSend(target)) return;
        if (speechRecognizer == null || !speechRecognizer.isAvailable()) {
            sendVoiceFallback(audioFile, duration, target);
            return;
        }
        toast("正在识别…");
        speechRecognizer.transcribe(
                audioFile, audioFile.getName().endsWith(".aac") ? "aac" : "m4a", new SpeechRecognizer.Callback() {
                    @Override
                    public void onText(String text) {
                        if (!canSend(target))
                            return;
                        if (text == null || text.trim().isEmpty()) {
                            toast("没听清，已按语音发送");
                            sendVoiceFallback(audioFile, duration, target);
                            return;
                        }
                        try {
                            RemoteIMMessage queued = session.sendTextMessageTo(target.peer, text.trim(), target.quote);
                            didSend(target, queued);
                        } catch (RuntimeException | IOException error) {
                            toast("识别成功但发送失败，已按语音发送");
                            sendVoiceFallback(audioFile, duration, target);
                            return;
                        }
                        mediaExecutor.execute(audioFile::delete);
                    }

                    @Override
                    public void onError(String message) {
                        if (!canSend(target))
                            return;
                        toast(message == null || message.isEmpty() ? "语音识别失败，已按语音发送" : message);
                        sendVoiceFallback(audioFile, duration, target);
                    }
                });
    }

    private void sendVoiceFallback(File audioFile, int duration, SendTarget target) {
        if (!canSend(target)) return;
        try {
            didSend(target,
                    session.sendVoiceMessageTo(target.peer, audioFile.getAbsolutePath(), duration, target.quote));
        } catch (RuntimeException | IOException error) {
            toast("语音消息发送失败");
        }
    }
    private void cancelVoiceRecording() {
        holdingVoice = false; voiceUiGeneration++; updateSpeechPreview();
        if (session != null) session.stopHumanTyping();
        if (voiceRecorder != null) voiceRecorder.finish(true);
    }

    private void showAttachmentMenu(View anchor) {
        allowKeyboardLocation = false;
        LinearLayout menu = new LinearLayout(this);
        menu.setOrientation(LinearLayout.VERTICAL);
        menu.setPadding(dp(8), dp(8), dp(8), dp(8));
        menu.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 11, this));
        PopupWindow popup = new PopupWindow(menu, dp(190), ViewGroup.LayoutParams.WRAP_CONTENT, true);
        addPopupAction(menu, "▣  拍照发送", () -> {
            popup.dismiss();
            requestCamera();
        });
        addPopupAction(menu, "▧  从相册选择", () -> {
            popup.dismiss();
            openImagePicker();
        });
        addPopupAction(menu, "□  发送文件", () -> {
            popup.dismiss();
            openFilePicker();
        });
        addPopupAction(menu, "✨  AI 帮我回复", () -> {
            popup.dismiss();
            requestAiReplySuggestions();
        });
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setElevation(dp(10));
        popup.setOutsideTouchable(true);
        popup.showAsDropDown(anchor, -dp(146), -dp(216));
    }

    private void addPopupAction(LinearLayout menu, String title, Runnable action) {
        TextView row = MaiChatTheme.text(this, title, 14, MaiChatTheme.TEXT);
        row.setPadding(dp(12), 0, dp(12), 0);
        row.setOnClickListener(view -> action.run());
        menu.addView(row, match(dp(46)));
    }

    private void openImagePicker() {
        pendingAttachmentTarget = sendTarget();
        requestGalleryAccess(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"image/*", "video/*"});
            startActivityForResult(intent, REQUEST_PICK_IMAGE);
        });
    }

    void requestGalleryAccess(Runnable action) {
        if (action == null || destroyed) return;
        if (Build.VERSION.SDK_INT < 33) {
            if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED) {
                action.run();
            } else {
                pendingGalleryAction = action;
                requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE},
                    REQUEST_GALLERY_ACCESS);
            }
            return;
        }
        boolean images = checkSelfPermission(Manifest.permission.READ_MEDIA_IMAGES)
            == PackageManager.PERMISSION_GRANTED;
        boolean videos = checkSelfPermission(Manifest.permission.READ_MEDIA_VIDEO)
            == PackageManager.PERMISSION_GRANTED;
        if (images && videos) {
            action.run();
            return;
        }
        pendingGalleryAction = action;
        String[] permissions = Build.VERSION.SDK_INT >= 34
            ? new String[]{Manifest.permission.READ_MEDIA_IMAGES,
                Manifest.permission.READ_MEDIA_VIDEO,
                Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED}
            : new String[]{Manifest.permission.READ_MEDIA_IMAGES,
                Manifest.permission.READ_MEDIA_VIDEO};
        requestPermissions(permissions, REQUEST_GALLERY_ACCESS);
    }

    private void openFilePicker() {
        pendingAttachmentTarget = sendTarget();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_PICK_FILE);
    }

    private void requestCamera() {
        pendingAttachmentTarget = sendTarget();
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            openCamera();
        } else {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQUEST_CAMERA_PERMISSION);
        }
    }

    private void openCamera() {
        SendTarget target = pendingAttachmentTarget;
        mediaExecutor.execute(() -> {
            try {
                File file = mediaStore.createCameraPhotoFile();
                runOnUiThread(() -> {
                    if (!canSend(target)) return;
                    pendingCameraFile = file;
                    Uri output = FileProvider.getUriForFile(this, getPackageName() + ".files", file);
                    Intent intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
                    intent.putExtra(MediaStore.EXTRA_OUTPUT, output);
                    intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    startActivityForResult(intent, REQUEST_TAKE_PHOTO);
                });
            } catch (IOException error) { runOnUiThread(() -> { if (!destroyed) toast("无法创建拍照文件"); }); }
        });
    }

    private void sendPickedImage(Uri uri) {
        SendTarget target = pendingAttachmentTarget;
        mediaExecutor.execute(() -> {
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                String mime = getContentResolver().getType(uri);
                if (mime != null && mime.startsWith("video/")) {
                    File file = mediaStore.createOutgoingFile(queryDisplayName(uri));
                    copy(input, file); prepareVideo(file, target);
                } else {
                    File file = mediaStore.copyPickedImage(input, uri.getLastPathSegment());
                    prepareImage(file, target);
                }
            } catch (IOException | RuntimeException error) {
                runOnUiThread(() -> {
                    if (!destroyed)
                        toast("图片读取失败");
                });
            }
        });
    }
    private void sendImageFile(File file) {
        SendTarget target = pendingAttachmentTarget;
        mediaExecutor.execute(() -> prepareImage(file, target));
    }
    private void prepareImage(File file, SendTarget target) {
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(file.getAbsolutePath(), options);
        long size = file.length();
        runOnUiThread(() -> {
            if (!canSend(target)) return;
            try {
                didSend(target,
                        session.sendImageMessageTo(target.peer, file.getAbsolutePath(), Math.max(0, options.outWidth),
                                Math.max(0, options.outHeight), size, target.quote));
            } catch (IOException | IllegalStateException error) { toast("图片发送失败"); }
        });
    }
    private void prepareVideo(File file, SendTarget target) throws IOException {
        android.media.MediaMetadataRetriever retriever = new android.media.MediaMetadataRetriever();
        Bitmap cover = null;
        try {
            retriever.setDataSource(file.getAbsolutePath());
            int width = mediaNumber(
                    retriever.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH));
            int height = mediaNumber(
                    retriever.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT));
            int seconds = Math.max(1,
                    mediaNumber(retriever.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_DURATION))
                            / 1000);
            if (Build.VERSION.SDK_INT >= 27)
                cover = retriever.getScaledFrameAtTime(0, android.media.MediaMetadataRetriever.OPTION_CLOSEST_SYNC, 480,
                        Math.max(1, Math.min(480, height * 480 / Math.max(1, width))));
            if (cover == null) {
                cover = Bitmap.createBitmap(160, 90, Bitmap.Config.RGB_565);
                cover.eraseColor(MaiChatTheme.SECONDARY);
            }
            File poster = mediaStore.createOutgoingFile("video-" + java.util.UUID.randomUUID() + ".jpg");
            try (FileOutputStream output = new FileOutputStream(poster)) {
                cover.compress(Bitmap.CompressFormat.JPEG, 85, output);
            }
            RemoteIMVideoAttachment attachment = new RemoteIMVideoAttachment(
                    file.getAbsolutePath(), poster.getAbsolutePath(), seconds, width, height, file.length());
            runOnUiThread(() -> {
                if (!canSend(target)) return;
                try { didSend(target, session.sendVideoMessageTo(target.peer, attachment, target.quote)); }
                catch (IOException | RuntimeException error) { toast("视频发送失败"); }
            });
        } finally {
            if (cover != null) cover.recycle();
            retriever.release();
        }
    }
    private static int mediaNumber(String value) {
        try { return value == null ? 0 : Integer.parseInt(value); } catch (NumberFormatException error) { return 0; }
    }

    private void sendPickedFile(Uri uri) {
        SendTarget target = pendingAttachmentTarget;
        mediaExecutor.execute(() -> {
            String fileName = queryDisplayName(uri);
            String type = getContentResolver().getType(uri);
            String mimeType = type == null || type.trim().isEmpty() ? "application/octet-stream" : type;
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                File file = mediaStore.createOutgoingFile(fileName);
                copy(input, file);
                long size = file.length();
                runOnUiThread(() -> {
                    if (!canSend(target)) return;
                    try {
                        didSend(target,
                                session.sendFileMessageTo(
                                        target.peer, file.getAbsolutePath(), fileName, mimeType, size, target.quote));
                    } catch (IOException | RuntimeException error) {
                        toast("文件发送失败");
                    }
                });
            } catch (IOException | RuntimeException error) {
                runOnUiThread(() -> {
                    if (!destroyed)
                        toast("文件读取失败");
                });
            }
        });
    }

    private String queryDisplayName(Uri uri) {
        try (android.database.Cursor cursor = getContentResolver().query(
            uri,
            new String[]{android.provider.OpenableColumns.DISPLAY_NAME},
            null,
            null,
            null
        )) {
            if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
        } catch (RuntimeException ignored) {
        }
        String name = uri.getLastPathSegment();
        return name == null || name.trim().isEmpty() ? "file" : name;
    }

    private View imageMessageContent(RemoteIMImageAttachment attachment) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        ImageView image = new ImageView(this);
        image.setScaleType(ImageView.ScaleType.CENTER_CROP);
        image.setBackgroundColor(MaiChatTheme.PAGE);
        image.setContentDescription("预览图片");
        image.setOnClickListener(view -> showFullScreenImage(
            attachment.localPath(), imMediaPlaylist(activeChatUserId), view));
        TextView imageMissing = MaiChatTheme.text(this, "图片暂不可预览", 14, MaiChatTheme.SECONDARY);
        imageMissing.setVisibility(View.GONE);
        MessageImageLoader.load(attachment.localPath(), dp(260), dp(190), image, () -> {
            image.setVisibility(View.GONE);
            imageMissing.setVisibility(View.VISIBLE);
        });
        box.addView(imageMissing, matchWrap());
        LinearLayout.LayoutParams imageParams = match(dp(190));
        imageParams.setMargins(0, dp(7), 0, dp(4));
        box.addView(image, imageParams);
        return box;
    }

    private View videoMessageContent(RemoteIMVideoAttachment attachment) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);

        FrameLayout stage = new FrameLayout(this);
        stage.setBackgroundColor(MaiChatTheme.PAGE);
        if (attachment.hasCover()) {
            ImageView image = new ImageView(this);
            image.setScaleType(ImageView.ScaleType.CENTER_CROP);
            MessageImageLoader.load(attachment.coverPath(), dp(260), dp(190), image, null);
            stage.addView(image, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            ));
        }

        // 播放角标压在封面上；没有封面时它就是整个气泡的主体，仍然可点。
        TextView badge = iconButton("▶", 22, Color.WHITE);
        badge.setBackground(MaiChatTheme.rounded(Color.argb(150, 0, 0, 0), 22, this));
        stage.addView(badge, new FrameLayout.LayoutParams(dp(44), dp(44), Gravity.CENTER));

        LinearLayout.LayoutParams stageParams = match(dp(190));
        stageParams.setMargins(0, dp(7), 0, dp(4));
        box.addView(stage, stageParams);

        box.addView(
            MaiChatTheme.text(this, videoSubtitle(attachment), 12, MaiChatTheme.SECONDARY),
            match(dp(22))
        );
        box.setOnClickListener(view -> showVideoPlayer(attachment));
        return box;
    }

    private String videoSubtitle(RemoteIMVideoAttachment attachment) {
        StringBuilder result = new StringBuilder();
        if (attachment.durationSeconds() > 0) {
            result.append(attachment.durationSeconds()).append(" 秒");
        }
        if (attachment.width() > 0 && attachment.height() > 0) {
            if (result.length() > 0) result.append("  ");
            result.append(attachment.width()).append('x').append(attachment.height());
        }
        return result.length() == 0 ? "视频" : result.toString();
    }

    private void showVideoPlayer(RemoteIMVideoAttachment attachment) {
        String peer = activeChatUserId;
        mediaExecutor.execute(() -> {
            boolean exists = new File(attachment.localPath()).isFile();
            runOnUiThread(() -> {
                if (destroyed) return;
                if (exists) openVideoPlayer(attachment.localPath(), 0, imMediaPlaylist(peer));
                else toast("视频还在下载中，稍后再试");
            });
        });
    }

    private List<GalleryItem> imMediaPlaylist(String peer) {
        if (peer == null || session == null) return Collections.emptyList();
        List<GalleryItem> items = new ArrayList<>();
        for (RemoteIMMessage message : session.chatState().messagesWith(peer)) {
            RemoteIMImageAttachment image = message.imageAttachment();
            if (image != null && new File(image.localPath()).isFile())
                items.add(new GalleryItem(image.localPath(), false));
            RemoteIMVideoAttachment video = message.videoAttachment();
            if (video != null && new File(video.localPath()).isFile())
                items.add(new GalleryItem(video.localPath(), true));
        }
        return items;
    }

    private List<GalleryItem> normalizeGallery(String path, boolean video,
                                                List<GalleryItem> candidates) {
        List<GalleryItem> result = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (GalleryItem item : candidates) {
            if (item == null || item.path == null || !new File(item.path).isFile()) continue;
            if (seen.add(item.video + "\0" + item.path)) result.add(item);
        }
        if (seen.add(video + "\0" + path)) result.add(0, new GalleryItem(path, video));
        return result;
    }

    private int galleryIndex(List<GalleryItem> items, String path, boolean video) {
        for (int i = 0; i < items.size(); i++) {
            GalleryItem item = items.get(i);
            if (item.video == video && item.path.equals(path)) return i;
        }
        return 0;
    }

    private void openVideoPlayer(String path) {
        openVideoPlayer(path, 0);
    }

    private void openVideoPlayer(String path, long startPositionUs) {
        openVideoPlayer(path, startPositionUs, Collections.emptyList());
    }

    private void openVideoPlayer(String path, long startPositionUs,
                                 List<GalleryItem> candidates) {
        openVideoPlayer(path, startPositionUs, candidates, 0);
    }

    // 打开指定视频及会话媒体序列，并把初始播放位置交给播放器。
    // 图片交给视频时保留旧帧，待新播放器首帧呈现后再移除。
    private void openVideoPlayer(String path, long startPositionUs,
                                 List<GalleryItem> candidates, int entryDirection) {
        openVideoPlayer(path, startPositionUs, candidates, entryDirection, null);
    }

    private void openVideoPlayer(String path, long startPositionUs,
                                 List<GalleryItem> candidates, int entryDirection,
                                 MaiCloudVod.Session cloudVod) {
        if (videoDialog != null) videoDialog.dismiss();
        String previousImagePath = galleryImagePath;
        Bitmap handoffFrame = entryDirection != 0 && galleryImageView != null
            ? galleryImageView.snapshot() : null;
        if (imageDialog != null) imageDialog.dismiss();
        List<GalleryItem> playlist = normalizeGallery(path, true, candidates);
        final int[] selected = {galleryIndex(playlist, path, true)};
        final String[] currentPath = {path};
        Dialog dialog = new Dialog(this, android.R.style.Theme_Black_NoTitleBar);
        LinearLayout rootView = new LinearLayout(this);
        rootView.setOrientation(LinearLayout.VERTICAL);
        rootView.setBackgroundColor(Color.BLACK);
        FrameLayout frame = new FrameLayout(this);
        rootView.addView(frame, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        MaiFfplayVideoView video = new MaiFfplayVideoView(this);
        video.setControlsBottomInset(cloudVod == null ? 72 : 0);
        if (cloudVod != null) video.setCloudVodHandle(cloudVod.handle());
        video.setVideoPath(path, startPositionUs);
        TextView mediaName = MaiChatTheme.text(this, "", 12, Color.WHITE);
        mediaName.setGravity(Gravity.CENTER);
        mediaName.setSingleLine(true);
        mediaName.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        mediaName.setBackground(MaiChatTheme.rounded(Color.argb(140, 0, 0, 0), 12, this));
        if (playlist.size() > 1) {
            mediaName.setText((selected[0] + 1) + "/" + playlist.size() + " · "
                + new File(path).getName());
        } else {
            mediaName.setVisibility(View.GONE);
        }
        video.setOnVideoSwipeListener(direction -> {
            int next = selected[0] + direction;
            while (next >= 0 && next < playlist.size()
                && !new File(playlist.get(next).path).isFile()) next += direction;
            if (next < 0 || next >= playlist.size()) {
                toast(direction > 0 ? "已经是最后一项" : "已经是第一项");
                return;
            }
            selected[0] = next;
            GalleryItem item = playlist.get(next);
            if (!item.video) {
                showFullScreenImage(item.path, playlist, direction);
                return;
            }
            currentPath[0] = item.path;
            mediaName.setText((next + 1) + "/" + playlist.size() + " · "
                + new File(currentPath[0]).getName());
            video.animate().cancel();
            video.setTranslationX(direction * dp(48));
            video.setAlpha(0.48f);
            video.setVideoPath(currentPath[0]);
            video.animate().translationX(0).alpha(1).setDuration(220).start();
        });
        FrameLayout.LayoutParams videoParams = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT,
            Gravity.CENTER
        );
        frame.addView(video, videoParams);
        View handoffPreview = handoffFrame == null ? null : gallerySnapshot(handoffFrame);
        if (handoffPreview != null)
            frame.addView(handoffPreview, new FrameLayout.LayoutParams(-1, -1));
        FrameLayout.LayoutParams nameParams = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(32), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        nameParams.setMargins(dp(80), dp(20), dp(80), 0);
        frame.addView(mediaName, nameParams);

        TextView transcript = MaiChatTheme.text(this, speechText, 19, Color.WHITE);
        transcript.setContentDescription("实时语音转写");
        transcript.setMaxLines(3);
        transcript.setEllipsize(TextUtils.TruncateAt.START);
        transcript.setPadding(dp(16), dp(12), dp(16), dp(12));
        transcript.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_DARK, 18, this));
        transcript.setVisibility(holdingVoice || assistantVoiceActive
            ? View.VISIBLE : View.GONE);
        FrameLayout.LayoutParams transcriptParams = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        transcriptParams.setMargins(dp(24), dp(72), dp(24), 0);
        frame.addView(transcript, transcriptParams);
        videoTranscriptOverlay = transcript;

        TextView close = iconButton("×", 26, Color.WHITE);
        close.setContentDescription("关闭视频");
        close.setBackground(MaiChatTheme.rounded(Color.argb(175, 55, 55, 55), 25, this));
        close.setOnClickListener(view -> dialog.dismiss());
        FrameLayout.LayoutParams closeParams =
            new FrameLayout.LayoutParams(dp(50), dp(50), Gravity.TOP | Gravity.START);
        closeParams.setMargins(dp(18), dp(16), 0, 0);
        frame.addView(close, closeParams);

        TextView expand = iconButton("⤢", 25, Color.WHITE);
        expand.setContentDescription("放大视频");
        expand.setBackground(MaiChatTheme.rounded(Color.argb(175, 55, 55, 55), 25, this));
        expand.setOnClickListener(view -> {
            videoExpanded = !videoExpanded;
            expand.setText(videoExpanded ? "⤡" : "⤢");
            expand.setContentDescription(videoExpanded ? "返回分屏" : "放大视频");
            updateVideoDockLayout();
        });
        FrameLayout.LayoutParams expandParams =
            new FrameLayout.LayoutParams(dp(50), dp(50), Gravity.TOP | Gravity.END);
        expandParams.setMargins(0, dp(16), dp(18), 0);
        frame.addView(expand, expandParams);

        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        actions.setPadding(dp(16), 0, dp(18), dp(10));
        TextView share = videoAction("↗", "转发或分享");
        share.setOnClickListener(view -> {
            if (cloudVod != null) toast("云盘视频正在点播，无法直接转发");
            else sharePreviewVideo(currentPath[0]);
        });
        actions.addView(share, new LinearLayout.LayoutParams(dp(50), dp(50)));
        TextView save = videoAction("↓", "保存到相册");
        save.setOnClickListener(view -> {
            if (cloudVod != null) toast("云盘视频正在点播，未下载到相册");
            else savePreviewVideo(currentPath[0]);
        });
        LinearLayout.LayoutParams actionSpacing = new LinearLayout.LayoutParams(dp(50), dp(50));
        actionSpacing.leftMargin = dp(12);
        actions.addView(save, actionSpacing);
        TextView copyPath = videoAction("⧉", "复制文件路径");
        copyPath.setOnClickListener(view -> {
            if (cloudVod != null) toast("云盘视频没有本地文件路径");
            else copyToClipboard(currentPath[0]);
        });
        LinearLayout.LayoutParams copySpacing = new LinearLayout.LayoutParams(dp(50), dp(50));
        copySpacing.leftMargin = dp(12);
        actions.addView(copyPath, copySpacing);
        if (cloudVod == null) frame.addView(actions, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(72), Gravity.BOTTOM));

        dialog.setOnDismissListener(d -> {
            MaiCloudVod.Session stream = cloudPlayerStreams.remove(dialog);
            if (stream != null) stream.cancel();
            video.stop();
            if (stream != null) {
                stream.close();
                cloudPlayerOwner = "";
            }
            if (videoDialog == dialog) {
                videoDialog = null;
                videoTranscriptOverlay = null;
                videoExpanded = false;
                updateSpeechPreview();
                updateVideoDockLayout();
            }
        });
        dialog.setContentView(rootView);
        allowKeyboardLocation = false;
        videoExpanded = false;
        videoDialog = dialog;
        dialog.show();
        updateSpeechPreview();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.BLACK));
            window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN);
        }
        updateVideoDockLayout();
        if (handoffPreview != null) {
            Handler handler = new Handler(Looper.getMainLooper());
            long deadline = SystemClock.uptimeMillis() + 8000;
            Runnable[] check = new Runnable[1];
            check[0] = () -> {
                if (videoDialog != dialog || !dialog.isShowing()) return;
                if (video.hasPresentedFrame()) {
                    handoffPreview.animate().alpha(0).setDuration(180)
                        .withEndAction(() -> frame.removeView(handoffPreview)).start();
                } else if (SystemClock.uptimeMillis() >= deadline) {
                    if (previousImagePath != null && new File(previousImagePath).isFile()) {
                        dialog.dismiss();
                        showFullScreenImage(previousImagePath, playlist, 0);
                        toast("视频暂不可播放");
                    } else {
                        handoffPreview.animate().alpha(0).setDuration(180)
                            .withEndAction(() -> frame.removeView(handoffPreview)).start();
                    }
                } else handler.postDelayed(check[0], 40);
            };
            handler.post(check[0]);
        }
        animateGalleryEntry(rootView, entryDirection);
    }

    // Keep one player instance while the conversation remains usable beneath its dock.
    private void updateVideoDockLayout() {
        if (root == null || insetsHost == null) return;
        int width = insetsHost.getWidth();
        int height = insetsHost.getHeight();
        if (width <= 0 || height <= 0) return;
        boolean docked = videoDialog != null && videoDialog.isShowing() && !videoExpanded;
        boolean landscape = width > height && width >= dp(600);
        int dockHeight = docked && !landscape
            ? (keyboardVisible ? dp(240) : Math.min(Math.max((int) (height * 0.62f), dp(320)), dp(560)))
            : 0;
        int dockWidth = docked && landscape
            ? Math.min((int) (width * (keyboardVisible ? 0.42f : 0.52f)), width - dp(340))
            : 0;
        if (landscapeConversationPane != null) {
            int availableWidth = width - dockWidth;
            boolean showSidebar = activeChatUserId == null || availableWidth >= dp(700);
            landscapeConversationPane.setVisibility(showSidebar ? View.VISIBLE : View.GONE);
            if (landscapeConversationDivider != null)
                landscapeConversationDivider.setVisibility(showSidebar ? View.VISIBLE : View.GONE);
            if (showSidebar) {
                int paneDp = Math.min(Math.max((int) (availableWidth /
                    getResources().getDisplayMetrics().density * 0.32f), 280), 360);
                ViewGroup.LayoutParams paneParams = landscapeConversationPane.getLayoutParams();
                if (paneParams != null && paneParams.width != dp(paneDp)) {
                    paneParams.width = dp(paneDp);
                    landscapeConversationPane.setLayoutParams(paneParams);
                }
            }
        }
        FrameLayout.LayoutParams contentParams = (FrameLayout.LayoutParams) root.getLayoutParams();
        if (contentParams != null && (contentParams.topMargin != dockHeight
                || contentParams.leftMargin != dockWidth)) {
            contentParams.topMargin = dockHeight;
            contentParams.leftMargin = dockWidth;
            root.setLayoutParams(contentParams);
        }
        if (videoDialog == null || !videoDialog.isShowing()) return;
        Window window = videoDialog.getWindow();
        if (window == null) return;
        WindowManager.LayoutParams params = window.getAttributes();
        params.gravity = Gravity.TOP | Gravity.START;
        int[] origin = new int[2];
        insetsHost.getLocationOnScreen(origin);
        params.x = origin[0];
        params.y = origin[1];
        params.width = videoExpanded ? width : (landscape ? dockWidth : width);
        params.height = videoExpanded ? height : (landscape ? height : dockHeight);
        window.setAttributes(params);
    }

    private void animateGalleryEntry(View content, int direction) {
        if (direction == 0) return;
        content.setTranslationX(direction * dp(48));
        content.setAlpha(0.48f);
        content.animate().translationX(0).alpha(1).setDuration(220).start();
    }

    private TextView videoAction(String icon, String label) {
        TextView button = iconButton(icon, 26, Color.WHITE);
        button.setContentDescription(label);
        button.setBackground(MaiChatTheme.rounded(Color.argb(105, 255, 255, 255), 25, this));
        return button;
    }

    private void sharePreviewVideo(String path) {
        File source = new File(path);
        if (!source.isFile()) { toast("视频文件已丢失"); return; }
        try {
            Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".files", source);
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("video/*");
            share.putExtra(Intent.EXTRA_STREAM, uri);
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, "转发或分享视频"));
        } catch (Exception error) {
            toast("无法分享视频");
        }
    }

    private void savePreviewVideo(String path) {
        savePreviewVideo(path, null);
    }

    public void saveCloudDriveVideo(String path, Runnable completion) {
        try {
            File source = new File(path).getCanonicalFile();
            File cache = getCacheDir().getCanonicalFile();
            if (!source.getPath().startsWith(cache.getPath() + File.separator)) {
                toast("云盘保存路径无效");
                if (completion != null) completion.run();
                return;
            }
            savePreviewVideo(source.getAbsolutePath(), completion);
        } catch (IOException failure) {
            toast("云盘保存路径无效");
            if (completion != null) completion.run();
        }
    }

    private void savePreviewVideo(String path, Runnable completion) {
        File source = new File(path);
        if (!source.isFile()) {
            toast("视频文件已丢失");
            if (completion != null) completion.run();
            return;
        }
        if (Build.VERSION.SDK_INT < 29) {
            if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
                pendingVideoSavePath = path;
                pendingVideoSaveCompletion = completion;
                requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE},
                    REQUEST_SAVE_VIDEO_PERMISSION);
                return;
            }
            saveLegacyPreviewVideo(source, completion);
            return;
        }
        mediaExecutor.execute(() -> {
            Uri target = null;
            try {
                String extension = source.getName().toLowerCase(Locale.US).endsWith(".mov")
                    ? ".mov" : ".mp4";
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, "MaiChat-"
                    + new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date())
                    + extension);
                values.put(MediaStore.MediaColumns.MIME_TYPE,
                    extension.equals(".mov") ? "video/quicktime" : "video/mp4");
                values.put(MediaStore.MediaColumns.RELATIVE_PATH,
                    Environment.DIRECTORY_MOVIES + "/MaiChat");
                values.put(MediaStore.Video.Media.DATE_TAKEN, System.currentTimeMillis());
                values.put(MediaStore.MediaColumns.IS_PENDING, 1);
                target = getContentResolver().insert(
                    MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values);
                if (target == null) throw new IOException("Cannot create gallery video");
                try (InputStream input = new FileInputStream(source);
                     OutputStream output = getContentResolver().openOutputStream(target)) {
                    if (output == null) throw new IOException("Cannot write gallery video");
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                }
                values.clear();
                values.put(MediaStore.MediaColumns.IS_PENDING, 0);
                getContentResolver().update(target, values, null, null);
                runOnUiThread(() -> toast("已保存到相册"));
            } catch (Exception error) {
                if (target != null) getContentResolver().delete(target, null, null);
                runOnUiThread(() -> toast("保存视频失败"));
            } finally {
                if (completion != null) completion.run();
            }
        });
    }

    private void saveLegacyPreviewVideo(File source, Runnable completion) {
        mediaExecutor.execute(() -> {
            File destination = null;
            try {
                File directory = new File(Environment.getExternalStoragePublicDirectory(
                    Environment.DIRECTORY_MOVIES), "MaiChat");
                if (!directory.isDirectory() && !directory.mkdirs())
                    throw new IOException("Cannot create gallery directory");
                String extension = source.getName().toLowerCase(Locale.US).endsWith(".mov")
                    ? ".mov" : ".mp4";
                destination = new File(directory, "MaiChat-"
                    + new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date())
                    + extension);
                try (InputStream input = new FileInputStream(source);
                     OutputStream output = new FileOutputStream(destination)) {
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                }
                destination.setLastModified(System.currentTimeMillis());
                MediaScannerConnection.scanFile(this,
                    new String[]{destination.getAbsolutePath()},
                    new String[]{extension.equals(".mov") ? "video/quicktime" : "video/mp4"},
                    null);
                runOnUiThread(() -> toast("视频已保存"));
            } catch (Exception error) {
                if (destination != null) destination.delete();
                runOnUiThread(() -> toast("保存视频失败"));
            } finally {
                if (completion != null) completion.run();
            }
        });
    }

    private String previewImageExtension(File source) {
        String name = source.getName().toLowerCase(Locale.US);
        for (String extension : new String[]{".jpeg", ".jpg", ".png", ".webp", ".gif", ".heic"})
            if (name.endsWith(extension)) return extension;
        return ".png";
    }

    private String previewImageMime(String extension) {
        if (extension.equals(".jpeg") || extension.equals(".jpg")) return "image/jpeg";
        if (extension.equals(".webp")) return "image/webp";
        if (extension.equals(".gif")) return "image/gif";
        if (extension.equals(".heic")) return "image/heic";
        return "image/png";
    }

    private void sharePreviewImage(String path) {
        File source = new File(path);
        if (!source.isFile()) { toast("图片文件已丢失"); return; }
        try {
            Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".files", source);
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType(previewImageMime(previewImageExtension(source)));
            share.putExtra(Intent.EXTRA_STREAM, uri);
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, "转发或分享图片"));
        } catch (Exception error) {
            toast("无法分享图片");
        }
    }

    private void savePreviewImage(String path) {
        File source = new File(path);
        if (!source.isFile()) { toast("图片文件已丢失"); return; }
        if (Build.VERSION.SDK_INT < 29) {
            if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
                pendingImageSavePath = path;
                requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE},
                    REQUEST_SAVE_IMAGE_PERMISSION);
                return;
            }
            saveLegacyPreviewImage(source);
            return;
        }
        mediaExecutor.execute(() -> {
            Uri target = null;
            try {
                String extension = previewImageExtension(source);
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, "MaiChat-"
                    + new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date())
                    + extension);
                values.put(MediaStore.MediaColumns.MIME_TYPE, previewImageMime(extension));
                values.put(MediaStore.MediaColumns.RELATIVE_PATH,
                    Environment.DIRECTORY_PICTURES + "/MaiChat");
                values.put(MediaStore.Images.Media.DATE_TAKEN, System.currentTimeMillis());
                values.put(MediaStore.MediaColumns.IS_PENDING, 1);
                target = getContentResolver().insert(
                    MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values);
                if (target == null) throw new IOException("Cannot create gallery image");
                try (InputStream input = new FileInputStream(source);
                     OutputStream output = getContentResolver().openOutputStream(target)) {
                    if (output == null) throw new IOException("Cannot write gallery image");
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                }
                values.clear();
                values.put(MediaStore.MediaColumns.IS_PENDING, 0);
                getContentResolver().update(target, values, null, null);
                runOnUiThread(() -> toast("已保存到相册"));
            } catch (Exception error) {
                if (target != null) getContentResolver().delete(target, null, null);
                runOnUiThread(() -> toast("保存图片失败"));
            }
        });
    }

    private void saveLegacyPreviewImage(File source) {
        mediaExecutor.execute(() -> {
            File destination = null;
            try {
                File directory = new File(Environment.getExternalStoragePublicDirectory(
                    Environment.DIRECTORY_PICTURES), "MaiChat");
                if (!directory.isDirectory() && !directory.mkdirs())
                    throw new IOException("Cannot create gallery directory");
                String extension = previewImageExtension(source);
                destination = new File(directory, "MaiChat-"
                    + new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date())
                    + extension);
                try (InputStream input = new FileInputStream(source);
                     OutputStream output = new FileOutputStream(destination)) {
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                }
                destination.setLastModified(System.currentTimeMillis());
                MediaScannerConnection.scanFile(this,
                    new String[]{destination.getAbsolutePath()},
                    new String[]{previewImageMime(extension)}, null);
                runOnUiThread(() -> toast("已保存到相册"));
            } catch (Exception error) {
                if (destination != null) destination.delete();
                runOnUiThread(() -> toast("保存图片失败"));
            }
        });
    }

    public boolean openAgentVideo(String path) {
        return openAgentVideo(path, 0);
    }

    public boolean openAgentVideo(String path, List<String> playlist) {
        if (destroyed || path == null || !new File(path).isFile()) return false;
        List<GalleryItem> items = new ArrayList<>();
        for (String candidate : playlist) items.add(new GalleryItem(candidate, true));
        openVideoPlayer(path, 0, items);
        return true;
    }

    public boolean openAgentMedia(String path, boolean video, List<GalleryItem> playlist) {
        if (destroyed || path == null || !new File(path).isFile()) return false;
        if (video) openVideoPlayer(path, 0, playlist);
        else showFullScreenImage(path, playlist);
        return true;
    }

    public boolean openCloudDriveVideo(MaiCloudVod.Session source) {
        if (destroyed || source == null || source.handle() == 0) return false;
        String account = MaiChatAuthClient.shared(this).accountId();
        if (account.isEmpty()) return false;
        openVideoPlayer(source.displayPath, 0, Collections.emptyList(), 0, source);
        if (videoDialog == null) return false;
        cloudPlayerStreams.put(videoDialog, source);
        cloudPlayerOwner = account;
        return true;
    }

    public boolean openCloudDriveImage(String path) {
        if (destroyed || path == null) return false;
        try {
            String account = MaiChatAuthClient.shared(this).accountId();
            if (account.isEmpty()) return false;
            File source = new File(path).getCanonicalFile();
            File cache = new File(getCacheDir(), "cloud-drive-images").getCanonicalFile();
            if (!source.isFile() || !source.getParentFile().equals(cache)) return false;
            if (imageDialog != null) imageDialog.dismiss();
            showFullScreenImage(source.getAbsolutePath());
            if (imageDialog == null) return false;
            cloudImageFiles.put(imageDialog, source);
            cloudImageOwner = account;
            return true;
        } catch (IOException failure) { return false; }
    }

    void cleanupIdleCloudDriveImages() {
        File directory = new File(getCacheDir(), "cloud-drive-images");
        File active = imageDialog == null ? null : cloudImageFiles.get(imageDialog);
        File[] files = directory.listFiles();
        if (files == null) return;
        for (File file : files) {
            if (file.isFile() && !file.equals(active)) file.delete();
        }
    }

    public boolean openAgentVideo(String path, long startPositionUs) {
        if (destroyed || path == null || !new File(path).isFile()) return false;
        openVideoPlayer(path, startPositionUs);
        return true;
    }

    public boolean commandAgentVideo(String action, double percent, double speed,
                                     double seconds) {
        if (action.equals("close")) {
            if (videoDialog == null) return false;
            videoDialog.dismiss();
            return true;
        }
        MaiFfplayVideoView video = MaiFfplayVideoView.activeView();
        if (video == null) return false;
        if (action.equals("seek_percent")) return video.seekPercent(percent / 100.0);
        if (action.equals("seek_seconds"))
            return seconds == 0 ? video.replay() : video.seekSeconds(seconds);
        if (action.equals("replay")) return video.replay();
        if (action.equals("set_speed")) return video.setSpeed(speed);
        return video.command(action);
    }

    private View fileMessageContent(RemoteIMFileAttachment attachment) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(0, dp(7), 0, dp(3));
        TextView title = MaiChatTheme.label(
            this,
            (RemoteIMGitDiffDisplayPolicy.isGitDiff(attachment) ? "Δ  " : "□  ") + attachment.fileName(),
            15,
            MaiChatTheme.BLUE_DARK
        );
        title.setSingleLine(true);
        title.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        box.addView(title, match(dp(26)));
        TextView detail = MaiChatTheme.text(this, fileSubtitle(attachment), 12, MaiChatTheme.SECONDARY);
        box.addView(detail, match(dp(22)));
        box.setOnClickListener(view -> showFilePreview(attachment));
        return box;
    }

    @SuppressLint("ClickableViewAccessibility")
    public void showFullScreenImage(String path) {
        showFullScreenImage(path, Collections.emptyList());
    }

    @SuppressLint("ClickableViewAccessibility")
    private void showFullScreenImage(String path, List<GalleryItem> candidates) {
        showFullScreenImage(path, candidates, 0);
    }

    private void showFullScreenImage(String path, List<GalleryItem> candidates, View source) {
        showFullScreenImage(path, candidates, 0, source);
    }

    @SuppressLint("ClickableViewAccessibility")
    // 将选中的图片放入全屏媒体序列，接收左右滑动以导航相邻图片或视频。
    // 封面、来源路径和序列索引在进入前确定，避免滑动时拿错文件。
    private void showFullScreenImage(String path, List<GalleryItem> candidates,
                                     int entryDirection) {
        showFullScreenImage(path, candidates, entryDirection, null);
    }

    private void showFullScreenImage(String path, List<GalleryItem> candidates,
                                     int entryDirection, View source) {
        boolean reuseDialog = imageDialog != null && imageDialog.isShowing();
        Rect sourceBounds = new Rect();
        Bitmap sourceBitmap = !reuseDialog && videoDialog == null && source != null
            && source.getGlobalVisibleRect(sourceBounds)
            ? snapshotPreviewSource(source) : null;
        boolean bubbleEntry = sourceBitmap != null && !sourceBounds.isEmpty();
        String previousPath = galleryImagePath;
        Bitmap previousFrame = reuseDialog && entryDirection != 0 && galleryImageView != null
            ? galleryImageView.snapshot() : null;
        if (previousFrame == null && entryDirection != 0 && videoDialog != null) {
            MaiFfplayVideoView previousVideo = MaiFfplayVideoView.activeView();
            if (previousVideo != null) previousFrame = previousVideo.snapshot();
        }
        if (videoDialog != null) videoDialog.dismiss();
        List<GalleryItem> playlist = normalizeGallery(path, false, candidates);
        int selected = galleryIndex(playlist, path, false);
        Dialog dialog = reuseDialog ? imageDialog :
            new Dialog(this, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(bubbleEntry ? Color.TRANSPARENT : Color.BLACK);
        View dim = new View(this);
        dim.setBackgroundColor(Color.BLACK);
        dim.setAlpha(bubbleEntry ? 0.68f : 1f);
        frame.addView(dim, new FrameLayout.LayoutParams(-1, -1));
        // 同一入口供 IM 与 AI 助手共用：背景铺满屏幕，前景仍完整显示原图。
        MaiGraphicsImageView backdrop = new MaiGraphicsImageView(this);
        backdrop.showImage(path, true, () -> { });
        backdrop.setAlpha(bubbleEntry ? 0f : 0.48f);
        if (Build.VERSION.SDK_INT >= 31) {
            backdrop.setRenderEffect(android.graphics.RenderEffect.createBlurEffect(
                dp(24), dp(24), android.graphics.Shader.TileMode.CLAMP));
        }
        frame.addView(backdrop, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        View previousPreview = previousFrame == null ? null : gallerySnapshot(previousFrame);
        MaiGraphicsImageView image = new MaiGraphicsImageView(this);
        if (bubbleEntry) image.setAlpha(0f);
        final ImageView[] entryOverlay = {null};
        List<View> entryChrome = new ArrayList<>();
        final boolean[] transitioned = {false};
        image.setPresentationListener(success -> {
            if (imageDialog != dialog || galleryImageView != image || transitioned[0]) return;
            // 图片解码完成后才让实际画面滑入，避免只动了加载中的黑底。
            if (success) {
                transitioned[0] = true;
                if (bubbleEntry) {
                    dim.animate().alpha(1f).setDuration(260).start();
                    backdrop.animate().alpha(0.48f).setStartDelay(100)
                        .setDuration(160).start();
                    image.animate().alpha(1f).setDuration(260).start();
                    for (View chrome : entryChrome)
                        chrome.animate().alpha(1f).setDuration(260).start();
                    if (entryOverlay[0] != null) {
                        ImageView snapshot = entryOverlay[0];
                        snapshot.animate().scaleX(1f).scaleY(1f)
                            .translationX(0f).translationY(0f).alpha(0f)
                            .setDuration(260).withEndAction(() -> frame.removeView(snapshot)).start();
                    }
                } else animateGalleryEntry(image, entryDirection);
                if (previousPreview != null) {
                    previousPreview.animate().translationX(-entryDirection * dp(48))
                        .alpha(0).setDuration(220)
                        .withEndAction(() -> frame.removeView(previousPreview)).start();
                }
            }
        });
        // 全屏预览直接绑定自己的 TextureView，与气泡使用不同的渲染目标。
        image.showImage(
            path,
            false,
            () -> {
                if (imageDialog != dialog || galleryImageView != image) return;
                toast("图片暂不可预览");
                if (entryDirection != 0 && previousPath != null && !previousPath.equals(path))
                    showFullScreenImage(previousPath, candidates, 0);
                else dialog.dismiss();
            });
        final float[] scale = {1f};
        final float[] last = {0f, 0f};
        ScaleGestureDetector zoom = new ScaleGestureDetector(this,
            new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                @Override
                public boolean onScale(ScaleGestureDetector detector) {
                    scale[0] = Math.max(1f, Math.min(4f,
                        scale[0] * detector.getScaleFactor()));
                    image.setScaleX(scale[0]);
                    image.setScaleY(scale[0]);
                    float maxX = image.getWidth() * (scale[0] - 1f) / 2f;
                    float maxY = image.getHeight() * (scale[0] - 1f) / 2f;
                    image.setTranslationX(Math.max(-maxX,
                        Math.min(maxX, image.getTranslationX())));
                    image.setTranslationY(Math.max(-maxY,
                        Math.min(maxY, image.getTranslationY())));
                    return true;
                }
            });
        image.setOnTouchListener((view, event) -> {
            zoom.onTouchEvent(event);
            if (event.getActionMasked() == MotionEvent.ACTION_POINTER_UP) {
                last[0] = event.getX(0);
                last[1] = event.getY(0);
                return true;
            }
            if (event.getPointerCount() > 1) return true;
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    last[0] = event.getX();
                    last[1] = event.getY();
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (scale[0] <= 1.01f) return true;
                    float maxX = image.getWidth() * (scale[0] - 1f) / 2f;
                    float maxY = image.getHeight() * (scale[0] - 1f) / 2f;
                    image.setTranslationX(Math.max(-maxX,
                        Math.min(maxX, image.getTranslationX() + event.getX() - last[0])));
                    image.setTranslationY(Math.max(-maxY,
                        Math.min(maxY, image.getTranslationY() + event.getY() - last[1])));
                    last[0] = event.getX();
                    last[1] = event.getY();
                    return true;
                case MotionEvent.ACTION_UP:
                    if (scale[0] <= 1.01f) {
                        float dx = event.getX() - last[0];
                        float dy = event.getY() - last[1];
                        if (Math.abs(dx) >= dp(72) && Math.abs(dx) > Math.abs(dy) * 1.4f)
                            showMediaNeighbor(playlist, selected, dx < 0 ? 1 : -1);
                    }
                    return true;
                default:
                    return true;
            }
        });
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(path, bounds);
        FrameLayout.LayoutParams imageParams;
        if (bounds.outWidth > 0 && bounds.outHeight > 0) {
            android.util.DisplayMetrics display = getResources().getDisplayMetrics();
            float fit = Math.min((float) display.widthPixels / bounds.outWidth,
                (float) display.heightPixels / bounds.outHeight);
            imageParams = new FrameLayout.LayoutParams(
                Math.max(1, Math.round(bounds.outWidth * fit)),
                Math.max(1, Math.round(bounds.outHeight * fit)), Gravity.CENTER);
        } else {
            imageParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT);
        }
        android.util.DisplayMetrics display = getResources().getDisplayMetrics();
        int targetWidth = imageParams.width > 0 ? imageParams.width : display.widthPixels;
        int targetHeight = imageParams.height > 0 ? imageParams.height : display.heightPixels;
        frame.addView(image, imageParams);
        if (previousPreview != null)
            frame.addView(previousPreview, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        if (bubbleEntry) {
            ImageView snapshot = new ImageView(this);
            snapshot.setImageBitmap(sourceBitmap);
            snapshot.setScaleType(ImageView.ScaleType.FIT_XY);
            frame.addView(snapshot, new FrameLayout.LayoutParams(
                targetWidth, targetHeight, Gravity.CENTER));
            snapshot.setScaleX((float) sourceBounds.width() / targetWidth);
            snapshot.setScaleY((float) sourceBounds.height() / targetHeight);
            snapshot.setTranslationX(sourceBounds.exactCenterX() - display.widthPixels / 2f);
            snapshot.setTranslationY(sourceBounds.exactCenterY() - display.heightPixels / 2f);
            entryOverlay[0] = snapshot;
        }
        if (playlist.size() > 1) {
            TextView mediaName = MaiChatTheme.text(this,
                (selected + 1) + "/" + playlist.size() + " · " + new File(path).getName(),
                12, Color.WHITE);
            mediaName.setGravity(Gravity.CENTER);
            mediaName.setSingleLine(true);
            mediaName.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            mediaName.setBackground(MaiChatTheme.rounded(Color.argb(140, 0, 0, 0), 12, this));
            FrameLayout.LayoutParams nameParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(32), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
            nameParams.setMargins(dp(80), dp(20), dp(80), 0);
            frame.addView(mediaName, nameParams);
            if (bubbleEntry) { mediaName.setAlpha(0f); entryChrome.add(mediaName); }
        }
        TextView close = iconButton("×", 26, Color.WHITE);
        close.setContentDescription("关闭图片预览");
        close.setBackground(MaiChatTheme.rounded(Color.argb(160, 0, 0, 0), 20, this));
        final boolean[] closing = {false};
        close.setOnClickListener(view -> {
            if (!bubbleEntry || !transitioned[0] || source == null || !source.isAttachedToWindow()
                || imageDialog != dialog || galleryImageView != image) {
                dialog.dismiss();
                return;
            }
            Rect destination = new Rect();
            if (closing[0] || !source.getGlobalVisibleRect(destination)) {
                if (!closing[0]) dialog.dismiss();
                return;
            }
            Bitmap current = image.snapshot();
            if (current == null) current = sourceBitmap;
            if (current == null) { dialog.dismiss(); return; }
            closing[0] = true;
            ImageView shrink = new ImageView(this);
            shrink.setImageBitmap(current);
            shrink.setScaleType(ImageView.ScaleType.FIT_XY);
            frame.addView(shrink, new FrameLayout.LayoutParams(
                targetWidth, targetHeight, Gravity.CENTER));
            image.setAlpha(0f);
            dim.animate().alpha(0f).setDuration(220).start();
            backdrop.animate().alpha(0f).setDuration(220).start();
            for (View chrome : entryChrome) chrome.animate().alpha(0f).setDuration(160).start();
            shrink.animate()
                .scaleX((float) destination.width() / targetWidth)
                .scaleY((float) destination.height() / targetHeight)
                .translationX(destination.exactCenterX() - display.widthPixels / 2f)
                .translationY(destination.exactCenterY() - display.heightPixels / 2f)
                .setDuration(220).withEndAction(dialog::dismiss).start();
        });
        FrameLayout.LayoutParams closeParams = new FrameLayout.LayoutParams(
            dp(48), dp(48), Gravity.TOP | Gravity.START);
        closeParams.setMargins(dp(20), dp(18), 0, 0);
        frame.addView(close, closeParams);
        if (bubbleEntry) { close.setAlpha(0f); entryChrome.add(close); }
        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.CENTER_VERTICAL);
        actions.setPadding(dp(6), 0, dp(6), 0);
        actions.setBackground(MaiChatTheme.rounded(Color.argb(170, 0, 0, 0), 26, this));
        TextView share = iconButton("↗", 23, Color.WHITE);
        share.setContentDescription("分享图片");
        share.setOnClickListener(view -> sharePreviewImage(path));
        actions.addView(share, new LinearLayout.LayoutParams(dp(48), dp(48)));
        TextView save = iconButton("↓", 25, Color.WHITE);
        save.setContentDescription("保存图片");
        save.setOnClickListener(view -> savePreviewImage(path));
        actions.addView(save, new LinearLayout.LayoutParams(dp(48), dp(48)));
        FrameLayout.LayoutParams actionParams = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, dp(52), Gravity.BOTTOM | Gravity.END);
        actionParams.setMargins(0, 0, dp(20), dp(54));
        frame.addView(actions, actionParams);
        if (bubbleEntry) { actions.setAlpha(0f); entryChrome.add(actions); }
        dialog.setContentView(frame);
        if (bubbleEntry && dialog.getWindow() != null) {
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            dialog.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        }
        if (bubbleEntry)
            dialog.setOnKeyListener((ignored, key, event) -> {
                if (key != KeyEvent.KEYCODE_BACK) return false;
                if (event.getAction() == KeyEvent.ACTION_UP) close.performClick();
                return true;
            });
        else dialog.setOnKeyListener(null);
        dialog.setOnDismissListener(ignored -> {
            File cloudFile = cloudImageFiles.remove(dialog);
            if (cloudFile != null) {
                cloudFile.delete();
                cloudImageOwner = "";
            }
            if (imageDialog == dialog) {
                imageDialog = null;
                galleryImageView = null;
                galleryImagePath = null;
            }
        });
        allowKeyboardLocation = false;
        imageDialog = dialog;
        galleryImageView = image;
        galleryImagePath = path;
        if (!reuseDialog) dialog.show();
    }

    private Bitmap snapshotPreviewSource(View source) {
        if (source.getWidth() <= 0 || source.getHeight() <= 0) return null;
        Bitmap bitmap = Bitmap.createBitmap(
            source.getWidth(), source.getHeight(), Bitmap.Config.ARGB_8888);
        source.draw(new Canvas(bitmap));
        return bitmap;
    }

    private View gallerySnapshot(Bitmap bitmap) {
        FrameLayout snapshot = new FrameLayout(this);
        snapshot.setBackgroundColor(Color.BLACK);
        ImageView backdrop = new ImageView(this);
        backdrop.setImageBitmap(bitmap);
        backdrop.setScaleType(ImageView.ScaleType.CENTER_CROP);
        backdrop.setAlpha(0.48f);
        if (Build.VERSION.SDK_INT >= 31)
            backdrop.setRenderEffect(android.graphics.RenderEffect.createBlurEffect(
                dp(24), dp(24), android.graphics.Shader.TileMode.CLAMP));
        snapshot.addView(backdrop, new FrameLayout.LayoutParams(-1, -1));
        ImageView foreground = new ImageView(this);
        foreground.setImageBitmap(bitmap);
        foreground.setScaleType(ImageView.ScaleType.FIT_CENTER);
        snapshot.addView(foreground, new FrameLayout.LayoutParams(-1, -1));
        return snapshot;
    }

    private void showMediaNeighbor(List<GalleryItem> playlist, int selected, int direction) {
        int next = selected + direction;
        while (next >= 0 && next < playlist.size()
            && !new File(playlist.get(next).path).isFile()) next += direction;
        if (next < 0 || next >= playlist.size()) {
            toast(direction > 0 ? "已经是最后一项" : "已经是第一项");
            return;
        }
        GalleryItem item = playlist.get(next);
        if (item.video) openVideoPlayer(item.path, 0, playlist, direction);
        else showFullScreenImage(item.path, playlist, direction);
    }

    // 作用：根据附件类型展示可打开的文件预览。
    private void showFilePreview(RemoteIMFileAttachment attachment) {
        if (RemoteIMGitDiffDisplayPolicy.isGitDiff(attachment)
            && !RemoteIMGitDiffDisplayPolicy.hasValidIntegrity(attachment)) {
            Toast.makeText(this, "Diff 文件 SHA256 校验失败，已停止渲染", Toast.LENGTH_LONG).show();
            return;
        }
        String mime = attachment.mimeType().toLowerCase(Locale.ROOT);
        String name = attachment.fileName().toLowerCase(Locale.ROOT);
        if (mime.equals("application/pdf") || name.endsWith(".pdf")) {
            PdfPreviewDialog.show(this, new File(attachment.localPath()), null,
                attachment.fileName());
            return;
        }
        if (mime.contains("html") || name.endsWith(".html") || name.endsWith(".htm")) {
            Dialog dialog = previewDialog(
                RemoteIMGitDiffDisplayPolicy.isGitDiff(attachment) ? "代码 Diff" : attachment.fileName()
            );
            WebView web = new WebView(this);
            web.getSettings().setJavaScriptEnabled(false);
            mediaExecutor.execute(() -> {
                String html = readTextFile(attachment.localPath());
                runOnUiThread(() -> {
                    if (!destroyed && dialog.isShowing())
                        web.loadDataWithBaseURL(new File(attachment.localPath()).getParentFile().toURI().toString(),
                                html, "text/html", "utf-8", null);
                });
            });
            addPreviewContent(dialog, web);
            return;
        }
        if (mime.contains("markdown") || name.endsWith(".md") || name.endsWith(".markdown")
                || mime.startsWith("text/")) {
            ScrollView scroll = new ScrollView(this);
            TextView text = MaiChatTheme.text(this, "", 14, MaiChatTheme.TEXT);
            text.setText("正在打开…");
            text.setTextIsSelectable(true);
            text.setPadding(dp(16), dp(14), dp(16), dp(18));
            scroll.addView(text, matchWrap());
            Dialog dialog = previewDialog(attachment.fileName());
            addPreviewContent(dialog, scroll);
            mediaExecutor.execute(() -> {
                String source = readTextFile(attachment.localPath());
                MarkdownRenderer.prepare(Collections.singletonList(source), () -> !destroyed, () -> {
                    if (dialog.isShowing()) MarkdownRenderer.bind(text, source);
                });
            });
            return;
        }
        try {
            Uri uri = FileProvider.getUriForFile(
                this,
                getPackageName() + ".files",
                new File(attachment.localPath())
            );
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(uri, attachment.mimeType());
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(intent);
        } catch (RuntimeException error) {
            toast("没有可预览此文件的应用");
        }
    }

    private Dialog previewDialog(String title) {
        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout frame = new LinearLayout(this);
        frame.setId(android.R.id.content);
        frame.setOrientation(LinearLayout.VERTICAL);
        frame.setBackgroundColor(Color.WHITE);
        TextView header = MaiChatTheme.label(this, title, 17, MaiChatTheme.TEXT);
        header.setPadding(dp(16), 0, dp(16), 0);
        frame.addView(header, match(dp(52)));
        dialog.setContentView(frame);
        allowKeyboardLocation = false;
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setLayout(
                (int) (getResources().getDisplayMetrics().widthPixels * 0.94f),
                (int) (getResources().getDisplayMetrics().heightPixels * 0.88f)
            );
        }
        return dialog;
    }

    private void addPreviewContent(Dialog dialog, View contentView) {
        Window window = dialog.getWindow();
        if (window == null) return;
        View rootView = window.getDecorView().findViewById(android.R.id.content);
        if (!(rootView instanceof ViewGroup)) return;
        ViewGroup root = (ViewGroup) rootView;
        if (root.getChildCount() == 0 || !(root.getChildAt(0) instanceof LinearLayout)) return;
        LinearLayout frame = (LinearLayout) root.getChildAt(0);
        frame.addView(contentView, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));
        Button close = secondaryButton("关闭");
        close.setOnClickListener(view -> dialog.dismiss());
        LinearLayout.LayoutParams closeParams = match(dp(46));
        closeParams.setMargins(dp(16), dp(8), dp(16), dp(12));
        frame.addView(close, closeParams);
    }

    private void toggleVoicePlayback(RemoteIMMessage message) {
        if (message.voiceAttachment() == null) return;
        if (message.id().equals(playingMessageId)) { stopAudioPlayback(); render(); return; }
        stopAudioPlayback();
        playingMessageId = message.id();
        voicePlayback.play(message.voiceAttachment().localPath(), () -> {
            if (destroyed || !message.id().equals(playingMessageId)) return;
            playingMessageId = null; render();
        });
        render();
    }
    private void stopAudioPlayback() {
        if (voicePlayback != null) voicePlayback.stop();
        playingMessageId = null;
    }

    private void showMessageCopyDialog(RemoteIMMessage message) {
        Dialog dialog = new Dialog(this);
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(12), dp(12), dp(12), dp(12));
        card.setBackground(MaiChatTheme.rounded(Color.WHITE, 14, this));
        TextView copy = popupTextButton("复制消息内容");
        TextView copyFull = popupTextButton("复制完整信息");
        TextView reply = popupTextButton("引用回复");
        TextView forward = popupTextButton("转发");
        reply.setOnClickListener(view -> {
            pendingQuote = MessageQuote.from(message);
            dialog.dismiss();
            render();
            if (messageInput != null) {
                messageInput.requestFocus();
                messageInput.post(() -> ((InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
                    .showSoftInput(messageInput, InputMethodManager.SHOW_IMPLICIT));
            }
        });
        copy.setOnClickListener(view -> {
            copyToClipboard(message.text());
            dialog.dismiss();
        });
        copyFull.setOnClickListener(view -> {
            copyToClipboard(fullMessageText(message));
            dialog.dismiss();
        });
        card.addView(reply, match(dp(46)));
        forward.setOnClickListener(view -> {
            dialog.dismiss();
            showForwardMessageDialog(message);
        });
        card.addView(forward, match(dp(46)));
        card.addView(copy, match(dp(46)));
        card.addView(copyFull, match(dp(46)));
        dialog.setContentView(card);
        allowKeyboardLocation = false;
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setLayout(dp(260), ViewGroup.LayoutParams.WRAP_CONTENT);
        }
    }

    // 作用：展示消息转发目标并提交选中的转发操作。
    private void showForwardMessageDialog(RemoteIMMessage message) {
        List<RemoteIMContact> contacts = session.acceptedFriends();
        if (contacts.isEmpty()) {
            toast("还没有联系人，暂时无法转发");
            return;
        }

        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(18), dp(18), dp(16));
        card.setBackground(MaiChatTheme.rounded(Color.WHITE, 16, this));
        card.addView(MaiChatTheme.label(this, "转发给", 20, MaiChatTheme.TEXT), match(dp(32)));

        EditText filter = new EditText(this);
        filter.setSingleLine(true);
        filter.setHint("搜索联系人");
        filter.setPadding(dp(12), 0, dp(12), 0);
        filter.setBackground(MaiChatTheme.bordered(
            MaiChatTheme.PAGE, MaiChatTheme.BORDER, 9, this
        ));
        card.addView(filter, match(dp(42)));

        ScrollView scroll = new ScrollView(this);
        LinearLayout rows = new LinearLayout(this);
        rows.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(rows, matchWrap());
        LinearLayout.LayoutParams scrollParams = match(dp(340));
        scrollParams.setMargins(0, dp(10), 0, dp(4));
        card.addView(scroll, scrollParams);

        Runnable refreshRows = () -> {
            String query = filter.getText().toString().trim().toLowerCase(java.util.Locale.ROOT);
            rows.removeAllViews();
            for (RemoteIMContact contact : contacts) {
                String displayName = contact.displayName().isEmpty()
                    ? contact.userId() : contact.displayName();
                if (!query.isEmpty()
                    && !displayName.toLowerCase(java.util.Locale.ROOT).contains(query)
                    && !contact.userId().toLowerCase(java.util.Locale.ROOT).contains(query)) {
                    continue;
                }
                LinearLayout row = new LinearLayout(this);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(dp(12), dp(7), dp(12), dp(7));
                row.setBackground(MaiChatTheme.rounded(MaiChatTheme.PAGE, 10, this));
                TextView name = MaiChatTheme.label(this, displayName, 15, MaiChatTheme.TEXT);
                row.addView(name, new LinearLayout.LayoutParams(0, dp(44), 1));
                TextView arrow = MaiChatTheme.text(this, "➜", 17, MaiChatTheme.BLUE_DARK);
                row.addView(arrow, new LinearLayout.LayoutParams(dp(30), dp(44)));
                row.setContentDescription("转发给 " + displayName);
                row.setOnClickListener(view -> {
                    row.setEnabled(false);
                    session.forwardMessageAsync(message, contact.userId(), forwarded -> {
                        if (destroyed) return;
                        dialog.dismiss(); toast("正在转发给 " + displayName);
                        if (contact.userId().equals(activeChatUserId) && currentMessageList != null) {
                            currentMessageList.requestMessageBottomAfterUpdate(forwarded.id()); refreshChatMessages();
                        }
                    }, error -> { row.setEnabled(true); if (!destroyed) toast(error == null ? "转发失败" : error); });
                });
                LinearLayout.LayoutParams rowParams = match(dp(58));
                rowParams.setMargins(0, 0, 0, dp(8));
                rows.addView(row, rowParams);
            }
            if (rows.getChildCount() == 0) {
                TextView empty = MaiChatTheme.text(
                    this, "没有匹配的联系人", 14, MaiChatTheme.SECONDARY
                );
                empty.setGravity(Gravity.CENTER);
                rows.addView(empty, match(dp(72)));
            }
        };
        filter.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable editable) { refreshRows.run(); }
        });
        refreshRows.run();

        dialog.setContentView(card);
        allowKeyboardLocation = false;
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setDimAmount(0.28f);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setLayout(
                (int) (getResources().getDisplayMetrics().widthPixels * 0.88f),
                ViewGroup.LayoutParams.WRAP_CONTENT
            );
        }
    }

    private void confirm(String title, String message, String confirmTitle, Runnable action) {
        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(20), dp(20), dp(20), dp(18));
        card.setBackground(MaiChatTheme.rounded(Color.WHITE, 16, this));
        card.addView(MaiChatTheme.label(this, title, 19, MaiChatTheme.TEXT), match(dp(28)));
        TextView body = MaiChatTheme.text(this, message, 14, MaiChatTheme.SECONDARY);
        body.setPadding(0, dp(8), 0, dp(16));
        card.addView(body, matchWrap());
        LinearLayout buttons = new LinearLayout(this);
        Button cancel = secondaryButton("取消");
        Button confirm = primaryButton(confirmTitle);
        confirm.setBackground(MaiChatTheme.rounded(MaiChatTheme.RED, 9, this));
        cancel.setOnClickListener(view -> dialog.dismiss());
        confirm.setOnClickListener(view -> {
            dialog.dismiss();
            action.run();
        });
        buttons.addView(cancel, new LinearLayout.LayoutParams(0, dp(42), 1));
        LinearLayout.LayoutParams confirmParams = new LinearLayout.LayoutParams(0, dp(42), 1);
        confirmParams.setMargins(dp(10), 0, 0, 0);
        buttons.addView(confirm, confirmParams);
        card.addView(buttons, match(dp(42)));
        dialog.setContentView(card);
        allowKeyboardLocation = false;
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setDimAmount(0.28f);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * 0.86f),
                    ViewGroup.LayoutParams.WRAP_CONTENT);
        }
    }

    private View statusDot() {
        View dot = new View(this);
        int color;
        switch (session.connectionState()) {
            case CONNECTED: color = MaiChatTheme.GREEN; break;
            case CONNECTING: color = Color.rgb(245, 158, 11); break;
            case FAILED: color = MaiChatTheme.RED; break;
            case DISCONNECTED:
            default: color = MaiChatTheme.SECONDARY; break;
        }
        dot.setBackground(MaiChatTheme.rounded(color, 4, this));
        return dot;
    }

    private String connectionText() {
        switch (session.connectionState()) {
            case CONNECTED: return "已连接";
            case CONNECTING: return "连接中";
            case FAILED: return "连接失败";
            case DISCONNECTED:
            default: return "未连接";
        }
    }

    private View avatar(RemoteIMContact contact, boolean outgoing, int sizeDp) {
        FrameLayout frame = new FrameLayout(this);
        byte[] accountBytes = contact.userId().getBytes(StandardCharsets.UTF_8);
        int avatarHash = 0x811c9dc5;
        for (byte accountByte : accountBytes)
            avatarHash = (avatarHash ^ (accountByte & 0xff)) * 0x01000193;
        ImageView fallback = new ImageView(this);
        fallback.setImageBitmap(DefaultAvatarAtlas.portrait(
            this, Integer.remainderUnsigned(avatarHash, 26)));
        fallback.setScaleType(ImageView.ScaleType.CENTER_CROP);
        fallback.setBackground(MaiChatTheme.rounded(Color.TRANSPARENT, 10, this));
        fallback.setClipToOutline(true);
        frame.addView(fallback, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));
        if (!contact.avatarUrl().isEmpty()
                && session.connectionState() == TencentIMClient.ConnectionState.CONNECTED) {
            ImageView image = new ImageView(this);
            image.setScaleType(ImageView.ScaleType.CENTER_CROP);
            image.setBackground(MaiChatTheme.rounded(Color.TRANSPARENT, 10, this));
            image.setClipToOutline(true);
            frame.addView(image, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            ));
            AvatarImageLoader.load(contact.avatarUrl(), image);
        }
        if (session.presenceStatus(contact.userId()) == TencentIMClient.PresenceStatus.ONLINE) {
            View dot = new View(this);
            dot.setBackground(MaiChatTheme.bordered(MaiChatTheme.GREEN, Color.WHITE, 6, this));
            FrameLayout.LayoutParams dotParams =
                    new FrameLayout.LayoutParams(dp(11), dp(11), Gravity.BOTTOM | Gravity.END);
            dotParams.rightMargin = -dp(1);
            dotParams.bottomMargin = -dp(1);
            frame.addView(dot, dotParams);
        }
        return frame;
    }

    private TextView presenceBadge(String userId) {
        TencentIMClient.PresenceStatus status = session.presenceStatus(userId);
        String text = status == TencentIMClient.PresenceStatus.ONLINE
            ? "在线"
            : status == TencentIMClient.PresenceStatus.OFFLINE ? "离线" : "";
        TextView badge = MaiChatTheme.label(
            this,
            text,
            11,
            status == TencentIMClient.PresenceStatus.ONLINE ? Color.rgb(12, 132, 74) : MaiChatTheme.SECONDARY
        );
        badge.setGravity(Gravity.CENTER);
        badge.setPadding(dp(7), 0, dp(7), 0);
        badge.setBackground(MaiChatTheme.rounded(
            status == TencentIMClient.PresenceStatus.ONLINE
                ? MaiChatTheme.GREEN_SOFT
                : Color.rgb(241, 243, 246),
            10,
            this
        ));
        return badge;
    }

    private TextView unreadBadge(int count) {
        TextView badge = MaiChatTheme.label(this, count > 99 ? "99+" : String.valueOf(count), 11, Color.WHITE);
        badge.setGravity(Gravity.CENTER);
        badge.setMinWidth(dp(18));
        badge.setMinHeight(dp(18));
        badge.setPadding(dp(5), 0, dp(5), 0);
        badge.setBackground(MaiChatTheme.rounded(Color.rgb(245, 63, 63), 9, this));
        return badge;
    }

    private View emptyState(String symbol, String title, String detail) {
        LinearLayout empty = new LinearLayout(this);
        empty.setOrientation(LinearLayout.VERTICAL);
        empty.setGravity(Gravity.CENTER);
        TextView icon = MaiChatTheme.text(this, symbol, 30, Color.rgb(143, 151, 163));
        icon.setGravity(Gravity.CENTER);
        empty.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));
        TextView heading = MaiChatTheme.label(this, title, 16, MaiChatTheme.TEXT);
        heading.setGravity(Gravity.CENTER);
        empty.addView(heading, match(dp(30)));
        TextView body = MaiChatTheme.text(this, detail, 13, MaiChatTheme.SECONDARY);
        body.setGravity(Gravity.CENTER);
        empty.addView(body, match(dp(28)));
        return empty;
    }

    private Button primaryButton(String title) {
        Button button = new Button(this);
        button.setText(title);
        button.setTextSize(15);
        button.setTextColor(Color.WHITE);
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setPadding(dp(12), 0, dp(12), 0);
        button.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 10, this));
        return button;
    }

    private Button secondaryButton(String title) {
        Button button = new Button(this);
        button.setText(title);
        button.setTextSize(15);
        button.setTextColor(MaiChatTheme.TEXT);
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setPadding(dp(12), 0, dp(12), 0);
        button.setBackground(MaiChatTheme.bordered(Color.rgb(245, 247, 250), MaiChatTheme.BORDER, 9, this));
        return button;
    }

    private TextView iconButton(String title, float textSize, int color) {
        TextView view = MaiChatTheme.label(this, title, textSize, color);
        view.setGravity(Gravity.CENTER);
        view.setClickable(true);
        view.setFocusable(true);
        return view;
    }

    private View symbolButton(
        MaiChatSymbolView.Symbol symbol,
        int color,
        int iconSizeDp,
        int backgroundColor,
        int cornerRadiusDp
    ) {
        FrameLayout button = new FrameLayout(this);
        button.setClickable(true);
        button.setFocusable(true);
        button.setBackground(
            cornerRadiusDp > 0
                ? MaiChatTheme.rounded(backgroundColor, cornerRadiusDp, this)
                : new ColorDrawable(backgroundColor)
        );
        MaiChatSymbolView icon = new MaiChatSymbolView(this, symbol);
        icon.setSymbolColor(color);
        button.addView(
            icon,
            new FrameLayout.LayoutParams(dp(iconSizeDp), dp(iconSizeDp), Gravity.CENTER)
        );
        return button;
    }

    private TextView remoteControlButton(String title, boolean selected) {
        TextView button = iconButton(title, 19, Color.WHITE);
        button.setBackground(MaiChatTheme.bordered(
            selected ? Color.argb(220, 15, 141, 221) : Color.argb(130, 45, 52, 62),
            Color.argb(90, 255, 255, 255),
            10,
            this
        ));
        return button;
    }

    private TextView popupTextButton(String title) {
        TextView view = MaiChatTheme.text(this, title, 15, MaiChatTheme.TEXT);
        view.setGravity(Gravity.CENTER);
        view.setBackground(MaiChatTheme.rounded(MaiChatTheme.PAGE, 9, this));
        return view;
    }

    private LinearLayout.LayoutParams sectionParams() {
        LinearLayout.LayoutParams params = matchWrap();
        params.setMargins(0, dp(16), 0, 0);
        return params;
    }

    private RemoteIMContact contact(String userId) {
        for (RemoteIMContact contact : session.chatState().contacts()) {
            if (contact.userId().equals(userId)) return contact;
        }
        return null;
    }

    public RemoteIMMessage latestMessage(String userId) {
        List<RemoteIMMessage> messages = session.chatState().messagesWith(userId);
        return messages.isEmpty() ? null : messages.get(messages.size() - 1);
    }

    private String statusText(RemoteIMMessage.Status status) {
        switch (status) {
            case PENDING: return "发送中";
            case SENT: return "已发送";
            case RECEIVED: return "已接收";
            case FAILED:
            default: return "发送失败";
        }
    }

    private int statusColor(RemoteIMMessage.Status status) {
        if (status == RemoteIMMessage.Status.FAILED) return MaiChatTheme.RED;
        if (status == RemoteIMMessage.Status.SENT) return MaiChatTheme.GREEN;
        return MaiChatTheme.SECONDARY;
    }

    private String fileSubtitle(RemoteIMFileAttachment attachment) {
        if (RemoteIMGitDiffDisplayPolicy.isGitDiff(attachment)) return "代码 Diff · 点击查看";
        if (attachment.mimeType().contains("html")) return "HTML 文件，点击预览";
        if (attachment.mimeType().contains("markdown") || attachment.fileName().endsWith(".md")) {
            return "Markdown 文件，点击预览";
        }
        return "点击使用系统应用预览";
    }

    private String fullMessageText(RemoteIMMessage message) {
        String type = message.imageAttachment() != null
            ? "图片"
            : message.voiceAttachment() != null
                ? "语音"
                : message.videoAttachment() != null
                    ? "视频"
                    : message.fileAttachment() != null ? "文件" : "文本";
        return "发送人：" + message.fromUserId() + "\n"
            + "接收人：" + message.toUserId() + "\n"
            + "时间：" + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.CHINA)
                .format(new Date(message.createdAtMillis())) + "\n"
            + "方向：" + (message.direction() == RemoteIMMessage.Direction.OUTGOING ? "发出" : "收到") + "\n"
            + "状态：" + statusText(message.status()) + "\n"
            + "类型：" + type + "\n"
            + "内容：\n" + message.text();
    }

    private String timestamp(long millis) {
        Date date = new Date(millis);
        java.util.Calendar now = java.util.Calendar.getInstance();
        java.util.Calendar value = java.util.Calendar.getInstance();
        value.setTime(date);
        if (sameDay(now, value)) return new SimpleDateFormat("HH:mm", Locale.CHINA).format(date);
        now.add(java.util.Calendar.DAY_OF_YEAR, -1);
        if (sameDay(now, value)) return "昨天 " + new SimpleDateFormat("HH:mm", Locale.CHINA).format(date);
        return new SimpleDateFormat("M 月 d 日 HH:mm", Locale.CHINA).format(date);
    }

    private boolean sameDay(java.util.Calendar left, java.util.Calendar right) {
        return left.get(java.util.Calendar.ERA) == right.get(java.util.Calendar.ERA)
            && left.get(java.util.Calendar.YEAR) == right.get(java.util.Calendar.YEAR)
            && left.get(java.util.Calendar.DAY_OF_YEAR) == right.get(java.util.Calendar.DAY_OF_YEAR);
    }

    private void copyToClipboard(String value) {
        ClipboardManager manager = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        manager.setPrimaryClip(ClipData.newPlainText("MaiChat message", value));
        toast("已复制");
    }

    public JSONObject exportDiagnosticsForAgent() throws Exception {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper())
            throw new IllegalStateException("排障日志不能在 UI 线程导出");
        if (diagnostics == null || destroyed)
            throw new IllegalStateException("排障日志暂不可用");
        File report = AIAssistantController.shared(this).workspaceDiagnosticOutputForHost();
        String header = "MaiChat Android\nversion=" + BuildConfig.VERSION_NAME
            + "\naccount=" + maskedAccount(session.settings().loginUserId())
            + "\nconnection=" + connectionText()
            + "\ncontacts=" + session.chatState().contacts().size()
            + "\nmessages_in_memory=" + session.chatState().messages().size() + "\n";
        CountDownLatch finished = new CountDownLatch(1);
        java.util.concurrent.atomic.AtomicBoolean failed =
            new java.util.concurrent.atomic.AtomicBoolean();
        diagnostics.export(report, header, file -> finished.countDown(), () -> {
            failed.set(true);
            finished.countDown();
        });
        if (!finished.await(30, TimeUnit.SECONDS) || failed.get()
            || !report.isFile() || report.length() < header.getBytes(StandardCharsets.UTF_8).length
            || report.length() > 20L * 1024 * 1024) {
            report.delete();
            throw new IOException("排障日志导出失败");
        }
        // 工具结果不包含日志正文，只提供 AI 会话卡片所需的工作区文件名。
        return new JSONObject().put("path", report.getName())
            .put("mime_type", "text/plain").put("bytes", report.length());
    }

    private String maskedAccount(String value) {
        if (value == null || value.isEmpty()) return "none";
        if (value.length() <= 4) return "***";
        return value.substring(0, 2) + "***" + value.substring(value.length() - 2);
    }

    private String readTextFile(String path) {
        try (InputStream input = new FileInputStream(path)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            return output.toString(StandardCharsets.UTF_8.name());
        } catch (IOException error) {
            return "文件暂不可预览";
        }
    }

    private void copy(InputStream input, File target) throws IOException {
        if (input == null) throw new IOException("input unavailable");
        try (FileOutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        }
    }

    private void hideKeyboard() {
        View focus = getCurrentFocus();
        if (focus == null) return;
        ((InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
            .hideSoftInputFromWindow(focus.getWindowToken(), 0);
        focus.clearFocus();
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
    }

    private int dp(float value) {
        return MaiChatTheme.dp(this, value);
    }

    private LinearLayout.LayoutParams match(int height) {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, height);
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
    }

    private LinearLayout.LayoutParams matchMatch() {
        return new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        );
    }

    private LinearLayout.LayoutParams wrapWrap() {
        return new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
    }

    private LinearLayout.LayoutParams weightMatch() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1);
    }

    private abstract static class SimpleTextWatcher implements TextWatcher {
        @Override
        public void beforeTextChanged(CharSequence value, int start, int count, int after) {
        }

        @Override
        public void onTextChanged(CharSequence value, int start, int before, int count) {
        }
    }
}
