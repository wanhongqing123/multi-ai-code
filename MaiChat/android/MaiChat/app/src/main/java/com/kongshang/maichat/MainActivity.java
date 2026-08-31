package com.kongshang.maichat;

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
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.media.MediaPlayer;
import android.media.MediaRecorder;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
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
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.ScrollView;
import android.widget.Space;
import android.widget.MediaController;
import android.widget.TextView;
import android.widget.VideoView;
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
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity implements RemoteIMSessionController.Listener {
    private static final int REQUEST_PICK_IMAGE = 1001;
    private static final int REQUEST_RECORD_AUDIO = 1002;
    private static final int REQUEST_CAMERA_PERMISSION = 1003;
    private static final int REQUEST_TAKE_PHOTO = 1004;
    private static final int REQUEST_PICK_FILE = 1005;
    private static final int REQUEST_POST_NOTIFICATIONS = 1006;
    private static final String TAG = "MaiChat.notify";
    private static final String MESSAGE_CHANNEL_ID = "maichat-new-messages";
    private static final String MESSAGE_GROUP_KEY = "maichat-private-messages";
    private static final String EXTRA_NOTIFICATION_PEER = "notification-peer-user-id";

    private RemoteIMSessionController session;
    private RemoteIMMediaStore mediaStore;
    private RemoteIMTab activeTab = RemoteIMTab.MESSAGES;
    private LinearLayout root;
    private LinearLayout content;
    private GrowingMessageEditText messageInput;
    private String activeChatUserId;
    private String draftText = "";
    private RemoteIMQuote pendingQuote;
    private String historyAnchorMessageId;
    private String messageSearchTargetId;
    private boolean stickToLatestMessage = true;
    private ScrollView currentMessageScroll;
    private LinearLayout currentMessageContainer;
    private String preservedScrollAnchorId;
    private int preservedScrollAnchorOffset;
    private String lastRenderedLatestMessageId;
    private boolean hasUnseenLatestMessage;
    private boolean voiceMode;
    private MediaRecorder recorder;
    private SpeechRecognizer speechRecognizer;
    private File recordingFile;
    private long recordingStartedAtMillis;
    private boolean cancelRecording;
    private MediaPlayer mediaPlayer;
    private String playingMessageId;
    private File pendingCameraFile;
    private boolean destroyed;
    private boolean showInitialLogin;
    private boolean loginSubmitting;
    private String loginError = "";
    private String loginUserDraft = "";
    private String contactSearchQuery = "";
    private String conversationSearchQuery = "";
    private List<RemoteIMMessageSearchHit> messageSearchResults = Collections.emptyList();
    private boolean messageSearchLoading;
    private int messageSearchGeneration;
    private LinearLayout currentConversationRows;
    private final ExecutorService messageSearchExecutor = Executors.newSingleThreadExecutor();
    private String pendingNotificationPeerUserId = "";
    private boolean activityInForeground;
    private final Set<String> collapsedContactGroups = new HashSet<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(Color.WHITE);
        getWindow().setNavigationBarColor(Color.WHITE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        // 发出的媒体同样落持久目录，不放缓存：路径会进聊天记录并被长期引用。
        mediaStore = new RemoteIMMediaStore(RemoteIMMediaPaths.forApp(this));
        // 凭证由 Gradle 从 local.properties 注入；没配时 isAvailable() 为 false，
        // 录音会照旧当语音消息发出去，不会因此报错。
        speechRecognizer = new TencentSpeechRecognizer(
                BuildConfig.TENCENT_ASR_APP_ID,
                BuildConfig.TENCENT_ASR_SECRET_ID,
                BuildConfig.TENCENT_ASR_SECRET_KEY);
        session = new RemoteIMSessionController(this, this);
        createMessageNotificationChannel();
        showInitialLogin = session.requiresLogin();
        render();
        handleNotificationIntent(getIntent());
        if (!session.requiresLogin()) requestNotificationPermissionIfNeeded();
    }

    @Override
    protected void onResume() {
        super.onResume();
        activityInForeground = true;
    }

    @Override
    protected void onPause() {
        activityInForeground = false;
        super.onPause();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleNotificationIntent(intent);
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        stopAudioPlayback();
        cancelVoiceRecording();
        messageSearchExecutor.shutdownNow();
        if (session != null) session.destroy();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (activeChatUserId != null) {
            hideKeyboard();
            session.setConversationVisible(activeChatUserId, false);
            activeChatUserId = null;
            messageSearchTargetId = null;
            pendingQuote = null;
            render();
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

    @Override
    public void onStateChanged() {
        if (destroyed) return;
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
            renderPreservingInput();
        });
    }

    @Override
    public void onError(String message) {
        if (destroyed) return;
        runOnUiThread(() -> Toast.makeText(
            this,
            message == null || message.trim().isEmpty() ? "操作失败" : message,
            Toast.LENGTH_LONG
        ).show());
    }

    @Override
    public void onNewIncomingMessage(
        RemoteIMMessage message,
        boolean conversationVisible
    ) {
        if (destroyed || message == null) return;
        runOnUiThread(() -> showNewMessageNotification(message, conversationVisible));
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK) return;
        if (requestCode == REQUEST_PICK_IMAGE && data != null && data.getData() != null) {
            sendPickedImage(data.getData());
        } else if (requestCode == REQUEST_PICK_FILE && data != null && data.getData() != null) {
            sendPickedFile(data.getData());
        } else if (requestCode == REQUEST_TAKE_PHOTO && pendingCameraFile != null) {
            sendImageFile(pendingCameraFile);
            pendingCameraFile = null;
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
        if (requestCode == REQUEST_RECORD_AUDIO) {
            if (granted) startVoiceRecording();
            else toast("没有麦克风权限，无法发送语音");
        } else if (requestCode == REQUEST_CAMERA_PERMISSION) {
            if (granted) openCamera();
            else toast("没有相机权限，无法拍照");
        } else if (requestCode == REQUEST_POST_NOTIFICATIONS && !granted) {
            Log.i(TAG, "notification-suppressed reason=permission-denied");
        }
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

    private void showNewMessageNotification(
        RemoteIMMessage message,
        boolean conversationVisible
    ) {
        if (!RemoteIMNewMessageNotificationPolicy.shouldNotify(
            true,
            activityInForeground,
            conversationVisible
        )) {
            Log.d(TAG, "notification-suppressed reason=visible-foreground-conversation"
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
        boolean restoreFocus = messageInput != null && messageInput.hasFocus();
        if (messageInput != null) draftText = messageInput.getText().toString();
        render();
        if (restoreFocus && messageInput != null) {
            messageInput.requestFocus();
            messageInput.setSelection(messageInput.length());
            messageInput.post(() -> {
                InputMethodManager manager = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                manager.showSoftInput(messageInput, InputMethodManager.SHOW_IMPLICIT);
            });
        }
    }

    private void captureCurrentMessagePosition() {
        if (stickToLatestMessage
            || historyAnchorMessageId != null
            || currentMessageScroll == null
            || currentMessageContainer == null) {
            return;
        }
        int scrollY = currentMessageScroll.getScrollY();
        for (int index = 0; index < currentMessageContainer.getChildCount(); index += 1) {
            View child = currentMessageContainer.getChildAt(index);
            if (child.getBottom() < scrollY) continue;
            Object tag = child.getTag();
            if (tag instanceof String) {
                preservedScrollAnchorId = (String) tag;
                preservedScrollAnchorOffset = child.getTop() - scrollY;
            }
            return;
        }
    }

    private void render() {
        if (destroyed) return;
        captureCurrentMessagePosition();
        currentMessageScroll = null;
        currentMessageContainer = null;
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(MaiChatTheme.PAGE);
        setContentView(root);

        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(MaiChatTheme.PAGE);
        root.addView(content, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));

        if (showInitialLogin || session.requiresLogin()) {
            renderLogin();
            return;
        }

        if (activeChatUserId != null) {
            renderChatDetail(activeChatUserId);
        } else {
            switch (activeTab) {
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
        if (!hideTabs) root.addView(bottomTabBar(), match(dp(72)));
    }

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
            Gravity.CENTER
        );
        page.addView(form, formParams);

        ImageView icon = new ImageView(this);
        icon.setImageResource(getApplicationInfo().icon);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        form.addView(icon, new LinearLayout.LayoutParams(dp(64), dp(64)));

        TextView title = MaiChatTheme.label(this, "欢迎使用 MaiChat", 20, MaiChatTheme.TEXT);
        title.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams titleParams = wrapWrap();
        titleParams.setMargins(0, dp(12), 0, dp(28));
        form.addView(title, titleParams);

        EditText account = new EditText(this);
        account.setSingleLine(true);
        account.setTextSize(15);
        account.setHint("请输入登录账号");
        account.setTextColor(MaiChatTheme.TEXT);
        account.setHintTextColor(Color.rgb(100, 116, 139));
        account.setPadding(dp(14), 0, dp(14), 0);
        account.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 10, this));
        account.setImeOptions(EditorInfo.IME_ACTION_GO);
        account.setText(loginUserDraft);
        account.setSelection(account.length());
        form.addView(account, match(dp(46)));

        TextView errorText = MaiChatTheme.label(this, loginError, 13, MaiChatTheme.RED);
        errorText.setGravity(Gravity.CENTER);
        errorText.setVisibility(loginError.isEmpty() ? View.GONE : View.VISIBLE);
        LinearLayout.LayoutParams errorParams = matchWrap();
        errorParams.setMargins(0, dp(12), 0, 0);
        form.addView(errorText, errorParams);

        Button login = new Button(this);
        login.setText(loginSubmitting ? "登录中..." : "登录");
        login.setTextSize(15);
        login.setAllCaps(false);
        login.setGravity(Gravity.CENTER);
        login.setPadding(dp(12), 0, dp(12), 0);
        LinearLayout.LayoutParams loginParams = match(dp(46));
        loginParams.setMargins(0, dp(16), 0, 0);
        form.addView(login, loginParams);

        Runnable updateLoginAppearance = () -> {
            boolean enabled = !loginSubmitting && !loginUserDraft.trim().isEmpty();
            login.setEnabled(enabled);
            login.setTextColor(enabled ? Color.WHITE : Color.rgb(170, 180, 195));
            login.setBackground(MaiChatTheme.rounded(
                enabled ? Color.rgb(47, 129, 247) : Color.rgb(238, 241, 245),
                10,
                this
            ));
        };
        updateLoginAppearance.run();
        account.setOnFocusChangeListener((view, focused) -> account.setBackground(
            MaiChatTheme.bordered(
                Color.WHITE,
                focused ? Color.rgb(47, 129, 247) : MaiChatTheme.BORDER,
                10,
                this
            )
        ));
        account.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable editable) {
                loginUserDraft = editable.toString();
                loginError = "";
                updateLoginAppearance.run();
            }
        });
        View.OnClickListener submit = view -> {
            String userId = account.getText().toString().trim();
            if (userId.isEmpty() || loginSubmitting) return;
            loginSubmitting = true;
            loginError = "";
            login.setText("登录中...");
            updateLoginAppearance.run();
            try {
                session.login(userId);
                activeTab = RemoteIMTab.MESSAGES;
            } catch (IOException failure) {
                loginSubmitting = false;
                loginError = "登录设置保存失败";
                login.setText("登录");
                updateLoginAppearance.run();
            }
        };
        login.setOnClickListener(submit);
        account.setOnEditorActionListener((view, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_GO) {
                submit.onClick(view);
                return true;
            }
            return false;
        });
        page.requestFocus();
        content.addView(page, matchMatch());
    }

    private View bottomTabBar() {
        LinearLayout outside = new LinearLayout(this);
        outside.setPadding(dp(16), dp(9), dp(16), dp(10));
        outside.setBackgroundColor(Color.WHITE);

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER);
        bar.setPadding(dp(6), dp(6), dp(6), dp(6));
        bar.setBackground(MaiChatTheme.bordered(
            Color.rgb(245, 247, 250),
            MaiChatTheme.BORDER,
            14,
            this
        ));
        outside.addView(bar, matchMatch());
        bar.addView(tabButton(RemoteIMTab.MESSAGES, MaiChatSymbolView.Symbol.MESSAGE), weightMatch());
        bar.addView(tabButton(RemoteIMTab.CONTACTS, MaiChatSymbolView.Symbol.CONTACTS), weightMatch());
        bar.addView(tabButton(RemoteIMTab.REMOTE, MaiChatSymbolView.Symbol.REMOTE), weightMatch());
        bar.addView(tabButton(RemoteIMTab.ME, MaiChatSymbolView.Symbol.USER), weightMatch());
        return outside;
    }

    private View tabButton(RemoteIMTab tab, MaiChatSymbolView.Symbol symbol) {
        FrameLayout button = new FrameLayout(this);
        boolean selected = activeTab == tab;
        button.setBackground(selected
            ? MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 10, this)
            : new ColorDrawable(Color.TRANSPARENT));
        button.setContentDescription(tab.title());
        button.setClickable(true);
        button.setFocusable(true);
        MaiChatSymbolView icon = new MaiChatSymbolView(this, symbol);
        icon.setSymbolColor(selected ? MaiChatTheme.BLUE_DARK : MaiChatTheme.SECONDARY);
        FrameLayout.LayoutParams iconParams = new FrameLayout.LayoutParams(dp(25), dp(25), Gravity.CENTER);
        button.addView(icon, iconParams);

        if (tab == RemoteIMTab.MESSAGES && session.totalUnreadCount() > 0) {
            View badge = new View(this);
            badge.setBackground(MaiChatTheme.rounded(Color.rgb(245, 60, 48), 4, this));
            FrameLayout.LayoutParams badgeParams = new FrameLayout.LayoutParams(dp(8), dp(8), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
            badgeParams.leftMargin = dp(13);
            badgeParams.topMargin = dp(5);
            button.addView(badge, badgeParams);
        }
        button.setOnClickListener(view -> {
            activeTab = tab;
            activeChatUserId = null;
            render();
        });
        return button;
    }

    private void renderConversationList() {
        content.addView(connectionHeader(), match(dp(42)));
        EditText search = new EditText(this);
        search.setSingleLine(true);
        search.setHint("搜索联系人或全部消息");
        search.setTextSize(14);
        search.setPadding(dp(14), 0, dp(14), 0);
        search.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 10, this));
        search.setText(conversationSearchQuery);
        search.setSelection(search.length());
        LinearLayout.LayoutParams searchParams = match(dp(42));
        searchParams.setMargins(dp(16), dp(10), dp(16), dp(8));
        content.addView(search, searchParams);

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
        renderConversationRows(
            rows,
            conversationSearchQuery,
            messageSearchResults,
            messageSearchLoading
        );
        search.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable editable) {
                String query = editable == null ? "" : editable.toString();
                conversationSearchQuery = query;
                int generation = ++messageSearchGeneration;
                messageSearchResults = Collections.emptyList();
                messageSearchLoading = !query.trim().isEmpty();
                renderConversationRows(rows, query, messageSearchResults, messageSearchLoading);
                if (!messageSearchLoading) return;

                search.postDelayed(() -> messageSearchExecutor.execute(() -> {
                    List<RemoteIMMessageSearchHit> hits = session.searchMessages(query, 100);
                    runOnUiThread(() -> {
                        if (destroyed
                            || generation != messageSearchGeneration
                            || !query.equals(conversationSearchQuery)) return;
                        messageSearchResults = hits;
                        messageSearchLoading = false;
                        if (currentConversationRows != null
                            && activeTab == RemoteIMTab.MESSAGES
                            && activeChatUserId == null) {
                            renderConversationRows(
                                currentConversationRows,
                                conversationSearchQuery,
                                messageSearchResults,
                                false
                            );
                        }
                    });
                }), 250);
            }
        });
    }

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
        row.setBackgroundColor(Color.WHITE);
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
        session.selectContact(userId);
        session.setConversationVisible(userId, true);
        session.loadInitialMessages(userId);
        activeChatUserId = userId;
        activeTab = RemoteIMTab.MESSAGES;
        stickToLatestMessage = true;
        preservedScrollAnchorId = null;
        historyAnchorMessageId = null;
        messageSearchTargetId = null;
        pendingQuote = null;
        lastRenderedLatestMessageId = null;
        hasUnseenLatestMessage = false;
        render();
    }

    private void openMessageSearchHit(RemoteIMMessageSearchHit hit) {
        RemoteIMContact opened = session.openMessageSearchHit(hit);
        if (opened == null) return;
        session.setConversationVisible(opened.userId(), true);
        activeChatUserId = opened.userId();
        activeTab = RemoteIMTab.MESSAGES;
        stickToLatestMessage = false;
        preservedScrollAnchorId = null;
        historyAnchorMessageId = null;
        messageSearchTargetId = hit.message().id();
        lastRenderedLatestMessageId = null;
        hasUnseenLatestMessage = false;
        render();
    }

    private void renderChatDetail(String userId) {
        RemoteIMContact contact = contact(userId);
        if (contact == null) {
            activeChatUserId = null;
            renderConversationList();
            return;
        }
        content.addView(chatDetailHeader(contact), match(dp(52)));

        ScrollView messageScroll = new ScrollView(this);
        messageScroll.setFillViewport(true);
        LinearLayout messages = new LinearLayout(this);
        messages.setOrientation(LinearLayout.VERTICAL);
        messages.setPadding(dp(12), dp(10), dp(12), dp(10));
        messageScroll.addView(messages, matchWrap());
        currentMessageScroll = messageScroll;
        currentMessageContainer = messages;
        List<RemoteIMMessage> values = session.chatState().messagesWith(userId);
        Map<String, RemoteIMApprovalDisplayPolicy.State> approvalStates =
            RemoteIMApprovalDisplayPolicy.statesFor(values);
        RemoteIMMessage latestMessage = values.isEmpty() ? null : values.get(values.size() - 1);
        String nextLatestMessageId = latestMessage == null ? null : latestMessage.id();
        if (lastRenderedLatestMessageId != null
            && nextLatestMessageId != null
            && !lastRenderedLatestMessageId.equals(nextLatestMessageId)) {
            if (stickToLatestMessage
                || latestMessage.direction() == RemoteIMMessage.Direction.OUTGOING) {
                stickToLatestMessage = true;
                hasUnseenLatestMessage = false;
            } else {
                hasUnseenLatestMessage = true;
            }
        }
        lastRenderedLatestMessageId = nextLatestMessageId;
        if (values.isEmpty()) {
            messages.addView(emptyState("◇", "暂无消息", "发送一条消息开始对话。"), match(dp(260)));
        } else {
            for (RemoteIMMessage message : values) {
                View bubble = messageBubble(
                    message,
                    contact,
                    RemoteIMApprovalDisplayPolicy.stateFor(
                        message.approvalRequest(),
                        approvalStates
                    )
                );
                bubble.setTag(message.id());
                messages.addView(bubble, matchWrap());
            }
        }
        FrameLayout messageStage = new FrameLayout(this);
        messageStage.addView(messageScroll, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));
        TextView unseenButton = MaiChatTheme.label(this, "↓  新消息", 12, Color.WHITE);
        unseenButton.setGravity(Gravity.CENTER);
        unseenButton.setPadding(dp(12), 0, dp(12), 0);
        unseenButton.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 17, this));
        unseenButton.setVisibility(hasUnseenLatestMessage ? View.VISIBLE : View.GONE);
        unseenButton.setOnClickListener(view -> {
            stickToLatestMessage = true;
            hasUnseenLatestMessage = false;
            unseenButton.setVisibility(View.GONE);
            messageScroll.post(() -> messageScroll.fullScroll(View.FOCUS_DOWN));
        });
        FrameLayout.LayoutParams unseenParams = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            dp(34),
            Gravity.BOTTOM | Gravity.END
        );
        unseenParams.setMargins(0, 0, dp(12), dp(12));
        messageStage.addView(unseenButton, unseenParams);
        content.addView(messageStage, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));
        content.addView(composer(), matchWrap());

        messageScroll.setOnScrollChangeListener((view, scrollX, scrollY, oldX, oldY) -> {
            int maximumScroll = Math.max(0, messages.getHeight() - messageScroll.getHeight());
            stickToLatestMessage = maximumScroll - scrollY <= dp(48);
            if (stickToLatestMessage && hasUnseenLatestMessage) {
                hasUnseenLatestMessage = false;
                unseenButton.setVisibility(View.GONE);
            }
            if (scrollY == 0 && oldY > 0 && session.hasEarlierMessages(userId) && !values.isEmpty()) {
                historyAnchorMessageId = values.get(0).id();
                session.loadEarlierMessages(userId);
            }
        });
        messageScroll.post(() -> {
            if (messageSearchTargetId != null) {
                View target = messages.findViewWithTag(messageSearchTargetId);
                if (target != null) {
                    int offset = Math.max(0, (messageScroll.getHeight() - target.getHeight()) / 3);
                    messageScroll.scrollTo(0, Math.max(0, target.getTop() - offset));
                }
                messageSearchTargetId = null;
            } else if (historyAnchorMessageId != null) {
                View anchor = messages.findViewWithTag(historyAnchorMessageId);
                if (anchor != null) messageScroll.scrollTo(0, anchor.getTop());
                historyAnchorMessageId = null;
            } else if (preservedScrollAnchorId != null) {
                View anchor = messages.findViewWithTag(preservedScrollAnchorId);
                if (anchor != null) {
                    messageScroll.scrollTo(0, anchor.getTop() - preservedScrollAnchorOffset);
                }
                preservedScrollAnchorId = null;
                preservedScrollAnchorOffset = 0;
            } else if (stickToLatestMessage) {
                messageScroll.fullScroll(View.FOCUS_DOWN);
            }
        });
    }

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
            hideKeyboard();
            session.setConversationVisible(contact.userId(), false);
            activeChatUserId = null;
            pendingQuote = null;
            messageSearchTargetId = null;
            render();
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
                controller.requestView(contact.userId(), session.settings().loginUserId(), session.currentUserSig());
            }
            activeChatUserId = null;
            activeTab = RemoteIMTab.REMOTE;
            render();
        });
        header.addView(remote, new LinearLayout.LayoutParams(dp(42), dp(42)));
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(dp(8), dp(8));
        statusParams.setMargins(dp(5), 0, dp(4), 0);
        header.addView(statusDot(), statusParams);
        return header;
    }

    private View messageBubble(
        RemoteIMMessage message,
        RemoteIMContact peer,
        RemoteIMApprovalDisplayPolicy.State approvalState
    ) {
        boolean outgoing = message.direction() == RemoteIMMessage.Direction.OUTGOING;
        LinearLayout outer = new LinearLayout(this);
        outer.setOrientation(LinearLayout.HORIZONTAL);
        outer.setGravity((outgoing ? Gravity.END : Gravity.START) | Gravity.TOP);
        outer.setPadding(0, dp(5), 0, dp(5));

        View avatar = avatar(
            outgoing
                ? new RemoteIMContact(session.chatState().ownerUserId(), session.chatState().ownerUserId())
                : peer,
            outgoing,
            34
        );

        LinearLayout bubble = new LinearLayout(this);
        bubble.setOrientation(LinearLayout.VERTICAL);
        bubble.setPadding(dp(12), dp(9), dp(12), dp(9));
        bubble.setBackground(MaiChatTheme.bordered(
            outgoing ? Color.WHITE : MaiChatTheme.YELLOW_SOFT,
            outgoing ? MaiChatTheme.BORDER : MaiChatTheme.YELLOW_BORDER,
            12,
            this
        ));

        TextView meta = MaiChatTheme.text(
            this,
            (outgoing ? "我" : peer.displayName()) + "  " + timestamp(message.createdAtMillis()),
            11,
            MaiChatTheme.SECONDARY
        );
        bubble.addView(meta, match(dp(20)));

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
            TextView body = MaiChatTheme.text(this, "", 15, MaiChatTheme.TEXT);
            body.setText(MarkdownRenderer.render(message.text()));
            body.setTextIsSelectable(true);
            body.setLineSpacing(0, 1.15f);
            body.setPadding(0, dp(5), 0, dp(2));
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

        if (outgoing) {
            TextView status = MaiChatTheme.text(this, statusText(message.status()), 11, statusColor(message.status()));
            status.setGravity(Gravity.END);
            bubble.addView(status, matchWrap());
        }
        bubble.setOnLongClickListener(view -> {
            showMessageCopyDialog(message);
            return true;
        });

        LinearLayout.LayoutParams bubbleParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.82f);
        LinearLayout.LayoutParams avatarParams = new LinearLayout.LayoutParams(dp(34), dp(34));
        if (outgoing) {
            bubbleParams.setMargins(dp(42), 0, dp(8), 0);
            outer.addView(bubble, bubbleParams);
            outer.addView(avatar, avatarParams);
        } else {
            outer.addView(avatar, avatarParams);
            bubbleParams.setMargins(dp(8), 0, dp(42), 0);
            outer.addView(bubble, bubbleParams);
        }
        return outer;
    }

    private TextView attachmentCaptionView(String caption) {
        TextView body = MaiChatTheme.text(this, "", 15, MaiChatTheme.TEXT);
        body.setText(MarkdownRenderer.render(caption));
        body.setTextIsSelectable(true);
        body.setLineSpacing(0, 1.15f);
        body.setPadding(0, dp(5), 0, dp(5));
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
        RemoteIMMessage target = session.findQuotedMessage(activeChatUserId, quote.messageId());
        if (target == null) {
            toast("原消息不在本地记录中");
            return;
        }
        messageSearchTargetId = target.id();
        stickToLatestMessage = false;
        render();
    }

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
        // Give immediate feedback without calling render(): Android's current render() rebuilds
        // the whole activity tree. The controller's debounced state notification will perform the
        // one authoritative rebuild after PENDING/SENT/FAILED is persisted.
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
    private View composer() {
        LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.VERTICAL);
        wrapper.setPadding(dp(16), dp(9), dp(16), dp(10));
        wrapper.setBackgroundColor(Color.WHITE);

        if (pendingQuote != null) {
            wrapper.addView(pendingQuoteBar(pendingQuote), match(dp(42)));
        }

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
            render();
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
            messageInput.setOnEditorActionListener((view, actionId, event) -> {
                if (actionId == EditorInfo.IME_ACTION_SEND
                    || (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER && event.getAction() == KeyEvent.ACTION_DOWN)) {
                    sendText();
                    return true;
                }
                return false;
            });
            messageInput.addTextChangedListener(new SimpleTextWatcher() {
                @Override
                public void afterTextChanged(Editable editable) {
                    draftText = editable.toString();
                    renderCommandSuggestions(suggestions, draftText);
                }
            });
            LinearLayout.LayoutParams inputParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
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
        bar.addView(plus, new LinearLayout.LayoutParams(dp(44), dp(44)));
        wrapper.addView(bar, matchWrap());
        return wrapper;
    }

    private void renderCommandSuggestions(LinearLayout container, String value) {
        container.removeAllViews();
        List<RemoteIMSlashCommand> commands = RemoteIMSlashCommandCatalog.suggestions(value);
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
        content.addView(contactToolbar(rows), match(dp(96)));
        ScrollView scroll = new ScrollView(this);
        scroll.addView(rows, matchWrap());
        content.addView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));

        renderContactRows(rows, contactSearchQuery);
    }

    private void renderContactRows(LinearLayout rows, String queryValue) {
        rows.removeAllViews();
        List<RemoteIMContact> contacts = session.chatState().contacts();
        List<String> groups = session.chatState().contactGroups();
        if (contacts.isEmpty() && groups.isEmpty()) {
            rows.addView(emptyState("◎", "暂无联系人", "添加好友账号后即可开始聊天。"), match(dp(280)));
            return;
        }

        boolean searching = queryValue != null && !queryValue.trim().isEmpty();
        for (ContactGroupDisplayPolicy.Row row : ContactGroupDisplayPolicy.rows(
            groups, contacts, collapsedContactGroups, queryValue
        )) {
            if (row.kind() == ContactGroupDisplayPolicy.Kind.GROUP_HEADER) {
                rows.addView(contactGroupHeader(
                    rows, row.groupName(), row.memberCount(), searching
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
                    "将删除 " + contact.displayName() + " 及本地聊天记录。",
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
        toolbar.setOrientation(LinearLayout.VERTICAL);
        toolbar.setPadding(dp(12), dp(4), dp(12), dp(4));
        toolbar.setBackgroundColor(Color.WHITE);
        EditText search = new EditText(this);
        search.setSingleLine(true);
        search.setHint("搜索联系人");
        search.setText(contactSearchQuery);
        search.setSelection(search.length());
        search.setTextSize(14);
        search.setPadding(dp(12), 0, dp(12), 0);
        search.setBackground(MaiChatTheme.bordered(MaiChatTheme.PAGE, MaiChatTheme.BORDER, 9, this));
        toolbar.addView(search, match(dp(42)));
        search.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable editable) {
                contactSearchQuery = editable.toString();
                renderContactRows(rows, contactSearchQuery);
            }
        });
        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.CENTER_VERTICAL);
        TextView group = iconButton("新建分组", 14, MaiChatTheme.BLUE_DARK);
        group.setContentDescription("新建分组");
        group.setOnClickListener(view -> showContactGroupNameDialog("新建分组", "", null));
        actions.addView(group, new LinearLayout.LayoutParams(0, dp(42), 1));
        TextView broadcast = iconButton("群发消息", 14, MaiChatTheme.BLUE_DARK);
        broadcast.setContentDescription("群发消息");
        broadcast.setOnClickListener(view -> showBroadcastDialog());
        actions.addView(broadcast, new LinearLayout.LayoutParams(0, dp(42), 1));
        TextView plus = iconButton("＋", 24, MaiChatTheme.BLUE_DARK);
        plus.setContentDescription("添加好友");
        plus.setOnClickListener(view -> showAddContactDialog());
        actions.addView(plus, new LinearLayout.LayoutParams(dp(44), dp(42)));
        toolbar.addView(actions, match(dp(42)));
        return toolbar;
    }

    private void showBroadcastDialog() {
        List<RemoteIMContact> contacts = session.chatState().contacts();
        if (contacts.isEmpty()) {
            new AlertDialog.Builder(this)
                .setTitle("还没有联系人")
                .setMessage("通讯录是空的，先加几个好友再群发。")
                .setPositiveButton("知道了", null)
                .show();
            return;
        }

        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(18), dp(18), dp(16));
        card.setBackground(MaiChatTheme.rounded(Color.WHITE, 16, this));
        card.addView(MaiChatTheme.label(this, "群发消息", 20, MaiChatTheme.TEXT), match(dp(30)));
        TextView detail = MaiChatTheme.text(
            this,
            "勾选的每个人都会单独收到一条私聊消息。",
            13,
            MaiChatTheme.SECONDARY
        );
        card.addView(detail, match(dp(28)));

        EditText filter = new EditText(this);
        filter.setSingleLine(true);
        filter.setHint("筛选联系人");
        filter.setPadding(dp(12), 0, dp(12), 0);
        filter.setBackground(MaiChatTheme.bordered(MaiChatTheme.PAGE, MaiChatTheme.BORDER, 9, this));
        card.addView(filter, match(dp(42)));

        ScrollView recipientScroll = new ScrollView(this);
        LinearLayout recipientRows = new LinearLayout(this);
        recipientRows.setOrientation(LinearLayout.VERTICAL);
        recipientScroll.addView(recipientRows, matchWrap());
        LinearLayout.LayoutParams listParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        );
        listParams.setMargins(0, dp(8), 0, dp(8));
        card.addView(recipientScroll, listParams);

        EditText message = new EditText(this);
        message.setHint("要发送的文本内容");
        message.setGravity(Gravity.TOP | Gravity.START);
        message.setMinLines(3);
        message.setMaxLines(5);
        message.setPadding(dp(12), dp(10), dp(12), dp(10));
        message.setBackground(MaiChatTheme.bordered(MaiChatTheme.PAGE, MaiChatTheme.BORDER, 9, this));
        card.addView(message, match(dp(92)));

        TextView summary = MaiChatTheme.text(this, "还没有选人", 13, MaiChatTheme.SECONDARY);
        Button send = primaryButton("发送");
        send.setEnabled(false);
        LinearLayout footer = new LinearLayout(this);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        footer.addView(summary, new LinearLayout.LayoutParams(0, dp(44), 1));
        Button cancel = secondaryButton("取消");
        footer.addView(cancel, new LinearLayout.LayoutParams(dp(72), dp(42)));
        LinearLayout.LayoutParams sendParams = new LinearLayout.LayoutParams(dp(112), dp(42));
        sendParams.setMargins(dp(8), 0, 0, 0);
        footer.addView(send, sendParams);
        LinearLayout.LayoutParams footerParams = match(dp(48));
        footerParams.setMargins(0, dp(8), 0, 0);
        card.addView(footer, footerParams);

        BroadcastRecipientPickerState pickerState = new BroadcastRecipientPickerState();
        Runnable updateSendState = () -> {
            int count = pickerState.selectedUserIds().size();
            summary.setText(count == 0 ? "还没有选人" : "已选 " + count + " 人");
            send.setText(count == 0 ? "发送" : "发送给 " + count + " 人");
            send.setEnabled(count > 0 && !message.getText().toString().trim().isEmpty());
        };
        Runnable[] refresh = new Runnable[1];
        refresh[0] = () -> renderBroadcastRecipientRows(
            recipientRows,
            contacts,
            session.chatState().contactGroups(),
            pickerState,
            refresh[0],
            updateSendState
        );
        filter.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable editable) {
                pickerState.setFilterText(editable.toString());
                refresh[0].run();
            }
        });
        message.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable editable) { updateSendState.run(); }
        });
        refresh[0].run();

        cancel.setOnClickListener(view -> dialog.dismiss());
        send.setOnClickListener(view -> {
            List<String> recipients = new ArrayList<>();
            List<String> names = new ArrayList<>();
            for (RemoteIMContact contact : contacts) {
                if (!pickerState.isSelected(contact.userId())) continue;
                recipients.add(contact.userId());
                names.add(contact.displayName());
            }
            String cleanText = message.getText().toString().trim();
            if (recipients.isEmpty() || cleanText.isEmpty()) return;
            new AlertDialog.Builder(this)
                .setTitle("确认群发")
                .setMessage("以下每个人会各收到一条相同的私聊消息：\n\n"
                    + String.join("、", names))
                .setNegativeButton("取消", null)
                .setPositiveButton("发送给 " + recipients.size() + " 人", (confirmDialog, which) -> {
                    dialog.dismiss();
                    try {
                        session.broadcastText(recipients, cleanText, this::showBroadcastResult);
                    } catch (IOException error) {
                        new AlertDialog.Builder(this)
                            .setTitle("群发失败")
                            .setMessage(error.getMessage())
                            .setPositiveButton("知道了", null)
                            .show();
                    }
                })
                .show();
        });

        dialog.setContentView(card);
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setDimAmount(0.28f);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setLayout(
                (int) (getResources().getDisplayMetrics().widthPixels * 0.94f),
                (int) (getResources().getDisplayMetrics().heightPixels * 0.84f)
            );
        }
    }

    private void renderBroadcastRecipientRows(
        LinearLayout rows,
        List<RemoteIMContact> contacts,
        List<String> groups,
        BroadcastRecipientPickerState pickerState,
        Runnable refresh,
        Runnable updateSendState
    ) {
        rows.removeAllViews();
        for (BroadcastRecipientDisplayPolicy.Row row : pickerState.visibleRows(groups, contacts)) {
            if (row.kind() == BroadcastRecipientDisplayPolicy.Kind.CONTACT) {
                rows.addView(broadcastContactRow(
                    row.contact(), row.isIndented(), pickerState, refresh, updateSendState
                ), match(dp(38)));
                continue;
            }
            String group = row.groupName();
            BroadcastSelectionPolicy.GroupState state =
                pickerState.groupState(group, contacts);
            String marker = state == BroadcastSelectionPolicy.GroupState.ALL ? "☑ "
                : state == BroadcastSelectionPolicy.GroupState.PARTIAL ? "◩ " : "☐ ";
            TextView header = MaiChatTheme.label(
                this, marker + group + "（" + row.memberCount() + "）", 14, MaiChatTheme.TEXT
            );
            header.setGravity(Gravity.CENTER_VERTICAL);
            header.setPadding(dp(4), 0, dp(4), 0);
            header.setOnClickListener(view -> {
                pickerState.toggleGroup(group, contacts);
                refresh.run();
                updateSendState.run();
            });
            rows.addView(header, match(dp(38)));
        }
    }

    private View broadcastContactRow(
        RemoteIMContact contact,
        boolean indented,
        BroadcastRecipientPickerState pickerState,
        Runnable refresh,
        Runnable updateSendState
    ) {
        TextView row = MaiChatTheme.label(
            this,
            (pickerState.isSelected(contact.userId()) ? "☑ " : "☐ ") + contact.displayName(),
            14,
            MaiChatTheme.TEXT
        );
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(indented ? dp(18) : dp(4), 0, dp(4), 0);
        row.setOnClickListener(view -> {
            pickerState.toggleContact(contact.userId());
            refresh.run();
            updateSendState.run();
        });
        return row;
    }

    private void showBroadcastResult(int total, List<String> failedUserIds) {
        List<String> failedNames = new ArrayList<>();
        for (String userId : failedUserIds) {
            RemoteIMContact contact = contact(userId);
            failedNames.add(contact == null ? userId : contact.displayName());
        }
        new AlertDialog.Builder(this)
            .setTitle(failedNames.isEmpty() ? "群发完成" : "部分没有发出去")
            .setMessage(failedNames.isEmpty()
                ? total + " 个人都收到了。"
                : total + " 个人里有 " + failedNames.size() + " 个没发出去：\n\n"
                    + String.join("、", failedNames)
                    + "\n\n失败消息保留在各自会话里，可以单独重发。")
            .setPositiveButton("知道了", null)
            .show();
    }

    private View contactGroupHeader(LinearLayout rows, String group, int count, boolean searching) {
        TextView header = MaiChatTheme.label(
            this,
            ((searching || !collapsedContactGroups.contains(group)) ? "⌄ " : "› ")
                + group + "  " + count,
            14,
            MaiChatTheme.TEXT
        );
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(4), 0, dp(8), 0);
        header.setOnClickListener(view -> {
            if (collapsedContactGroups.contains(group)) collapsedContactGroups.remove(group);
            else collapsedContactGroups.add(group);
            renderContactRows(rows, contactSearchQuery);
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
        labels.add("新建分组并移入…");
        new AlertDialog.Builder(this)
            .setTitle("移动到分组")
            .setItems(labels.toArray(new String[0]), (dialog, index) -> {
                if (index < groups.size()) {
                    session.setContactGroup(contact.userId(), groups.get(index));
                } else if (!contact.groupName().isEmpty() && index == groups.size()) {
                    session.setContactGroup(contact.userId(), "");
                } else {
                    showContactGroupNameDialog("新建分组", "", contact.userId());
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
                for (RemoteIMContact contact : session.chatState().contacts()) {
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
        LinearLayout.LayoutParams textParams = new LinearLayout.LayoutParams(0, dp(50), 1);
        textParams.setMargins(dp(12), 0, dp(8), 0);
        row.addView(text, textParams);
        TextView name = MaiChatTheme.label(this, contact.displayName(), 15, MaiChatTheme.TEXT);
        name.setSingleLine(true);
        name.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        text.addView(name, match(dp(25)));
        TextView userId = MaiChatTheme.text(this, contact.userId(), 12, MaiChatTheme.SECONDARY);
        userId.setTypeface(Typeface.MONOSPACE);
        userId.setSingleLine(true);
        userId.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        text.addView(userId, match(dp(23)));
        row.addView(presenceBadge(contact.userId()), wrapWrap());
        row.setOnClickListener(view -> openChat(contact.userId()));
        return row;
    }

    private void showAddContactDialog() {
        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(20), dp(20), dp(20), dp(20));
        card.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 16, this));
        card.addView(MaiChatTheme.label(this, "添加好友", 20, MaiChatTheme.TEXT), match(dp(28)));
        TextView description = MaiChatTheme.text(this, "请输入要添加的好友账号", 13, MaiChatTheme.SECONDARY);
        LinearLayout.LayoutParams descriptionParams = match(dp(24));
        descriptionParams.setMargins(0, dp(2), 0, dp(12));
        card.addView(description, descriptionParams);
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setTextSize(15);
        input.setHint("好友账号");
        input.setImeOptions(EditorInfo.IME_ACTION_DONE);
        input.setPadding(dp(14), 0, dp(14), 0);
        input.setBackground(MaiChatTheme.bordered(MaiChatTheme.PAGE, MaiChatTheme.BORDER, 9, this));
        card.addView(input, match(dp(44)));

        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams actionsParams = match(dp(46));
        actionsParams.setMargins(0, dp(16), 0, 0);
        card.addView(actions, actionsParams);
        Button cancel = secondaryButton("取消");
        Button add = primaryButton("添加");
        LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(0, dp(42), 1);
        actions.addView(cancel, buttonParams);
        LinearLayout.LayoutParams addParams = new LinearLayout.LayoutParams(0, dp(42), 1);
        addParams.setMargins(dp(10), 0, 0, 0);
        actions.addView(add, addParams);
        cancel.setOnClickListener(view -> dialog.dismiss());
        View.OnClickListener submit = view -> {
            String userId = input.getText().toString().trim();
            if (userId.isEmpty()) return;
            session.addContact(userId);
            dialog.dismiss();
            openChat(userId);
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
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setDimAmount(0.28f);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * 0.86f), ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        input.requestFocus();
        input.post(() -> ((InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
            .showSoftInput(input, InputMethodManager.SHOW_IMPLICIT));
    }

    private void renderSettings() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(16), dp(12), dp(16), dp(22));
        scroll.addView(page, matchWrap());
        content.addView(scroll, matchMatch());

        page.addView(settingsSection("账号", new String[][]{
            {"登录账号", session.settings().loginUserId()}
        }), matchWrap());
        page.addView(settingsSection("IM 配置", new String[][]{
            {"通信配置", "内置"},
            {"连接凭证", "使用内置凭证"}
        }), sectionParams());
        page.addView(settingsSection("连接", new String[][]{
            {"状态", connectionText()}
        }), sectionParams());

        LinearLayout diagnostics = settingsSection("排障", new String[][]{});
        TextView export = MaiChatTheme.label(this, "⇧  导出排障信息", 15, MaiChatTheme.BLUE_DARK);
        export.setPadding(dp(12), 0, dp(12), 0);
        export.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 9, this));
        export.setOnClickListener(view -> exportDiagnostics());
        diagnostics.addView(export, match(dp(46)));
        TextView note = MaiChatTheme.text(
            this,
            "导出内容只包含版本、连接状态与脱敏运行信息，不包含聊天正文、远程键盘内容或连接凭证。",
            12,
            MaiChatTheme.SECONDARY
        );
        note.setPadding(dp(2), dp(8), dp(2), 0);
        diagnostics.addView(note, matchWrap());
        page.addView(diagnostics, sectionParams());

        Button logout = secondaryButton("退出登录");
        logout.setTextColor(MaiChatTheme.RED);
        LinearLayout.LayoutParams logoutParams = match(dp(48));
        logoutParams.setMargins(0, dp(22), 0, 0);
        page.addView(logout, logoutParams);
        logout.setOnClickListener(view -> confirm(
            "退出登录？",
            "本地聊天记录会保留，下次使用同一账号登录可继续查看。",
            "退出",
            () -> {
                try {
                    session.logout();
                    activeTab = RemoteIMTab.MESSAGES;
                    activeChatUserId = null;
                    render();
                } catch (IOException error) {
                    toast("退出登录失败");
                }
            }
        ));
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
            line.addView(MaiChatTheme.text(this, row[0], 14, MaiChatTheme.TEXT), new LinearLayout.LayoutParams(0, dp(46), 1));
            TextView value = MaiChatTheme.text(this, row[1], 14, MaiChatTheme.SECONDARY);
            value.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
            line.addView(value, new LinearLayout.LayoutParams(0, dp(46), 1));
            card.addView(line, match(dp(46)));
        }
        if (rows.length > 0) section.addView(card, matchWrap());
        return section;
    }

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
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * 0.92f), ViewGroup.LayoutParams.WRAP_CONTENT);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE | WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
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
    private void installRemoteGestures(TXCloudVideoView view, RemoteDesktopController controller) {
        final float[] scale = {1f};
        final double[][] lastPoint = {new double[]{0.5, 0.5}};
        final float[][] translation = {new float[]{0f, 0f}};
        ScaleGestureDetector scaleDetector = new ScaleGestureDetector(this, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override
            public boolean onScale(ScaleGestureDetector detector) {
                if (controller.isControlEnabled()) return false;
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
            session.sendTextMessage(text, pendingQuote);
            draftText = "";
            pendingQuote = null;
            stickToLatestMessage = true;
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

    private void startVoiceRecording() {
        if (recorder != null || session.chatState().selectedPeerId() == null) return;
        try {
            recordingFile = mediaStore.createVoiceRecordingFile();
            recorder = new MediaRecorder();
            recorder.setAudioSource(MediaRecorder.AudioSource.MIC);
            recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
            recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
            recorder.setOutputFile(recordingFile.getAbsolutePath());
            recorder.prepare();
            recorder.start();
            recordingStartedAtMillis = System.currentTimeMillis();
        } catch (IOException | RuntimeException error) {
            cancelVoiceRecording();
            toast("录音启动失败");
        }
    }

    private void finishVoiceRecording() {
        if (recorder == null) return;
        java.io.File finishedFile = null;
        int duration = 1;
        try {
            recorder.stop();
            duration = Math.max(1, (int) ((System.currentTimeMillis() - recordingStartedAtMillis) / 1000));
            finishedFile = recordingFile;
        } catch (RuntimeException error) {
            if (recordingFile != null) recordingFile.delete();
            toast("录音结束失败");
        } finally {
            recorder.release();
            recorder = null;
            recordingFile = null;
        }
        if (finishedFile == null) {
            render();
            return;
        }
        transcribeThenSend(finishedFile, duration);
    }

    /**
     * 录完先转文字发文字；识别不可用或失败时回退成发语音消息——用户说过的话不能因为
     * 识别这一环出问题就凭空消失。
     */
    private void transcribeThenSend(java.io.File audioFile, int duration) {
        if (speechRecognizer == null || !speechRecognizer.isAvailable()) {
            sendVoiceFallback(audioFile, duration);
            return;
        }
        toast("正在识别…");
        speechRecognizer.transcribe(audioFile, "m4a", new SpeechRecognizer.Callback() {
            @Override
            public void onText(String text) {
                if (text == null || text.trim().isEmpty()) {
                    toast("没听清，已按语音发送");
                    sendVoiceFallback(audioFile, duration);
                    return;
                }
                try {
                    session.sendTextMessage(text.trim());
                } catch (RuntimeException | IOException error) {
                    toast("识别成功但发送失败，已按语音发送");
                    sendVoiceFallback(audioFile, duration);
                    return;
                }
                audioFile.delete();
                stickToLatestMessage = true;
                render();
            }

            @Override
            public void onError(String message) {
                toast(message == null || message.isEmpty() ? "语音识别失败，已按语音发送" : message);
                sendVoiceFallback(audioFile, duration);
            }
        });
    }

    private void sendVoiceFallback(java.io.File audioFile, int duration) {
        try {
            session.sendVoiceMessage(audioFile.getAbsolutePath(), duration);
            stickToLatestMessage = true;
        } catch (RuntimeException | IOException error) {
            audioFile.delete();
            toast("语音消息发送失败");
        }
        render();
    }

    private void cancelVoiceRecording() {
        if (recorder != null) {
            try {
                recorder.stop();
            } catch (RuntimeException ignored) {
            }
            recorder.release();
            recorder = null;
        }
        if (recordingFile != null) recordingFile.delete();
        recordingFile = null;
    }

    private void showAttachmentMenu(View anchor) {
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
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setElevation(dp(10));
        popup.setOutsideTouchable(true);
        popup.showAsDropDown(anchor, -dp(146), -dp(170));
    }

    private void addPopupAction(LinearLayout menu, String title, Runnable action) {
        TextView row = MaiChatTheme.text(this, title, 14, MaiChatTheme.TEXT);
        row.setPadding(dp(12), 0, dp(12), 0);
        row.setOnClickListener(view -> action.run());
        menu.addView(row, match(dp(46)));
    }

    private void openImagePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/*");
        startActivityForResult(intent, REQUEST_PICK_IMAGE);
    }

    private void openFilePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_PICK_FILE);
    }

    private void requestCamera() {
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            openCamera();
        } else {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQUEST_CAMERA_PERMISSION);
        }
    }

    private void openCamera() {
        try {
            pendingCameraFile = mediaStore.createCameraPhotoFile();
            Uri output = FileProvider.getUriForFile(this, getPackageName() + ".files", pendingCameraFile);
            Intent intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
            intent.putExtra(MediaStore.EXTRA_OUTPUT, output);
            intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(intent, REQUEST_TAKE_PHOTO);
        } catch (IOException error) {
            toast("无法创建拍照文件");
        }
    }

    private void sendPickedImage(Uri uri) {
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            File file = mediaStore.copyPickedImage(input, uri.getLastPathSegment());
            sendImageFile(file);
        } catch (IOException error) {
            toast("图片读取失败");
        }
    }

    private void sendImageFile(File file) {
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(file.getAbsolutePath(), options);
        try {
            session.sendImageMessage(
                file.getAbsolutePath(),
                Math.max(0, options.outWidth),
                Math.max(0, options.outHeight),
                file.length()
            );
            stickToLatestMessage = true;
            render();
        } catch (IOException | IllegalStateException error) {
            toast("图片发送失败");
        }
    }

    private void sendPickedFile(Uri uri) {
        String fileName = queryDisplayName(uri);
        String mimeType = getContentResolver().getType(uri);
        if (mimeType == null || mimeType.trim().isEmpty()) mimeType = "application/octet-stream";
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            File target = mediaStore.createOutgoingFile(fileName);
            copy(input, target);
            session.sendFileMessage(target.getAbsolutePath(), fileName, mimeType, target.length());
            stickToLatestMessage = true;
            render();
        } catch (IOException | IllegalStateException error) {
            toast("文件发送失败");
        }
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
        // 不在这里同步解原图：解码放后台、按气泡尺寸降采样、结果进缓存。
        // 原先直接 decodeFile 解全尺寸，一张手机照片就要几十毫秒，且每次界面重建都重解。
        ImageView image = new ImageView(this);
        image.setScaleType(ImageView.ScaleType.CENTER_CROP);
        image.setBackgroundColor(MaiChatTheme.PAGE);
        image.setOnClickListener(view -> showFullScreenImage(attachment.localPath()));
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
            // 封面同样后台降采样，不在主线程解原图。
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
        if (!new File(attachment.localPath()).exists()) {
            // 封面先到、视频后到是常态，这里必须说清楚，否则用户点了没反应会以为坏了。
            if (result.length() > 0) result.append("  ");
            result.append("下载中…");
        }
        return result.length() == 0 ? "视频" : result.toString();
    }

    private void showVideoPlayer(RemoteIMVideoAttachment attachment) {
        File source = new File(attachment.localPath());
        if (!source.exists()) {
            toast("视频还在下载中，稍后再试");
            return;
        }
        Dialog dialog = new Dialog(this, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(Color.BLACK);

        VideoView video = new VideoView(this);
        video.setVideoPath(source.getAbsolutePath());
        MediaController controller = new MediaController(this);
        controller.setAnchorView(video);
        video.setMediaController(controller);
        video.setOnPreparedListener(player -> video.start());
        video.setOnErrorListener((player, what, extra) -> {
            toast("无法播放该视频（" + what + "/" + extra + "）");
            dialog.dismiss();
            return true;
        });
        FrameLayout.LayoutParams videoParams = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT,
            Gravity.CENTER
        );
        frame.addView(video, videoParams);

        TextView close = iconButton("×", 26, Color.WHITE);
        close.setBackground(MaiChatTheme.rounded(Color.argb(160, 0, 0, 0), 20, this));
        close.setOnClickListener(view -> dialog.dismiss());
        FrameLayout.LayoutParams closeParams =
            new FrameLayout.LayoutParams(dp(44), dp(44), Gravity.TOP | Gravity.END);
        closeParams.setMargins(0, dp(14), dp(14), 0);
        frame.addView(close, closeParams);

        // 不停就关的话解码器可能还占着文件句柄。
        dialog.setOnDismissListener(d -> video.stopPlayback());
        dialog.setContentView(frame);
        dialog.show();
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

    private void showFullScreenImage(String path) {
        Dialog dialog = new Dialog(this, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(Color.BLACK);
        ImageView image = new ImageView(this);
        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
        // 预览按屏幕像素请求，气泡按气泡像素请求：同一文件的两个尺寸各存一份缓存，
        // 所以缓存键里必须带目标尺寸，否则先到的会把另一个顶掉。
        MessageImageLoader.load(
            path,
            getResources().getDisplayMetrics().widthPixels,
            getResources().getDisplayMetrics().heightPixels,
            image,
            () -> {
                toast("图片暂不可预览");
                dialog.dismiss();
            });
        frame.addView(image, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));
        TextView close = iconButton("×", 26, Color.WHITE);
        close.setBackground(MaiChatTheme.rounded(Color.argb(160, 0, 0, 0), 20, this));
        close.setOnClickListener(view -> dialog.dismiss());
        FrameLayout.LayoutParams closeParams = new FrameLayout.LayoutParams(dp(44), dp(44), Gravity.TOP | Gravity.END);
        closeParams.setMargins(0, dp(14), dp(14), 0);
        frame.addView(close, closeParams);
        dialog.setContentView(frame);
        dialog.show();
    }

    private void showFilePreview(RemoteIMFileAttachment attachment) {
        if (RemoteIMGitDiffDisplayPolicy.isGitDiff(attachment)
            && !RemoteIMGitDiffDisplayPolicy.hasValidIntegrity(attachment)) {
            Toast.makeText(this, "Diff 文件 SHA256 校验失败，已停止渲染", Toast.LENGTH_LONG).show();
            return;
        }
        String mime = attachment.mimeType().toLowerCase(Locale.ROOT);
        String name = attachment.fileName().toLowerCase(Locale.ROOT);
        if (mime.contains("html") || name.endsWith(".html") || name.endsWith(".htm")) {
            Dialog dialog = previewDialog(
                RemoteIMGitDiffDisplayPolicy.isGitDiff(attachment) ? "代码 Diff" : attachment.fileName()
            );
            WebView web = new WebView(this);
            web.getSettings().setJavaScriptEnabled(false);
            web.loadDataWithBaseURL(
                new File(attachment.localPath()).getParentFile().toURI().toString(),
                readTextFile(attachment.localPath()),
                "text/html",
                "utf-8",
                null
            );
            addPreviewContent(dialog, web);
            return;
        }
        if (mime.contains("markdown") || name.endsWith(".md") || name.endsWith(".markdown") || mime.startsWith("text/")) {
            ScrollView scroll = new ScrollView(this);
            TextView text = MaiChatTheme.text(this, "", 14, MaiChatTheme.TEXT);
            text.setText(MarkdownRenderer.render(readTextFile(attachment.localPath())));
            text.setTextIsSelectable(true);
            text.setPadding(dp(16), dp(14), dp(16), dp(18));
            scroll.addView(text, matchWrap());
            Dialog dialog = previewDialog(attachment.fileName());
            addPreviewContent(dialog, scroll);
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
        if (message.id().equals(playingMessageId)) {
            stopAudioPlayback();
            render();
            return;
        }
        stopAudioPlayback();
        try {
            mediaPlayer = new MediaPlayer();
            mediaPlayer.setDataSource(message.voiceAttachment().localPath());
            mediaPlayer.setOnCompletionListener(player -> {
                stopAudioPlayback();
                render();
            });
            mediaPlayer.prepare();
            mediaPlayer.start();
            playingMessageId = message.id();
            render();
        } catch (IOException error) {
            stopAudioPlayback();
            toast("语音暂时无法播放");
        }
    }

    private void stopAudioPlayback() {
        if (mediaPlayer != null) {
            mediaPlayer.stop();
            mediaPlayer.release();
            mediaPlayer = null;
        }
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
        card.addView(copy, match(dp(46)));
        card.addView(copyFull, match(dp(46)));
        dialog.setContentView(card);
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setLayout(dp(260), ViewGroup.LayoutParams.WRAP_CONTENT);
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
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setDimAmount(0.28f);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * 0.86f), ViewGroup.LayoutParams.WRAP_CONTENT);
        }
    }

    private View connectionHeader() {
        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL | Gravity.END);
        header.setPadding(dp(16), dp(7), dp(16), dp(7));
        header.setBackgroundColor(Color.WHITE);
        header.addView(statusDot(), new LinearLayout.LayoutParams(dp(8), dp(8)));
        if (session.connectionState() != TencentIMClient.ConnectionState.CONNECTED) {
            TextView value = MaiChatTheme.label(this, connectionText(), 12, MaiChatTheme.SECONDARY);
            LinearLayout.LayoutParams valueParams = wrapWrap();
            valueParams.setMargins(dp(7), 0, 0, 0);
            header.addView(value, valueParams);
        }
        return header;
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
        dot.setContentDescription("IM 连接状态：" + connectionText());
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
        TextView avatar = MaiChatTheme.label(
            this,
            avatarText(contact.displayName(), contact.userId()),
            Math.max(11, sizeDp * 0.3f),
            Color.WHITE
        );
        avatar.setGravity(Gravity.CENTER);
        avatar.setBackground(MaiChatTheme.gradientAvatar(outgoing, this));
        frame.addView(avatar, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));
        if (!contact.avatarUrl().isEmpty()) {
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
            FrameLayout.LayoutParams dotParams = new FrameLayout.LayoutParams(dp(11), dp(11), Gravity.BOTTOM | Gravity.END);
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

    private RemoteIMMessage latestMessage(String userId) {
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

    private String avatarText(String displayName, String userId) {
        String source = displayName == null || displayName.trim().isEmpty() || displayName.equals(userId)
            ? userId
            : displayName.trim();
        if (source == null || source.isEmpty()) return "M";
        String[] words = source.replace('-', ' ').replace('_', ' ').trim().split("\\s+");
        if (words.length >= 2) {
            return (words[0].substring(0, 1) + words[words.length - 1].substring(0, 1)).toUpperCase(Locale.ROOT);
        }
        int count = source.codePointCount(0, source.length());
        int end = source.offsetByCodePoints(0, Math.min(2, count));
        return source.substring(0, end).toUpperCase(Locale.ROOT);
    }

    private void copyToClipboard(String value) {
        ClipboardManager manager = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        manager.setPrimaryClip(ClipData.newPlainText("MaiChat message", value));
        toast("已复制");
    }

    private void exportDiagnostics() {
        try {
            File directory = new File(getCacheDir(), "diagnostics");
            if (!directory.exists() && !directory.mkdirs()) throw new IOException("create diagnostics failed");
            File report = new File(directory, "MaiChat-Android-diagnostics.txt");
            String text = "MaiChat Android\n"
                + "version=0.1.51\n"
                + "account=" + maskedAccount(session.settings().loginUserId()) + "\n"
                + "connection=" + connectionText() + "\n"
                + "contacts=" + session.chatState().contacts().size() + "\n"
                + "messages_in_memory=" + session.chatState().messages().size() + "\n"
                + "remote_state=" + session.remoteDesktop().state().name().toLowerCase(Locale.ROOT) + "\n";
            try (FileOutputStream output = new FileOutputStream(report)) {
                output.write(text.getBytes(StandardCharsets.UTF_8));
            }
            Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".files", report);
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("text/plain");
            share.putExtra(Intent.EXTRA_STREAM, uri);
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, "导出 MaiChat 排障信息"));
        } catch (IOException error) {
            toast("排障信息导出失败");
        }
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
