// 文件作用：Android 创作中心的会话、消息和 Agent 交互。
// AIAssistantPanel 负责面板呈现、输入处理和状态反馈。
// 类型关系：AIAssistantPanel extends LinearLayout implements AIAssistantController.Listener。
// 主要入口：onScrollStateChanged、onTextChanged、onAttachedToWindow、onDetachedFromWindow。
package com.kongshang.maichat;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.ContentValues;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.media.MediaMetadataRetriever;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.*;
import androidx.core.content.FileProvider;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URLConnection;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.*;
import org.json.JSONArray;
import org.json.JSONObject;

/** Stable composer + recycled message rows. No native calls, disk I/O or parsing on UI. */
final class AIAssistantPanel extends LinearLayout implements AIAssistantController.Listener {
    private static final int MAX_MEDIA_ATTACHMENTS = 10;
    static final int REQUEST_FILE = 7107;
    static final int REQUEST_IMAGE = 7108;
    static final int REQUEST_CAMERA = 7109;
    static final int REQUEST_CAMERA_PERMISSION = 7110;
    private final Activity activity;
    final AIAssistantController controller;
    final EditText composer;
    final ImageButton send;
    private final ImageButton voiceModeButton, attachmentOrStopButton;
    private final Button voiceHoldButton;
    private final TextView error;
    private final LatestMessageList list;
    private final LinearLayout pending;
    private final LinearLayout attachmentSection, attachmentTray, attachmentPanel;
    private final LinearLayout quoteSection;
    private final TextView quoteText;
    private final TextView attachmentCount;
    private final MessageAdapter adapter = new MessageAdapter();
    private AIAssistantController.State state;
    private String selected = "", pendingSignature = "";
    private String renderedMessagesSession = "", pendingLatestSession = "";
    private int renderedMessageHash, renderedMessageLength, renderedVisibleLimit = -1;
    private final Map<String, String> drafts = new HashMap<>();
    private final Map<String, List<AIAssistantController.ImportedFile>> attachments = new HashMap<>();
    private final Map<String, Integer> pendingMediaImports = new HashMap<>();
    private final Set<String> deletedSessions = new HashSet<>();
    private final Set<String> expanded = new HashSet<>();
    private List<JSONObject> messages = Collections.emptyList();
    private boolean following = true, submitting;
    private int visibleLimit = 30;
    private final Button older;
    private boolean visibleToUser = true;
    private File pendingCameraFile;
    private String pendingMediaSession, pendingFileSession, pendingCameraSession, submittingSession = "";
    private String firstSentSession = "";
    private JSONObject quotedMessage;
    private String quoteSession = "";
    private boolean navigationPending;
    private int navigationGeneration;
    private final VoiceRecordingController voiceRecorder;
    private final SpeechRecognizer speechRecognizer;
    private boolean voiceMode, voiceRecording, cancelVoice, attachmentPanelVisible;
    private String voicePartialText = "";
    private int voiceGeneration;
    private final Runnable elapsed = new Runnable() {
        @Override
        public void run() {
            if (!isAttachedToWindow() || !visibleToUser)
                return;
            for (int i = 0; i < list.getChildCount(); ++i) {
                View row = list.getChildAt(i);
                if (row instanceof MessageRow)
                    ((MessageRow) row).updateTime();
            }
            postDelayed(this, 1000);
        }
    };
    AIAssistantPanel(Activity activity) {
        this(activity, AIAssistantController.shared(activity),
            activity instanceof MainActivity ? ((MainActivity) activity).voiceRecorderForAssistant() : null,
            activity instanceof MainActivity ? ((MainActivity) activity).speechRecognizerForAssistant() : null);
    }
    AIAssistantPanel(Activity activity, AIAssistantController controller) {
        this(activity, controller, null, null);
    }
    AIAssistantPanel(Activity activity, AIAssistantController controller,
                     VoiceRecordingController voiceRecorder, SpeechRecognizer speechRecognizer) {
        super(activity);
        this.activity = activity;
        this.controller = controller;
        this.voiceRecorder = voiceRecorder;
        this.speechRecognizer = speechRecognizer;
        setOrientation(VERTICAL);
        setBackgroundColor(Color.WHITE);
        MarkdownRenderer.initialize(activity);
        FrameLayout header = new FrameLayout(activity);
        ImageButton sessionsButton = headerIcon(R.drawable.ic_ai_sidebar, "对话列表", v -> sessions());
        FrameLayout.LayoutParams sessionsParams = new FrameLayout.LayoutParams(dp(34), dp(34),
            Gravity.START | Gravity.CENTER_VERTICAL);
        sessionsParams.setMarginStart(dp(16));
        header.addView(sessionsButton, sessionsParams);
        TextView title = text("创作中心", 18, MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        title.setGravity(Gravity.CENTER);
        header.addView(title, new FrameLayout.LayoutParams(
            LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT, Gravity.CENTER));
        ImageButton moreButton = headerIcon(R.drawable.ic_ai_more, "更多", null);
        moreButton.setOnClickListener(v -> menu(v));
        FrameLayout.LayoutParams moreParams = new FrameLayout.LayoutParams(dp(34), dp(34),
            Gravity.END | Gravity.CENTER_VERTICAL);
        moreParams.setMarginEnd(dp(16));
        header.addView(moreButton, moreParams);
        addView(header, new LayoutParams(LayoutParams.MATCH_PARENT, dp(54)));
        View headerDivider = new View(activity);
        headerDivider.setBackgroundColor(Color.argb(18, 0, 0, 0));
        addView(headerDivider, new LayoutParams(LayoutParams.MATCH_PARENT, dp(1)));
        list = new LatestMessageList();
        list.setStackFromBottom(true);
        list.setTranscriptMode(ListView.TRANSCRIPT_MODE_NORMAL);
        list.setDivider(null);
        list.setDividerHeight(dp(24));
        list.setPadding(dp(24), dp(18), dp(24), dp(24));
        list.setClipToPadding(false);
        older = button("显示更早消息", "显示更早消息", v -> {
            visibleLimit += 30;
            updateMessages();
        });
        // 首次快照尚未返回时不能显示历史入口；只在确认有更早消息后再露出。
        older.setVisibility(GONE);
        list.addHeaderView(older, null, false);
        list.setAdapter(adapter);
        list.setOnScrollListener(new AbsListView.OnScrollListener() {
            @Override
            public void onScrollStateChanged(AbsListView view, int scrollState) {
                if (scrollState != SCROLL_STATE_IDLE) {
                    following = false;
                    pendingLatestSession = "";
                } else if (atBottom())
                    following = true;
            }
            @Override
            public void onScroll(AbsListView view, int first, int count, int total) {}
        });
        list.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (following && b - t != ob - ot)
                locateLatest();
        });
        addView(list, new LayoutParams(LayoutParams.MATCH_PARENT, 0, 1));
        pending = column();
        pending.setPadding(dp(12), 0, dp(12), 0);
        addView(pending, matchWrap());
        error = text("", 12, Color.rgb(183, 45, 45));
        error.setPadding(dp(14), dp(5), dp(14), dp(5));
        error.setTextIsSelectable(true);
        error.setVisibility(GONE);
        addView(error, matchWrap());
        View composerDivider = new View(activity);
        composerDivider.setBackgroundColor(MaiChatTheme.BORDER);
        addView(composerDivider, new LayoutParams(LayoutParams.MATCH_PARENT, dp(1)));
        LinearLayout composerArea = column();
        composerArea.setBackgroundColor(Color.WHITE);
        quoteSection = row();
        quoteSection.setGravity(Gravity.CENTER_VERTICAL);
        quoteSection.setPadding(dp(16), dp(8), dp(16), dp(8));
        quoteSection.setBackgroundColor(MaiChatTheme.BLUE_SOFT);
        TextView quoteIcon = text("↩", 17, MaiChatTheme.BLUE);
        quoteSection.addView(quoteIcon, new LayoutParams(dp(28), LayoutParams.WRAP_CONTENT));
        quoteText = text("", 12, MaiChatTheme.TEXT);
        quoteText.setMaxLines(2);
        quoteText.setEllipsize(android.text.TextUtils.TruncateAt.END);
        quoteSection.addView(quoteText, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
        TextView cancelQuote = text("×", 22, MaiChatTheme.SECONDARY);
        cancelQuote.setGravity(Gravity.CENTER);
        cancelQuote.setContentDescription("取消引用");
        cancelQuote.setOnClickListener(view -> {
            quotedMessage = null;
            quoteSession = "";
            updateQuote();
        });
        quoteSection.addView(cancelQuote, new LayoutParams(dp(32), dp(32)));
        quoteSection.setVisibility(GONE);
        composerArea.addView(quoteSection, matchWrap());
        attachmentSection = column();
        attachmentSection.setPadding(dp(16), dp(10), dp(16), 0);
        attachmentCount = text("待发送附件", 12, MaiChatTheme.SECONDARY);
        attachmentCount.setTypeface(MaiChatTypography.semibold());
        attachmentSection.addView(attachmentCount, matchWrap());
        HorizontalScrollView attachmentScroll = new HorizontalScrollView(activity);
        attachmentScroll.setHorizontalScrollBarEnabled(false);
        attachmentTray = row();
        attachmentScroll.addView(attachmentTray);
        attachmentSection.addView(attachmentScroll, matchWrap());
        attachmentSection.setVisibility(GONE);
        composerArea.addView(attachmentSection, matchWrap());

        LinearLayout inputRow = row();
        inputRow.setGravity(Gravity.BOTTOM);
        inputRow.setPadding(dp(16), dp(12), dp(16), dp(10));
        voiceModeButton = circularIcon(R.drawable.ic_ai_voice, "切换到按住转文字", true,
            v -> setVoiceMode(!voiceMode));
        inputRow.addView(voiceModeButton, new LayoutParams(dp(44), dp(44)));

        FrameLayout inputSurface = new FrameLayout(activity);
        composer = new EditText(activity);
        composer.setTag("ai-composer");
        composer.setContentDescription("创作中心消息输入框");
        composer.setHint("可按住转文字");
        composer.setTextSize(16);
        composer.setMinLines(1);
        composer.setMaxLines(6);
        composer.setMinHeight(dp(44));
        composer.setPadding(dp(13), dp(9), dp(13), dp(9));
        composer.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BLUE, 14, activity));
        composer.setInputType(android.text.InputType.TYPE_CLASS_TEXT
            | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | android.text.InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        composer.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int st, int c, int a) {}
            @Override
            public void onTextChanged(CharSequence s, int st, int before, int count) {
                updateSend();
            }
            @Override
            public void afterTextChanged(Editable e) {}
        });
        composer.setOnFocusChangeListener((v, focused) -> {
            if (focused) {
                hideAttachmentPanel();
                following = true;
                locateLatest();
            }
        });
        inputSurface.addView(composer, new FrameLayout.LayoutParams(
            LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        voiceHoldButton = button("按住转文字", "按住转文字", null);
        voiceHoldButton.setTextSize(16);
        voiceHoldButton.setTextColor(MaiChatTheme.TEXT);
        voiceHoldButton.setSingleLine(true);
        voiceHoldButton.setEllipsize(android.text.TextUtils.TruncateAt.END);
        voiceHoldButton.setBackground(MaiChatTheme.bordered(
            Color.WHITE, MaiChatTheme.BLUE, 14, activity));
        voiceHoldButton.setOnTouchListener((view, event) -> voiceTouch(event));
        voiceHoldButton.setVisibility(GONE);
        inputSurface.addView(voiceHoldButton, new FrameLayout.LayoutParams(
            LayoutParams.MATCH_PARENT, dp(44)));
        LayoutParams inputParams = new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1);
        inputParams.setMargins(dp(8), 0, dp(8), 0);
        inputRow.addView(inputSurface, inputParams);

        attachmentOrStopButton = circularIcon(R.drawable.ic_ai_add, "展开更多功能", false,
            v -> {
                if (state != null && state.busy())
                    controller.action("stop", value("session", selected), null);
                else
                    setAttachmentPanelVisible(!attachmentPanelVisible);
            });
        inputRow.addView(attachmentOrStopButton, new LayoutParams(dp(44), dp(44)));
        send = circularIcon(R.drawable.ic_ai_send, "发送给 AI", false, v -> send());
        send.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 22, activity));
        send.setVisibility(GONE);
        LayoutParams sendParams = new LayoutParams(dp(44), dp(44));
        sendParams.setMargins(dp(8), 0, 0, 0);
        inputRow.addView(send, sendParams);
        composerArea.addView(inputRow, matchWrap());

        attachmentPanel = row();
        attachmentPanel.setPadding(dp(16), dp(8), dp(16), dp(14));
        attachmentPanel.addView(attachmentAction(
            R.drawable.ic_ai_gallery, "相册", "选择图片或视频", v -> openMediaPicker()),
            new LayoutParams(0, dp(64), 1));
        attachmentPanel.addView(attachmentAction(
            R.drawable.ic_ai_camera, "拍照", "拍照", v -> {
                hideAttachmentPanel();
                requestCamera();
            }), new LayoutParams(0, dp(64), 1));
        attachmentPanel.addView(attachmentAction(
            R.drawable.ic_ai_file, "文件", "导入文本文件", v -> openTextFilePicker()),
            new LayoutParams(0, dp(64), 1));
        attachmentPanel.setVisibility(GONE);
        composerArea.addView(attachmentPanel, matchWrap());
        addView(composerArea, matchWrap());
    }
    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        setForeground(true);
    }
    @Override
    protected void onDetachedFromWindow() {
        setForeground(false);
        super.onDetachedFromWindow();
    }
    void setForeground(boolean value) {
        visibleToUser = value;
        if (!value && voiceRecording) finishVoice(true);
        if (!value) updateHostVoiceOverlay(false, "");
        removeCallbacks(elapsed);
        if (value)
            controller.setListener(this);
        else
            controller.clearListener(this);
        if (value)
            post(elapsed);
    }
    @Override
    public void onState(AIAssistantController.State next) {
        boolean changedSession = !selected.equals(next.selected);
        if (changedSession) {
            String previous = selected;
            drafts.put(previous, composer.getText().toString());
            boolean firstSend = submitting && previous.isEmpty()
                && previous.equals(submittingSession);
            selected = next.selected;
            if (firstSend) {
                firstSentSession = selected;
                if (attachments.containsKey(previous))
                    attachments.put(selected, attachments.remove(previous));
                drafts.put(selected, drafts.remove(previous));
            }
            if (!firstSend)
                composer.setText(drafts.getOrDefault(selected, ""));
            if (!firstSend) {
                quotedMessage = null;
                quoteSession = "";
            }
            visibleLimit = 30;
            expanded.clear();
            following = true;
            pendingLatestSession = selected;
        }
        state = next;
        error.setText(next.error);
        error.setVisibility(next.error.isEmpty() ? GONE : VISIBLE);
        updateSend();
        updateAttachments();
        updateQuote();
        updateMessages();
        updatePending();
    }
    private void updateMessages() {
        if (state == null)
            return;
        if (renderedMessagesSession.equals(selected) && renderedVisibleLimit == visibleLimit
            && renderedMessageHash == state.messageHash
            && renderedMessageLength == state.messageLength)
            return;
        JSONArray array = state.data.optJSONArray("messages");
        int count = array == null ? 0 : array.length();
        List<JSONObject> next = new ArrayList<>();
        for (int i = Math.max(0, count - visibleLimit); i < count; i++) next.add(array.optJSONObject(i));
        messages = next;
        renderedMessagesSession = selected;
        renderedVisibleLimit = visibleLimit;
        renderedMessageHash = state.messageHash;
        renderedMessageLength = state.messageLength;
        older.setVisibility(count > visibleLimit ? VISIBLE : GONE);
        adapter.notifyDataSetChanged();
        if (following)
            locateLatest();
        if (!next.isEmpty() && selected.equals(pendingLatestSession)) {
            String target = selected;
            pendingLatestSession = "";
            list.post(() -> {
                if (!target.equals(selected)) return;
                following = true;
                list.setSelection(list.getCount() - 1);
                locateLatest();
            });
        }
    }
    private boolean atBottom() {
        if (list.getCount() == 0)
            return true;
        View last = list.getChildAt(list.getChildCount() - 1);
        return list.getLastVisiblePosition() == list.getCount() - 1 && last != null
            && last.getBottom() <= list.getHeight() - list.getPaddingBottom() + dp(30);
    }
    private void locateLatest() {
        list.showLatestContent();
    }
    private final class LatestMessageList extends ListView {
        LatestMessageList() {
            super(activity);
        }
        // 短消息使用系统的底部堆叠；当最后一条消息本身高于视口时，必须等真实行高
        // 可用后设置负偏移，才能看到该消息末尾，不能按字符数估算高度。
        void showLatestContent() {
            post(() -> {
                if (!following || getCount() == 0) return;
                int target = getCount() - 1;
                setSelection(target);
                postOnAnimation(() -> {
                    if (!following || target != getCount() - 1) return;
                    View last = getChildAt(target - getFirstVisiblePosition());
                    if (last == null) return;
                    int viewportHeight = getHeight() - getPaddingTop() - getPaddingBottom();
                    if (last.getHeight() > viewportHeight) {
                        int top = getPaddingTop() + viewportHeight - last.getHeight();
                        setSelectionFromTop(target, top);
                    }
                });
            });
        }
    }
    private void updateSend() {
        if (send == null) return;
        boolean busy = state != null && state.busy();
        boolean hasDraft = !composer.getText().toString().trim().isEmpty()
            || !currentAttachments().isEmpty();
        boolean canSubmit = state != null && state.ready && !busy && !submitting
            && !navigationPending && pendingMediaImports.getOrDefault(selected, 0) == 0 && hasDraft;
        send.setVisibility(!voiceMode && hasDraft && !busy ? VISIBLE : GONE);
        send.setEnabled(canSubmit);
        attachmentOrStopButton.setImageResource(busy ? R.drawable.ic_ai_stop : R.drawable.ic_ai_add);
        attachmentOrStopButton.setContentDescription(busy ? "停止" :
            (attachmentPanelVisible ? "收起更多功能" : "展开更多功能"));
        attachmentOrStopButton.setColorFilter(busy ? Color.WHITE : MaiChatTheme.TEXT);
        attachmentOrStopButton.setBackground(busy
            ? MaiChatTheme.rounded(Color.rgb(29, 32, 36), 22, activity)
            : MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 22, activity));
        attachmentOrStopButton.setEnabled(state != null && state.ready && !submitting);
        voiceModeButton.setEnabled(state != null && state.ready && !busy && !submitting);
        voiceHoldButton.setEnabled(state != null && state.ready && !busy && !submitting);
        // 提交期间保持原生编辑器焦点；禁用控件会让输入法意外收起。
    }
    private void setVoiceMode(boolean enabled) {
        if (voiceRecording || voiceMode == enabled) return;
        voiceMode = enabled;
        hideAttachmentPanel();
        voiceModeButton.setImageResource(enabled ? R.drawable.ic_ai_keyboard : R.drawable.ic_ai_voice);
        voiceModeButton.setContentDescription(enabled ? "切换到键盘输入" : "切换到按住转文字");
        composer.setVisibility(enabled ? GONE : VISIBLE);
        voiceHoldButton.setVisibility(enabled ? VISIBLE : GONE);
        if (enabled) hideKeyboard();
        updateSend();
    }
    private void setAttachmentPanelVisible(boolean visible) {
        if (attachmentPanelVisible == visible) return;
        if (visible && voiceMode) setVoiceMode(false);
        attachmentPanelVisible = visible;
        attachmentPanel.animate().cancel();
        if (visible) {
            hideKeyboard();
            attachmentPanel.setVisibility(VISIBLE);
            attachmentPanel.setAlpha(0);
            attachmentPanel.setTranslationY(dp(8));
            attachmentPanel.animate().alpha(1).translationY(0).setDuration(160).start();
        } else {
            attachmentPanel.animate().alpha(0).translationY(dp(8)).setDuration(120)
                .withEndAction(() -> {
                    if (!attachmentPanelVisible) attachmentPanel.setVisibility(GONE);
                }).start();
        }
        updateSend();
    }
    private void hideAttachmentPanel() {
        if (attachmentPanelVisible) setAttachmentPanelVisible(false);
    }
    private void hideKeyboard() {
        ((android.view.inputmethod.InputMethodManager) activity.getSystemService(
             Context.INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(getWindowToken(), 0);
        composer.clearFocus();
    }
    private void send() {
        if (state == null || submitting || navigationPending)
            return;
        if (state.busy()) {
            controller.action("stop", value("session", selected), null);
            return;
        }
        if (pendingMediaImports.getOrDefault(selected, 0) > 0) return;
        if (!state.data.optBoolean("configured")) {
            retryMainModelConnection();
            return;
        }
        String source = composer.getText().toString(), origin = selected;
        List<AIAssistantController.ImportedFile> sending = new ArrayList<>(currentAttachments());
        if (source.trim().isEmpty() && sending.isEmpty()) return;
        if (sending.stream().anyMatch(file -> file.image) &&
            !AIAssistantMediaPolicy.supportsImages(state.model)) {
            Toast.makeText(activity, "glm-5.3 仅支持文本，请先切换到 glm-5.3-flash。",
                Toast.LENGTH_LONG).show();
            return;
        }
        String submittedText = source;
        String quotedMessageId = quotedMessage != null && quoteSession.equals(origin)
            ? quotedMessage.optString("id") : "";
        submitting = true;
        submittingSession = origin;
        firstSentSession = "";
        updateAttachments();
        updateSend();
        following = true;
        controller.send(origin, source, sending, quotedMessageId, success -> {
            submitting = false;
            String target = origin.isEmpty() && !firstSentSession.isEmpty()
                ? firstSentSession : origin;
            if (success) {
                if (selected.equals(target) && composer.getText().toString().equals(submittedText))
                    composer.setText("");
                if (submittedText.equals(drafts.get(target))) drafts.put(target, "");
                if (!quotedMessageId.isEmpty() && selected.equals(target) &&
                    quotedMessage != null && quotedMessageId.equals(quotedMessage.optString("id"))) {
                    quotedMessage = null;
                    quoteSession = "";
                    updateQuote();
                }
                List<AIAssistantController.ImportedFile> values = attachments.get(target);
                if (values != null) {
                    values.removeAll(sending);
                    if (values.isEmpty()) attachments.remove(target);
                }
            }
            submittingSession = "";
            firstSentSession = "";
            updateAttachments();
            updateSend();
            if (success)
                locateLatest();
        });
    }
    void importFile(Uri uri) {
        importFile(uri, selected);
    }
    private void importFile(Uri uri, String origin) {
        controller.importFile(uri, file -> {
            if (file == null)
                return;
            acceptImportedFile(file, origin);
        });
    }
    private List<AIAssistantController.ImportedFile> currentAttachments() {
        return attachments.getOrDefault(selected, Collections.emptyList());
    }
    private void beginQuote(JSONObject message) {
        if (message == null || message.optString("id").isEmpty()) return;
        quotedMessage = message;
        quoteSession = selected;
        updateQuote();
        composer.requestFocus();
        android.view.inputmethod.InputMethodManager keyboard =
            (android.view.inputmethod.InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        if (keyboard != null) keyboard.showSoftInput(composer,
            android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
    }
    static String quotePreview(JSONObject message) {
        if (message == null) return "消息";
        JSONArray parts = message.optJSONArray("parts");
        StringBuilder content = new StringBuilder();
        List<String> media = new ArrayList<>();
        if (parts != null)
            for (int i = 0; i < parts.length(); i++) {
                JSONObject part = parts.optJSONObject(i);
                if (part == null) continue;
                String kind = part.optString("kind");
                if (kind.equals("text")) content.append(part.optString("text"));
                else if (kind.equals("image")) media.add("图片");
                else if (kind.equals("video")) media.add("视频");
                else if (kind.equals("tool")) {
                    JSONObject artifact = mediaFromToolOutput(part);
                    if (artifact != null) {
                        String type = artifact.optString("type");
                        media.add(type.equals("video") ? "视频" : type.equals("image") ? "图片" : "音频");
                    }
                }
            }
        String excerpt = content.toString().trim();
        if (excerpt.isEmpty()) excerpt = "消息";
        if (excerpt.codePointCount(0, excerpt.length()) > 80)
            excerpt = excerpt.substring(0, excerpt.offsetByCodePoints(0, 80));
        return (media.isEmpty() ? "" : "[" + String.join("、", media) + "] ") + excerpt;
    }
    private void updateQuote() {
        boolean visible = quotedMessage != null && quoteSession.equals(selected);
        quoteSection.setVisibility(visible ? VISIBLE : GONE);
        if (visible)
            quoteText.setText((quotedMessage.optString("role").equals("user")
                ? "引用你的消息\n" : "引用 AI 消息\n") + quotePreview(quotedMessage));
    }
    private void acceptImportedFile(AIAssistantController.ImportedFile file, String target) {
        if (deletedSessions.contains(target)) {
            controller.discardComposerMedia(file);
            return;
        }
        if (!file.image && !file.mimeType.startsWith("video/")) {
            String reference = "\n请查看工作区文件：" + file.relativePath + "\n";
            if (target.equals(selected)) composer.append(reference);
            else drafts.put(target, drafts.getOrDefault(target, "") + reference);
            return;
        }
        attachments.computeIfAbsent(target, ignored -> new ArrayList<>()).add(file);
        if (target.equals(selected)) { updateAttachments(); updateSend(); }
    }
    // 只重绘当前会话的横向草稿托盘；缩略图、序号和移除操作都绑定同一份附件列表。
    private void updateAttachments() {
        if (attachmentTray == null) return;
        attachmentTray.removeAllViews();
        if (!currentAttachments().isEmpty() && voiceMode) setVoiceMode(false);
        composer.setHint(currentAttachments().isEmpty() ? "可按住转文字" : "添加说明（可选）");
        int imageOrdinal = 0;
        int videoOrdinal = 0;
        for (AIAssistantController.ImportedFile file : currentAttachments()) {
            int ordinal = file.image ? ++imageOrdinal : ++videoOrdinal;
            LinearLayout chip = column();
            chip.setPadding(dp(5), dp(5), dp(5), dp(5));
            chip.setBackground(MaiChatTheme.bordered(MaiChatTheme.BLUE_SOFT,
                MaiChatTheme.BORDER, 9, activity));
            LinearLayout top = row();
            File source = controller.workspaceFile(file.relativePath);
            if (file.image) {
                ImageView preview = new ImageView(activity);
                preview.setScaleType(ImageView.ScaleType.CENTER_CROP);
                preview.setContentDescription("预览图片");
                if (source != null)
                    MessageImageLoader.load(source.getPath(), dp(82), dp(58), preview, null);
                preview.setOnClickListener(view -> previewAttachment(file));
                top.addView(preview, new LayoutParams(dp(82), dp(58)));
            } else {
                FrameLayout preview = new FrameLayout(activity);
                preview.setBackgroundColor(Color.rgb(38, 46, 58));
                preview.setContentDescription("预览视频 " + ordinal);
                ImageView cover = new ImageView(activity);
                cover.setScaleType(ImageView.ScaleType.CENTER_CROP);
                preview.addView(cover, new FrameLayout.LayoutParams(
                    LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
                if (source != null)
                    MessageImageLoader.load(source.getPath(), dp(82), dp(58), cover, null);
                TextView play = text("▶", 20, Color.WHITE);
                play.setGravity(Gravity.CENTER);
                play.setBackground(MaiChatTheme.rounded(Color.argb(180, 0, 0, 0), 18, activity));
                FrameLayout.LayoutParams playParams = new FrameLayout.LayoutParams(dp(36), dp(36));
                playParams.gravity = Gravity.CENTER;
                preview.addView(play, playParams);
                preview.setOnClickListener(view -> previewAttachment(file));
                top.addView(preview, new LayoutParams(dp(82), dp(58)));
            }
            Button remove = button("×", "移除" + (file.image ? "图片" : "视频"), view -> {
                List<AIAssistantController.ImportedFile> values = attachments.get(selected);
                if (values != null) {
                    values.remove(file);
                    if (values.isEmpty()) attachments.remove(selected);
                    controller.discardComposerMedia(file);
                }
                updateAttachments(); updateSend();
            });
            remove.setEnabled(!submitting);
            top.addView(remove, new LayoutParams(dp(48), dp(58)));
            chip.addView(top, matchWrap());
            TextView name = text((file.image ? "图片 " : "视频 ") + ordinal
                + " · " + file.displayName, 11, MaiChatTheme.TEXT);
            name.setSingleLine(true);
            name.setEllipsize(android.text.TextUtils.TruncateAt.END);
            name.setContentDescription((file.image ? "图片 " : "视频 ") + ordinal
                + "，" + file.displayName);
            chip.addView(name, matchWrap());
            LayoutParams params = matchWrap();
            params.width = dp(140);
            params.setMargins(0, dp(4), dp(6), dp(4));
            attachmentTray.addView(chip, params);
        }
        int pendingCount = pendingMediaImports.getOrDefault(selected, 0);
        if (pendingCount > 0) {
            TextView loading = text("正在添加媒体（" + pendingCount + "）…", 12,
                MaiChatTheme.SECONDARY);
            loading.setGravity(Gravity.CENTER);
            loading.setPadding(dp(12), dp(5), dp(12), dp(5));
            attachmentTray.addView(loading, new LayoutParams(dp(126), dp(82)));
        }
        int attachmentCountValue = currentAttachments().size();
        attachmentCount.setText(attachmentCountValue == 0 ? "待发送附件"
            : "待发送附件  " + attachmentCountValue);
        attachmentSection.setVisibility(
            attachmentCountValue > 0 || pendingCount > 0 ? VISIBLE : GONE);
    }
    private void previewAttachment(AIAssistantController.ImportedFile file) {
        File source = controller.workspaceFile(file.relativePath);
        if (source == null || !source.isFile() || !(activity instanceof MainActivity)
            || !((MainActivity) activity).openAgentMedia(
                source.getAbsolutePath(), !file.image, draftMediaPlaylist()))
            Toast.makeText(activity, "媒体文件无法预览", Toast.LENGTH_SHORT).show();
    }
    private List<MainActivity.GalleryItem> draftMediaPlaylist() {
        List<MainActivity.GalleryItem> result = new ArrayList<>();
        for (AIAssistantController.ImportedFile file : currentAttachments()) {
            File source = controller.workspaceFile(file.relativePath);
            if (source != null && source.isFile())
                result.add(new MainActivity.GalleryItem(source.getAbsolutePath(), !file.image));
        }
        return result;
    }
    private void openMediaPicker() {
        hideAttachmentPanel();
        pendingMediaSession = selected;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .setType("*/*")
            .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"image/*", "video/*"})
            .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            .addCategory(Intent.CATEGORY_OPENABLE);
        activity.startActivityForResult(intent, REQUEST_IMAGE);
    }
    private void openTextFilePicker() {
        hideAttachmentPanel();
        pendingFileSession = selected;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .setType("text/*").addCategory(Intent.CATEGORY_OPENABLE);
        activity.startActivityForResult(intent, REQUEST_FILE);
    }
    // 系统选择器返回后按启动时的会话归档，避免导入期间切换对话造成错发。
    boolean handleActivityResult(int requestCode, Intent data) {
        if (requestCode == REQUEST_FILE) {
            String target = pendingFileSession == null ? selected : pendingFileSession;
            pendingFileSession = null;
            if (data != null && data.getData() != null) importFile(data.getData(), target);
            return true;
        }
        if (requestCode == REQUEST_IMAGE) {
            String target = pendingMediaSession == null ? selected : pendingMediaSession;
            pendingMediaSession = null;
            LinkedHashSet<Uri> chosen = new LinkedHashSet<>();
            if (data != null && data.getClipData() != null)
                for (int i = 0; i < data.getClipData().getItemCount(); i++)
                    chosen.add(data.getClipData().getItemAt(i).getUri());
            else if (data != null && data.getData() != null)
                chosen.add(data.getData());
            int existing = attachments.getOrDefault(target, Collections.emptyList()).size();
            int available = Math.max(0, MAX_MEDIA_ATTACHMENTS - existing
                - pendingMediaImports.getOrDefault(target, 0));
            if (chosen.size() > available)
                Toast.makeText(activity, "每条消息最多添加 10 个图片或视频", Toast.LENGTH_LONG).show();
            for (Uri uri : chosen) {
                if (available-- <= 0) break;
                beginMediaImport(target);
                controller.importComposerMedia(uri, imported -> {
                    finishMediaImport(target);
                    if (imported != null) acceptImportedFile(imported, target);
                });
            }
            return true;
        }
        if (requestCode == REQUEST_CAMERA) {
            File file = pendingCameraFile;
            String target = pendingCameraSession == null ? selected : pendingCameraSession;
            pendingCameraFile = null;
            pendingCameraSession = null;
            if (file != null) {
                beginMediaImport(target);
                controller.importCameraFile(file, imported -> {
                    finishMediaImport(target);
                    if (imported != null) acceptImportedFile(imported, target);
                });
            }
            return true;
        }
        return false;
    }
    void handleActivityCancelled(int requestCode) {
        if (requestCode == REQUEST_FILE) pendingFileSession = null;
        if (requestCode == REQUEST_IMAGE) pendingMediaSession = null;
        if (requestCode == REQUEST_CAMERA && pendingCameraFile != null) {
            pendingCameraFile.delete();
            pendingCameraFile = null;
        }
        if (requestCode == REQUEST_CAMERA) pendingCameraSession = null;
    }
    private void beginMediaImport(String target) {
        pendingMediaImports.put(target, pendingMediaImports.getOrDefault(target, 0) + 1);
        if (target.equals(selected)) { updateAttachments(); updateSend(); }
    }
    private void finishMediaImport(String target) {
        int remaining = pendingMediaImports.getOrDefault(target, 0) - 1;
        if (remaining > 0) pendingMediaImports.put(target, remaining);
        else pendingMediaImports.remove(target);
        if (target.equals(selected)) { updateAttachments(); updateSend(); }
    }
    void onCameraPermission(boolean granted) {
        if (granted) openCamera();
        else {
            pendingCameraSession = null;
            Toast.makeText(activity, "没有相机权限，无法拍照", Toast.LENGTH_LONG).show();
        }
    }
    void onAudioPermission(boolean granted) {
        Toast.makeText(activity, granted ? "麦克风已启用，请按住语音按钮"
            : "没有麦克风权限，无法语音转文字", Toast.LENGTH_LONG).show();
    }
    private void requestCamera() {
        if (currentAttachments().size() + pendingMediaImports.getOrDefault(selected, 0)
            >= MAX_MEDIA_ATTACHMENTS) {
            Toast.makeText(activity, "每条消息最多添加 10 个图片或视频", Toast.LENGTH_LONG).show();
            return;
        }
        pendingCameraSession = selected;
        if (activity.checkSelfPermission(Manifest.permission.CAMERA)
            == android.content.pm.PackageManager.PERMISSION_GRANTED) openCamera();
        else activity.requestPermissions(new String[] {Manifest.permission.CAMERA},
            REQUEST_CAMERA_PERMISSION);
    }
    private void openCamera() {
        controller.prepareCameraFile(file -> {
            if (file == null) { pendingCameraSession = null; return; }
            pendingCameraFile = file;
            Uri output = FileProvider.getUriForFile(activity,
                activity.getPackageName() + ".files", file);
            Intent intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE)
                .putExtra(MediaStore.EXTRA_OUTPUT, output)
                .addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
            activity.startActivityForResult(intent, REQUEST_CAMERA);
        });
    }
    private void modelMenu() {
        if (state == null) return;
        String[] choices = {"glm-5.3", "glm-5.3-flash", "deepseek-flash"};
        new AlertDialog.Builder(activity).setTitle("选择模型").setItems(choices, (dialog, which) -> {
            if (state.busy()) {
                Toast.makeText(activity, "请先停止当前任务再切换模型", Toast.LENGTH_LONG).show();
                return;
            }
            if (choices[which].equalsIgnoreCase(state.model)) return;
            controller.switchModel(choices[which], success -> {
                if (!success) Toast.makeText(activity, controller.state.error,
                    Toast.LENGTH_LONG).show();
            });
        }).show();
    }
    private void policyMenu() {
        if (state == null) return;
        String[] titles = {"请求批准", "帮我批准", "完全访问"};
        String[] values = {"on-request", "unless-trusted", "never"};
        int checked = state.policy.equals("never") ? 2
            : state.policy.equals("unless-trusted") ? 1 : 0;
        new AlertDialog.Builder(activity)
            .setTitle("操作权限")
            .setSingleChoiceItems(titles, checked, (dialog, which) -> {
                if (state.busy()) {
                    Toast.makeText(activity, "请先停止当前任务再修改权限", Toast.LENGTH_LONG).show();
                    return;
                }
                dialog.dismiss();
                controller.save(state.model, values[which], success -> {
                    if (!success) Toast.makeText(activity, controller.state.error,
                        Toast.LENGTH_LONG).show();
                });
            })
            .setNegativeButton("取消", null)
            .show();
    }
    private String policyTitle(String policy) {
        return policy.equals("never") ? "完全访问"
            : policy.equals("unless-trusted") ? "帮我批准" : "请求批准";
    }
    private boolean voiceTouch(MotionEvent event) {
        if (voiceRecorder == null || state == null || state.busy() || submitting
            || (!voiceRecording && !composer.getText().toString().trim().isEmpty())) return false;
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            if (activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                activity.requestPermissions(new String[] {Manifest.permission.RECORD_AUDIO},
                    REQUEST_AUDIO_PERMISSION);
                return true;
            }
            beginVoice(); return true;
        }
        if (!voiceRecording) return false;
        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            cancelVoice = event.getY() < -dp(60);
            voiceHoldButton.setText(cancelVoice ? "松开取消"
                : voicePartialText.isEmpty() ? "正在听，请说话…" : voicePartialText);
            updateHostVoiceOverlay(true, cancelVoice ? "松开取消" : voicePartialText);
            return true;
        }
        if (event.getActionMasked() == MotionEvent.ACTION_UP
            || event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
            finishVoice(cancelVoice || event.getActionMasked() == MotionEvent.ACTION_CANCEL);
            return true;
        }
        return true;
    }
    static final int REQUEST_AUDIO_PERMISSION = 7111;
    private void beginVoice() {
        int generation = ++voiceGeneration;
        VoiceRecordingController.AsrTicketProvider ticketProvider =
            activity instanceof MainActivity ?
                () -> MaiChatAuthClient.shared(activity).realtimeAsrTicket() : null;
        boolean started = voiceRecorder.tryStart(new VoiceRecordingController.Completion() {
            @Override public void partial(String text) {
                if (voiceRecording && generation == voiceGeneration) {
                    voicePartialText = text == null ? "" : text.trim();
                    composer.setText(voicePartialText);
                    updateHostVoiceOverlay(true, voicePartialText);
                    if (!cancelVoice)
                        voiceHoldButton.setText(voicePartialText.isEmpty()
                            ? "正在听，请说话…" : voicePartialText);
                }
            }
            @Override public void recognized(String text, File file, int seconds) {
                file.delete();
                if (generation != voiceGeneration || text == null || text.trim().isEmpty()) return;
                composer.setText(text.trim());
                send();
            }
            @Override public void finished(File file, int seconds) {
                if (speechRecognizer != null && speechRecognizer.isAvailable()) {
                    speechRecognizer.transcribe(file, file.getName().endsWith(".aac") ? "aac" : "m4a",
                        new SpeechRecognizer.Callback() {
                            @Override public void onText(String text) {
                                file.delete();
                                if (generation != voiceGeneration || text == null
                                    || text.trim().isEmpty()) {
                                    Toast.makeText(activity, "没有识别到文字", Toast.LENGTH_LONG).show();
                                    return;
                                }
                                composer.setText(text.trim()); send();
                            }
                            @Override public void onError(String message) {
                                file.delete();
                                if (generation == voiceGeneration)
                                    Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
                            }
                        });
                } else {
                    file.delete();
                    if (generation == voiceGeneration)
                        Toast.makeText(activity, "没有识别到文字", Toast.LENGTH_LONG).show();
                }
            }
            @Override public void failed() {
                updateHostVoiceOverlay(false, "");
                if (generation == voiceGeneration)
                    Toast.makeText(activity, "语音转文字失败", Toast.LENGTH_LONG).show();
            }
        }, ticketProvider);
        if (!started) {
            Toast.makeText(activity, "麦克风正在被其他录音使用", Toast.LENGTH_LONG).show();
            return;
        }
        voiceRecording = true; cancelVoice = false; voicePartialText = "";
        voiceHoldButton.setText("正在听，请说话…");
        updateHostVoiceOverlay(true, "");
    }
    private void finishVoice(boolean cancel) {
        if (!voiceRecording) return;
        voiceRecording = false; cancelVoice = false; voicePartialText = "";
        voiceHoldButton.setText("按住转文字");
        updateHostVoiceOverlay(false, "");
        if (cancel) {
            voiceGeneration++;
            composer.setText("");
        }
        voiceRecorder.finish(cancel);
    }
    private void updateHostVoiceOverlay(boolean active, String text) {
        if (activity instanceof MainActivity)
            ((MainActivity) activity).setAssistantVoiceTranscript(active, text);
    }
    private JSONObject value(String... pairs) {
        JSONObject value = new JSONObject();
        try {
            for (int i = 0; i < pairs.length; i += 2) value.put(pairs[i], pairs[i + 1]);
        } catch (Exception e) {
            throw new IllegalArgumentException(e);
        }
        return value;
    }
    private boolean isPaidGeneration(String tool) {
        return tool.equals("wan_video") || tool.equals("wan_image")
            || tool.equals("seedance_video") || tool.equals("seedream_image")
            || tool.equals("glm_image")
            || tool.equals("kling_bailian_video") || tool.equals("kling_bailian_image")
            || tool.equals("minimax_video") || tool.equals("minimax_image");
    }
    private String paidApprovalTitle(String tool, JSONObject input) {
        boolean video = tool.equals("wan_video")
            || tool.equals("seedance_video")
            || tool.equals("kling_bailian_video") || tool.equals("minimax_video");
        return "确认" + (isPaidRevision(tool, input) ? "编辑" : "生成")
            + (video ? "视频" : "图片");
    }
    private boolean isPaidRevision(String tool, JSONObject input) {
        return input.optString("action").equals("revise")
            || input.optString("mode").equals("edit")
            || input.optString("mode").equals("extend")
            || input.optString("task_mode").equals("edit");
    }
    private String paidProvider(String tool) {
        if (tool.equals("wan_video") || tool.equals("wan_image")) return "万相";
        if (tool.equals("glm_image")) return "GLM";
        if (tool.equals("kling_bailian_video") || tool.equals("kling_bailian_image")) return "可灵 · 百炼";
        if (tool.equals("minimax_video") || tool.equals("minimax_image")) return "海螺 / MiniMax";
        return tool.equals("seedance_video") ? "Seedance" : "Seedream";
    }
    // 作用：依据工具名称和输入整理付费媒体任务的规格提示。
    private String paidSpecs(String tool, JSONObject input) {
        JSONObject production = input.optJSONObject("production");
        if (production == null) production = new JSONObject();
        java.util.List<String> values = new java.util.ArrayList<>();
        String model = input.optString("model");
        if (tool.equals("seedance_video")) {
            if (model.equals("doubao-seedance-2-0-fast-260128")) values.add("Seedance 2.0 Fast");
            else if (model.equals("doubao-seedance-2-0-mini-260615")) values.add("Seedance 2.0 Mini");
            else if (model.equals("doubao-seedance-2-0-260128")) values.add("Seedance 2.0");
            else if (model.equals("doubao-seedance-2-5-260628")) values.add("Seedance 2.5");
            else values.add(model.isEmpty() && input.optString("action").equals("revise")
                ? "沿用上一任务型号" : (model.isEmpty() ? "Seedance 2.0 Mini" : model));
        }
        if (model.equals("MiniMax-H3") || model.equals("MiniMax-H3-Max"))
            values.add(model.equals("MiniMax-H3-Max") ? "H3 Max" : "H3");
        if (tool.equals("minimax_image"))
            values.add(model.equals("image-01-live") ? "Image-01 Live" : "Image-01");
        if (tool.equals("kling_bailian_video") || tool.equals("kling_bailian_image"))
            values.add(model.contains("omni") ? "可灵 V3 Omni"
                : model.contains("turbo") ||
                    (model.isEmpty() && tool.equals("kling_bailian_video"))
                    ? "可灵 V3 Turbo" : "可灵 V3");
        int duration = input.optInt("duration", production.optInt("duration", 0));
        if (duration > 0) values.add(duration + " 秒");
        if (input.has("source_duration_seconds"))
            values.add(String.format(java.util.Locale.ROOT, "源片 %.1f 秒",
                input.optDouble("source_duration_seconds")));
        String resolution = input.optString("resolution", production.optString("resolution"));
        String ratio = input.optString("ratio", production.optString("ratio"));
        if (!resolution.isEmpty()) values.add(resolution);
        if (!ratio.isEmpty()) values.add(ratio.equals("adaptive") ? "按素材比例" : ratio);
        if (!input.optString("virtual_avatar_asset_id").isEmpty())
            values.add("平台虚拟人像");
        if (!input.optString("authorized_portrait_asset_id").isEmpty())
            values.add("已授权真人形象");
        JSONArray registeredAssets = input.optJSONArray("reference_asset_ids");
        if (registeredAssets != null && registeredAssets.length() > 0)
            values.add("已登记参考素材 " + registeredAssets.length() + " 张");
        int strictFrameAssets = !input.optString("first_frame_asset_id").isEmpty() ? 1 : 0;
        if (!input.optString("last_frame_asset_id").isEmpty()) strictFrameAssets++;
        if (strictFrameAssets > 0) values.add("已登记严格帧 " + strictFrameAssets + " 张");
        if (!input.optString("video_asset_id").isEmpty()) values.add("已登记视频素材");
        if (!input.optString("asset_group_type").isEmpty())
            values.add(input.optString("asset_group_type").equals("LivenessFace")
                ? "真人授权素材组" : "虚拟素材组");
        if (!input.optString("size").isEmpty()) values.add(input.optString("size"));
        JSONArray images = input.optJSONArray("local_image_paths");
        if (images == null) images = input.optJSONArray("image_paths");
        int imageCount = images == null ? 0 : images.length();
        if (!input.optString("image_path").isEmpty()
            || !input.optString("image_url").isEmpty()
            || !input.optString("first_frame").isEmpty()) imageCount++;
        if (!input.optString("last_frame_path").isEmpty()
            || !input.optString("last_frame").isEmpty()) imageCount++;
        JSONArray content = input.optJSONArray("content");
        int videoCount = !input.optString("video_path").isEmpty()
            || !input.optString("reference_video_path").isEmpty()
            || !input.optString("reference_video").isEmpty()
            || !input.optString("video_url").isEmpty()
            || !input.optString("video_asset_id").isEmpty()
            || !input.optString("video_task_id").isEmpty() ? 1 : 0;
        int audioCount = !input.optString("reference_audio_path").isEmpty()
            || !input.optString("reference_audio").isEmpty() ? 1 : 0;
        if (content != null) {
            for (int index = 0; index < content.length(); index++) {
                JSONObject item = content.optJSONObject(index);
                if (item == null) continue;
                switch (item.optString("type")) {
                    case "image_url": imageCount++; break;
                    case "video_url": videoCount++; break;
                    case "audio_url": audioCount++; break;
                    default: break;
                }
            }
        }
        if (imageCount > 0) values.add("参考图片 " + imageCount + " 张");
        if (videoCount > 0) values.add("参考视频 " + videoCount + " 个");
        if (audioCount > 0) values.add("参考音频 " + audioCount + " 个");
        return android.text.TextUtils.join(" · ", values);
    }
    private String mediaDisplayText(String text, boolean hasMedia) {
        if (!hasMedia) return text;
        String visible = text.trim();
        String[] automatic = {"请查看这张图片。", "请查看这些图片。",
            "请查看这个视频。", "请查看这些视频。", "请查看这些媒体。"};
        boolean changed;
        do {
            changed = false;
            for (String phrase : automatic) {
                if (visible.equals(phrase)) {
                    visible = "";
                    changed = true;
                    break;
                }
                if (visible.endsWith("\n" + phrase)) {
                    visible = visible.substring(0, visible.length() - phrase.length()).trim();
                    changed = true;
                    break;
                }
            }
        } while (changed);
        return visible;
    }
    // 作用：同步创作中心待处理卡片、权限提示和问题反馈。
    // 页面状态变化和用户操作在这里衔接；退出或失败时维持已显示内容。
    private void updatePending() {
        JSONArray permissions = state.data.optJSONArray("permissions"),
                  questions = state.data.optJSONArray("questions");
        String signature = String.valueOf(permissions) + String.valueOf(questions);
        if (signature.equals(pendingSignature))
            return;
        pendingSignature = signature;
        pending.removeAllViews();
        if (permissions != null)
            for (int i = 0; i < permissions.length(); i++) {
                JSONObject permission = permissions.optJSONObject(i);
                if (permission == null)
                    continue;
                String id = permission.optString("id");
                String tool = permission.optString("tool");
                boolean paid = isPaidGeneration(tool);
                JSONObject input;
                try { input = new JSONObject(permission.optString("input", "{}")); }
                catch (Exception ignored) { input = new JSONObject(); }
                boolean providerCanCancel = tool.equals("wan_video") ||
                    tool.equals("seedance_video") || tool.equals("kling_bailian_video");
                boolean taskManagement = ((providerCanCancel ||
                    tool.equals("minimax_video")) &&
                    input.optString("action").equals("delete")) ||
                    (providerCanCancel && input.optString("action").equals("cancel"));
                boolean taskDelete = input.optString("action").equals("delete");
                paid = paid && !taskManagement;
                boolean portraitValidation = tool.equals("ark_assets") &&
                    input.optString("action").equals("begin_real_validation");
                if (taskManagement) {
                    pending.addView(text(taskDelete ? "删除视频任务记录" : "取消视频任务",
                        17, MaiChatTheme.TEXT), matchWrap());
                    pending.addView(text(taskDelete
                        ? (tool.equals("seedance_video")
                           ? "删除方舟中的已结束任务记录及本地索引；已保存的视频保留。"
                           : "只删除本地已交付记录；云端任务和已保存的视频保留。")
                        : "仅排队中的任务可以取消；运行中任务会被平台拒绝。",
                        13, MaiChatTheme.SECONDARY), matchWrap());
                    pending.addView(text("任务：" + input.optString("conversation_id"),
                        13, MaiChatTheme.SECONDARY), matchWrap());
                } else if (paid) {
                    pending.addView(text(paidApprovalTitle(tool, input), 17, MaiChatTheme.TEXT), matchWrap());
                    JSONArray registeredAssets = input.optJSONArray("reference_asset_ids");
                    boolean usesRegisteredAsset =
                        registeredAssets != null && registeredAssets.length() > 0
                        || !input.optString("first_frame_asset_id").isEmpty()
                        || !input.optString("last_frame_asset_id").isEmpty()
                        || !input.optString("video_asset_id").isEmpty();
                    String note = usesRegisteredAsset
                        ? "将使用已登记的参考素材；平台仍会审核。确认后提交给"
                            + paidProvider(tool) + "，可能消耗模型额度。"
                        : !input.optString("virtual_avatar_asset_id").isEmpty()
                        ? "将使用平台虚拟人像，不保留真实人物长相。确认后提交给"
                            + paidProvider(tool) + "，可能消耗模型额度。"
                        : !input.optString("authorized_portrait_asset_id").isEmpty()
                            ? "将使用已授权真人形象。确认后提交给" + paidProvider(tool)
                                + "，可能消耗模型额度。"
                            : "确认后将提交给" + paidProvider(tool) + "，可能消耗模型额度。";
                    pending.addView(text(note, 13, MaiChatTheme.SECONDARY), matchWrap());
                    String request = input.optString("message", input.optString("prompt"));
                    if (!request.isEmpty()) {
                        TextView summary = text(request, 15, MaiChatTheme.TEXT);
                        summary.setMaxLines(5);
                        summary.setOnClickListener(v -> details("任务内容", request));
                        pending.addView(summary, matchWrap());
                    }
                    String specs = paidSpecs(tool, input);
                    if (!specs.isEmpty())
                        pending.addView(text(specs, 13, MaiChatTheme.SECONDARY), matchWrap());
                    TextView more = text("查看完整请求", 12, MaiChatTheme.SECONDARY);
                    more.setOnClickListener(v -> details("完整请求", permission.optString("input")));
                    pending.addView(more, matchWrap());
                } else if (portraitValidation) {
                    pending.addView(text("创建真人验证链接", 17, MaiChatTheme.TEXT), matchWrap());
                    pending.addView(text("将创建一次性 H5 链接，"
                        + "请由照片中的本人打开并完成验证。"
                        + "认证凭证有效期为 30 分钟。",
                        13, MaiChatTheme.SECONDARY), matchWrap());
                } else {
                    TextView heading = text(
                        "允许执行 " + tool + "？ 点击查看参数", 13, MaiChatTheme.TEXT);
                    heading.setOnClickListener(v -> details("操作参数", permission.optString("input")));
                    pending.addView(heading, matchWrap());
                }
                LinearLayout choices = row();
                java.util.List<String[]> availableChoices = new java.util.ArrayList<>();
                availableChoices.add(new String[] {
                    taskManagement ? "返回" : paid || portraitValidation ? "取消" : "拒绝", "denied"});
                availableChoices.add(new String[] {
                    taskManagement ? (taskDelete ? "确认删除" : "确认取消任务")
                    : paid ? (isPaidRevision(tool, input) ? "确认编辑" : "确认生成")
                    : portraitValidation ? "生成验证链接"
                    : permission.optBoolean("rememberOnApproval")
                        ? (permission.optInt("fileCount", 0) > 1
                            ? "允许并记住这些文件" : "允许并记住此文件")
                        : "允许一次", "approved"});
                if (!portraitValidation && !taskManagement && permission.optBoolean("allowForSession", true)
                    && !permission.optBoolean("rememberOnApproval"))
                    availableChoices.add(new String[] {"本会话允许", "approved_for_session"});
                for (String[] choice : availableChoices)
                    choices.addView(
                        button(choice[0], choice[0],
                            v
                            -> controller.action("permission", value("id", id, "decision", choice[1]), null)),
                        new LayoutParams(0, dp(42), 1));
                pending.addView(choices, matchWrap());
            }
        if (questions != null)
            for (int i = 0; i < questions.length(); i++) {
                JSONObject question = questions.optJSONObject(i);
                if (question == null)
                    continue;
                String id = question.optString("id");
                pending.addView(text(question.optString("question"), 14, MaiChatTheme.TEXT), matchWrap());
                JSONArray options = question.optJSONArray("options");
                if (options != null)
                    for (int j = 0; j < options.length(); j++) {
                        String option = options.optString(j);
                        pending.addView(
                            button(option, option,
                                v -> controller.action("answer", value("id", id, "text", option), null)),
                            matchWrap());
                    }
                LinearLayout answer = row();
                EditText input = new EditText(activity);
                input.setHint("你的回答");
                input.setTextSize(14);
                answer.addView(input, new LayoutParams(0, dp(42), 1));
                answer.addView(button("回复", "回复", v -> {
                    if (!input.getText().toString().trim().isEmpty())
                        controller.action(
                            "answer", value("id", id, "text", input.getText().toString()), null);
                }));
                pending.addView(answer, matchWrap());
            }
    }
    private void sessions() {
        if (submitting || navigationPending) return;
        if (state == null) return;
        JSONArray items = state.data.optJSONArray("sessions");
        if (items == null) return;
        // 尚未发送的空白会话也要能从列表返回，否则创建新对话会让草稿失去入口。
        boolean blankDraft = !attachments.getOrDefault("", Collections.emptyList()).isEmpty()
            || pendingMediaImports.getOrDefault("", 0) > 0
            || !(selected.isEmpty() ? composer.getText().toString()
                : drafts.getOrDefault("", "")).trim().isEmpty();
        FrameLayout overlay = new FrameLayout(activity);
        overlay.setBackgroundColor(Color.argb(42, 0, 0, 0));
        LinearLayout drawer = column();
        drawer.setBackgroundColor(Color.WHITE);
        drawer.setElevation(dp(18));
        LinearLayout drawerHeader = row();
        drawerHeader.setPadding(dp(18), 0, dp(12), 0);
        TextView drawerTitle = text("对话", 18, MaiChatTheme.TEXT);
        drawerTitle.setTypeface(MaiChatTypography.semibold());
        drawerHeader.addView(drawerTitle, new LayoutParams(0, dp(54), 1));
        ImageButton create = circularIcon(R.drawable.ic_ai_add, "新对话", false, null);
        drawerHeader.addView(create, new LayoutParams(dp(40), dp(40)));
        drawer.addView(drawerHeader, matchWrap());
        ScrollView scroll = new ScrollView(activity);
        LinearLayout sessionList = column();
        sessionList.setPadding(dp(10), dp(6), dp(10), dp(18));
        scroll.addView(sessionList);
        drawer.addView(scroll, new LayoutParams(LayoutParams.MATCH_PARENT, 0, 1));
        int drawerWidth = Math.min(dp(360),
            Math.round(activity.getResources().getDisplayMetrics().widthPixels * 0.86f));
        FrameLayout.LayoutParams drawerParams = new FrameLayout.LayoutParams(
            drawerWidth, LayoutParams.MATCH_PARENT, Gravity.START);
        overlay.addView(drawer, drawerParams);
        PopupWindow popup = new PopupWindow(
            overlay, LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT, true);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setOutsideTouchable(true);
        overlay.setOnClickListener(v -> popup.dismiss());
        drawer.setOnClickListener(v -> {});
        create.setOnClickListener(v -> {
            popup.dismiss();
            requestNewConversation();
        });
        if (blankDraft)
            sessionList.addView(sessionDrawerRow("未发送草稿", "", selected.isEmpty(), v -> {
                popup.dismiss();
                requestConversation("");
            }), matchWrap());
        for (int i = 0; i < items.length(); i++) {
            JSONObject session = items.optJSONObject(i);
            if (session == null) continue;
            String id = session.optString("id");
            String title = session.optString("title");
            if (title.equals("New session")) title = "新对话";
            String detail = session.optBoolean("busy") ? "正在处理" : "";
            sessionList.addView(sessionDrawerRow(title, detail, id.equals(selected), v -> {
                popup.dismiss();
                requestConversation(id);
            }), matchWrap());
        }
        popup.showAtLocation(this, Gravity.START | Gravity.TOP, 0, 0);
        drawer.setTranslationX(-drawerWidth);
        drawer.animate().translationX(0).setDuration(180).start();
    }
    private void requestNewConversation() {
        if (submitting || navigationPending) return;
        following = true;
        navigationPending = true;
        int generation = ++navigationGeneration;
        updateSend();
        controller.create(success -> finishNavigation(generation));
    }
    private void requestConversation(String target) {
        if (submitting || navigationPending) return;
        following = true;
        navigationPending = true;
        int generation = ++navigationGeneration;
        updateSend();
        controller.select(target, success -> finishNavigation(generation));
    }
    private void finishNavigation(int generation) {
        if (navigationGeneration != generation) return;
        navigationPending = false;
        updateSend();
    }
    private void menu(View anchor) {
        if (submitting || navigationPending) return;
        if (state == null) return;
        LinearLayout panel = column();
        panel.setPadding(0, dp(4), 0, dp(4));
        panel.setBackground(MaiChatTheme.bordered(Color.WHITE, Color.rgb(232, 235, 240), 14, activity));
        PopupWindow popup = new PopupWindow(panel, dp(264), LayoutParams.WRAP_CONTENT, true);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setOutsideTouchable(true);
        String modelValue = state.model.isEmpty() ? AIAssistantController.DEFAULT_MODEL : state.model;
        panel.addView(actionPanelRow("模型", modelValue, MaiChatTheme.TEXT, v -> {
            popup.dismiss();
            modelMenu();
        }), matchWrap());
        panel.addView(actionPanelDivider(), new LayoutParams(LayoutParams.MATCH_PARENT, dp(1)));
        panel.addView(actionPanelRow("操作权限", policyTitle(state.policy), MaiChatTheme.TEXT, v -> {
            popup.dismiss();
            policyMenu();
        }), matchWrap());
        panel.addView(actionPanelDivider(), new LayoutParams(LayoutParams.MATCH_PARENT, dp(1)));
        View clear = actionPanelRow("清空当前对话", "",
            selected.isEmpty() || state.busy() ? MaiChatTheme.SECONDARY : MaiChatTheme.RED, v -> {
            popup.dismiss();
            confirmClearConversation();
        });
        clear.setEnabled(!selected.isEmpty() && !state.busy());
        panel.addView(clear, matchWrap());
        panel.measure(MeasureSpec.UNSPECIFIED, MeasureSpec.UNSPECIFIED);
        popup.setElevation(dp(14));
        popup.showAsDropDown(anchor, anchor.getWidth() - dp(264), dp(6));
    }
    private void confirmClearConversation() {
        if (state == null || state.busy() || selected.isEmpty()) return;
        String target = selected;
        new AlertDialog.Builder(activity)
            .setMessage("清空当前对话的所有消息？")
            .setNegativeButton("取消", null)
            .setPositiveButton("确定",
                (dialog, which) -> controller.action("clear", value("session", target), null))
            .show();
    }
    private void discardDeletedSession(String target) {
        deletedSessions.add(target);
        List<AIAssistantController.ImportedFile> removed = attachments.remove(target);
        if (removed != null)
            for (AIAssistantController.ImportedFile file : removed)
                controller.discardComposerMedia(file);
        pendingMediaImports.remove(target);
        drafts.remove(target);
        if (selected.equals(target)) {
            composer.setText("");
            updateAttachments();
            updateSend();
        }
    }
    // 作用：在账号会话建立后重新领取所选模型凭据，不改变当前会话和输入草稿。
    private void retryMainModelConnection() {
        if (state == null || !state.ready) return;
        controller.save(state.model, state.policy, success -> {
            if (!success) Toast.makeText(activity, controller.state.error,
                Toast.LENGTH_LONG).show();
        });
    }
    private void details(String title, String source) {
        TextView view = text(source, 12, MaiChatTheme.TEXT);
        view.setTextIsSelectable(true);
        view.setTypeface(android.graphics.Typeface.MONOSPACE);
        view.setPadding(dp(16), dp(12), dp(16), dp(12));
        ScrollView scroller = new ScrollView(activity);
        scroller.addView(view);
        new AlertDialog.Builder(activity)
            .setTitle(title)
            .setView(scroller)
            .setPositiveButton("完成", null)
            .show();
    }
    private final class MessageAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return messages.isEmpty() ? 1 : messages.size();
        }
        @Override
        public Object getItem(int position) {
            return messages.isEmpty() ? null : messages.get(position);
        }
        @Override
        public long getItemId(int position) {
            JSONObject m = (JSONObject) getItem(position);
            return m == null ? -1 : m.optString("id").hashCode();
        }
        @Override
        public boolean hasStableIds() {
            return false;
        }
        @Override
        public View getView(int position, View convert, ViewGroup parent) {
            MessageRow row = convert instanceof MessageRow ? (MessageRow) convert : new MessageRow();
            row.bind((JSONObject) getItem(position));
            return row;
        }
    }
    private final class MessageRow extends LinearLayout {
        JSONObject message;
        String key = "";
        final Map<String, TextView> parts = new HashMap<>();
        final Map<String, ImageView> imageParts = new HashMap<>();
        final Map<String, LinearLayout> videoParts = new HashMap<>();
        final Map<String, Button> pdfCards = new HashMap<>();
        final Map<String, Button> diagnosticCards = new HashMap<>();
        final Map<String, LinearLayout> mediaCards = new HashMap<>();
        TextView status;
        TextView toolSummary;
        LinearLayout toolDetails;
        MessageRow() {
            super(activity);
            setOrientation(VERTICAL);
            setPadding(0, dp(6), 0, dp(6));
        }
        void bind(JSONObject value) {
            message = value;
            if (value == null) {
                if (!key.equals("empty")) {
                    key = "empty";
                    removeAllViews();
                    parts.clear();
                    imageParts.clear();
                    videoParts.clear();
                    pdfCards.clear();
                    diagnosticCards.clear();
                    mediaCards.clear();
                    status = null;
                    toolSummary = null;
                    toolDetails = null;
                    setGravity(Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
                    LinearLayout welcome = column();
                    welcome.setGravity(Gravity.CENTER_HORIZONTAL);
                    welcome.setPadding(0, dp(70), 0, dp(70));
                    ImageView sparkles = new ImageView(activity);
                    sparkles.setImageResource(R.drawable.ic_ai_sparkles);
                    sparkles.setContentDescription("创作中心");
                    welcome.addView(sparkles, new LayoutParams(dp(40), dp(40)));
                    TextView heading = text("有什么可以帮你？", 22, MaiChatTheme.TEXT);
                    heading.setTypeface(MaiChatTypography.semibold());
                    heading.setGravity(Gravity.CENTER);
                    LayoutParams headingParams = matchWrap();
                    headingParams.setMargins(0, dp(14), 0, 0);
                    welcome.addView(heading, headingParams);
                    TextView detail = text("可以聊天、分析文本和处理导入的文件。", 16,
                        MaiChatTheme.SECONDARY);
                    detail.setGravity(Gravity.CENTER);
                    LayoutParams detailParams = matchWrap();
                    detailParams.setMargins(0, dp(12), 0, 0);
                    welcome.addView(detail, detailParams);
                    if (state != null && !state.data.optBoolean("configured")) {
                        TextView accountHint = text("登录后会自动连接主聊天模型", 13,
                            MaiChatTheme.SECONDARY);
                        accountHint.setGravity(Gravity.CENTER);
                        LayoutParams hintParams = matchWrap();
                        hintParams.setMargins(0, dp(14), 0, 0);
                        welcome.addView(accountHint, hintParams);
                        Button retry = button("重试连接", "重试连接",
                            v -> retryMainModelConnection());
                        retry.setTextColor(Color.WHITE);
                        retry.setTextSize(14);
                        retry.setTypeface(MaiChatTypography.semibold());
                        retry.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 10, activity));
                        LayoutParams retryParams = new LayoutParams(dp(104), dp(38));
                        retryParams.setMargins(0, dp(16), 0, 0);
                        welcome.addView(retry, retryParams);
                    }
                    addView(welcome, matchWrap());
                }
                return;
            }
            setGravity(Gravity.NO_GRAVITY);
            setMinimumHeight(0);
            JSONArray array = value.optJSONArray("parts");
            if (array == null)
                array = new JSONArray();
            String toolGroupId = "tools:" + value.optString("id");
            StringBuilder structure = new StringBuilder(value.optString("id"));
            structure.append(expanded.contains(toolGroupId));
            for (int i = 0; i < array.length(); i++) {
                JSONObject part = array.optJSONObject(i);
                if (part == null || part.optString("kind").equals("reasoning")) continue;
                structure.append(part.optString("id"))
                    .append(expanded.contains(part.optString("id")))
                    .append(mediaFromToolOutput(part) != null);
            }
            JSONArray inlineMedia = value.optJSONArray("inline_media");
            if (inlineMedia != null)
                for (int i = 0; i < inlineMedia.length(); i++) {
                    JSONObject item = inlineMedia.optJSONObject(i);
                    if (item != null) structure.append(item.optString("path"));
                }
            boolean outgoing = value.optString("role").equals("user");
            if (!key.equals(structure.toString())) {
                key = structure.toString();
                removeAllViews();
                parts.clear();
                imageParts.clear();
                videoParts.clear();
                pdfCards.clear();
                diagnosticCards.clear();
                mediaCards.clear();
                status = null;
                toolSummary = null;
                toolDetails = null;
                if (outgoing) {
                    LinearLayout bubble = column();
                    TextView body = MaiChatTypography.body(activity);
                    for (int i = 0; i < array.length(); i++) {
                        JSONObject part = array.optJSONObject(i);
                        if (part == null || !part.optString("kind").equals("quote")) continue;
                        TextView quote = text("↩  " + part.optString("preview", "已引用消息"),
                            12, MaiChatTheme.SECONDARY);
                        quote.setMaxLines(2);
                        quote.setEllipsize(android.text.TextUtils.TruncateAt.END);
                        bubble.addView(quote, matchWrap());
                    }
                    bubble.addView(body, matchWrap());
                    for (int i = 0; i < array.length(); i++) {
                        JSONObject part = array.optJSONObject(i);
                        if (part == null || !part.optString("kind").equals("image")) continue;
                        ImageView image = new ImageView(activity);
                        image.setScaleType(ImageView.ScaleType.CENTER_CROP);
                        bubble.addView(image, new LayoutParams(dp(220), dp(150)));
                        imageParts.put(part.optString("id"), image);
                    }
                    for (int i = 0; i < array.length(); i++) {
                        JSONObject part = array.optJSONObject(i);
                        if (part == null || !part.optString("kind").equals("video")) continue;
                        LinearLayout video = column();
                        bubble.addView(video, matchWrap());
                        videoParts.put(part.optString("id"), video);
                    }
                    bubble.setPadding(dp(12), dp(10), dp(12), dp(10));
                    bubble.setBackground(MaiChatTheme.rounded(Color.rgb(244, 244, 247), 16, activity));
                    LayoutParams lp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
                    lp.gravity = Gravity.END;
                    lp.leftMargin = dp(28);
                    addView(bubble, lp);
                    TextView quoteAction = text("↩  引用", 12, MaiChatTheme.SECONDARY);
                    quoteAction.setContentDescription("引用这条消息");
                    quoteAction.setOnClickListener(view -> beginQuote(value));
                    LayoutParams quoteLayout = matchWrap();
                    quoteLayout.gravity = Gravity.END;
                    quoteLayout.topMargin = dp(4);
                    addView(quoteAction, quoteLayout);
                    parts.put("user", body);
                } else {
                    for (int i = 0; i < array.length(); i++) {
                        JSONObject part = array.optJSONObject(i);
                        String id = part.optString("id"), kind = part.optString("kind");
                        boolean deliveredMedia = kind.equals("tool") &&
                            mediaFromToolOutput(part) != null;
                        // 思考正文不创建视图；本行底部的状态文字已足够提示进度。
                        if (kind.equals("reasoning"))
                            continue;
                        if (kind.equals("tool") && !deliveredMedia && toolDetails == null) {
                            toolSummary = text("", 12, MaiChatTheme.SECONDARY);
                            toolSummary.setSingleLine(true);
                            toolSummary.setEllipsize(android.text.TextUtils.TruncateAt.END);
                            toolSummary.setContentDescription(toolGroupTitle(array));
                            toolSummary.setPadding(0, dp(4), 0, dp(4));
                            toolSummary.setOnClickListener(v -> {
                                if (!expanded.add(toolGroupId)) expanded.remove(toolGroupId);
                                adapter.notifyDataSetChanged();
                            });
                            LayoutParams summaryLayout = matchWrap();
                            summaryLayout.bottomMargin = dp(4);
                            addView(toolSummary, summaryLayout);
                            toolDetails = column();
                            toolDetails.setPadding(dp(12), 0, 0, 0);
                            toolDetails.setVisibility(expanded.contains(toolGroupId) ? VISIBLE : GONE);
                            addView(toolDetails, matchWrap());
                        }
                        TextView view = kind.equals("text") ? MaiChatTypography.body(activity)
                                                            : text("", 12, MaiChatTheme.SECONDARY);
                        parts.put(id, view);
                        LayoutParams lp = matchWrap();
                        lp.bottomMargin = kind.equals("tool") && !deliveredMedia ? dp(4) : dp(12);
                        if (kind.equals("tool") && !deliveredMedia) toolDetails.addView(view, lp);
                        else addView(view, lp);
                        if (!kind.equals("text"))
                            view.setOnClickListener(v -> {
                                if (!expanded.add(id))
                                    expanded.remove(id);
                                adapter.notifyDataSetChanged();
                            });
                        if (expanded.contains(id)) {
                            TextView detail = text("", 12, MaiChatTheme.SECONDARY);
                            detail.setTextIsSelectable(true);
                            detail.setTypeface(android.graphics.Typeface.MONOSPACE);
                            parts.put(id + ":detail", detail);
                            if (kind.equals("tool") && !deliveredMedia) toolDetails.addView(detail, lp);
                            else addView(detail, lp);
                        }
                        if (kind.equals("tool")) {
                            Button preview = button("预览 PDF", "预览生成的 PDF", clicked -> {
                                Object stored = clicked.getTag();
                                if (!(stored instanceof String)) return;
                                File pdf = controller.workspaceFile((String) stored);
                                PdfPreviewDialog.show(activity, pdf,
                                    controller.workspaceDirectoryForPreview());
                            });
                            preview.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                            preview.setPadding(dp(12), 0, dp(12), 0);
                            preview.setBackground(MaiChatTheme.bordered(
                                MaiChatTheme.BLUE_SOFT, MaiChatTheme.BORDER, 10, activity));
                            preview.setVisibility(GONE);
                            pdfCards.put(id, preview);
                            LayoutParams cardLayout = matchWrap();
                            cardLayout.bottomMargin = dp(12);
                            addView(preview, cardLayout);
                            Button diagnostic = button("分享排障日志", "分享导出的 TXT 日志", clicked -> {
                                Object stored = clicked.getTag();
                                if (!(stored instanceof String)) return;
                                File report = controller.workspaceFile((String) stored);
                                if (report != null && report.isFile()) shareDiagnosticFile(report);
                            });
                            diagnostic.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                            diagnostic.setPadding(dp(12), 0, dp(12), 0);
                            diagnostic.setBackground(MaiChatTheme.bordered(
                                MaiChatTheme.BLUE_SOFT, MaiChatTheme.BORDER, 10, activity));
                            diagnostic.setVisibility(GONE);
                            diagnosticCards.put(id, diagnostic);
                            LayoutParams diagnosticLayout = matchWrap();
                            diagnosticLayout.bottomMargin = dp(12);
                            addView(diagnostic, diagnosticLayout);
                            LinearLayout media = column();
                            media.setVisibility(GONE);
                            mediaCards.put(id, media);
                            LayoutParams mediaLayout = matchWrap();
                            mediaLayout.bottomMargin = dp(12);
                            addView(media, mediaLayout);
                        }
                    }
                    status = text("", 11, MaiChatTheme.SECONDARY);
                    LinearLayout footer = row();
                    footer.setGravity(Gravity.CENTER_VERTICAL);
                    footer.addView(status, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
                    TextView quoteAction = text("↩  引用", 11, MaiChatTheme.SECONDARY);
                    quoteAction.setContentDescription("引用这条消息");
                    quoteAction.setOnClickListener(view -> beginQuote(value));
                    footer.addView(quoteAction, new LayoutParams(
                        LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT));
                    addView(footer, matchWrap());
                    status.setOnClickListener(v -> {
                        StringBuilder text = new StringBuilder();
                        JSONArray a = message.optJSONArray("parts");
                        if (a != null)
                            for (int i = 0; i < a.length(); i++) {
                                JSONObject p = a.optJSONObject(i);
                                if (p.optString("kind").equals("text"))
                                    text.append(p.optString("text"));
                            }
                        ((ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE))
                            .setPrimaryClip(ClipData.newPlainText("AI 回复", text));
                    });
                }
                addInlineMediaCards(inlineMedia);
            }
            if (toolSummary != null) {
                String title = toolGroupTitle(array);
                toolSummary.setText("⌘  " + title + (expanded.contains(toolGroupId) ? "  ⌄" : "  ›"));
                toolSummary.setContentDescription(title);
            }
            if (outgoing) {
                StringBuilder body = new StringBuilder();
                for (int i = 0; i < array.length(); i++) {
                    JSONObject part = array.optJSONObject(i);
                    if (part.optString("kind").equals("text")) body.append(part.optString("text"));
                    if (part.optString("kind").equals("image")) {
                        ImageView image = imageParts.get(part.optString("id"));
                        File file = controller.workspaceFile(part.optString("path"));
                        if (image != null && file != null) {
                            MessageImageLoader.load(file.getPath(), dp(220), dp(150), image,
                                () -> image.setContentDescription("图片无法显示"));
                            image.setOnClickListener(view -> {
                                if (activity instanceof MainActivity)
                                    ((MainActivity) activity).openAgentMedia(
                                        file.getAbsolutePath(), false, mediaPlaylist());
                            });
                        }
                    }
                    if (part.optString("kind").equals("video")) {
                        LinearLayout card = videoParts.get(part.optString("id"));
                        File file = controller.workspaceFile(part.optString("path"));
                        if (card != null) {
                            boolean playable = file != null && file.isFile();
                            if (playable && !file.getPath().equals(card.getTag())) {
                                card.setTag(file.getPath());
                                showAgentMediaCard(card, file, "video", "");
                            } else if (!playable && !"missing".equals(card.getTag())) {
                                card.setTag("missing");
                                card.removeAllViews();
                                card.addView(text("视频文件已丢失", 13, MaiChatTheme.SECONDARY), matchWrap());
                            }
                        }
                    }
                }
                TextView userText = parts.get("user");
                String visible = mediaDisplayText(body.toString(),
                    !imageParts.isEmpty() || !videoParts.isEmpty());
                userText.setText(visible);
                userText.setVisibility(visible.isEmpty() ? GONE : VISIBLE);
            } else
                for (int i = 0; i < array.length(); i++) {
                    JSONObject part = array.optJSONObject(i);
                    String id = part.optString("id"), kind = part.optString("kind");
                    TextView view = parts.get(id);
                    if (view == null)
                        continue;
                    if (kind.equals("text")) {
                        String source = part.optString("text");
                        view.setTextColor(MaiChatTheme.TEXT);
                        view.setAlpha(1f);
                        view.setVisibility(source.isEmpty() ? GONE : VISIBLE);
                        if (!source.equals(view.getContentDescription())) {
                            view.setContentDescription(source);
                            MarkdownRenderer.bindStreaming(view, source);
                        }
                    } else {
                        String stage = seedanceToolStage(part);
                        view.setText(stage == null ? toolStatus(part.optString("state")) + " "
                            + part.optString("tool") + "  ›" : stage + "  ›");
                        TextView detail = parts.get(id + ":detail");
                        if (detail != null)
                            detail.setText(part.optString("input") + "\n"
                                + part.optString("output") + "\n" + part.optString("error"));
                        Button pdfCard = pdfCards.get(id);
                        if (pdfCard != null) {
                            String path = pdfPathFromToolOutput(part);
                            pdfCard.setVisibility(path == null ? GONE : VISIBLE);
                            if (path != null) {
                                pdfCard.setTag(path);
                                pdfCard.setText("PDF  " + new File(path).getName() + "  ·  预览");
                            }
                        }
                        Button diagnosticCard = diagnosticCards.get(id);
                        if (diagnosticCard != null) {
                            String path = diagnosticPathFromToolOutput(part);
                            File report = path == null ? null : controller.workspaceFile(path);
                            boolean available = report != null && report.isFile()
                                && report.length() > 0 && report.length() <= 20L * 1024 * 1024;
                            diagnosticCard.setVisibility(available ? VISIBLE : GONE);
                            if (available) {
                                diagnosticCard.setTag(path);
                                diagnosticCard.setText("排障日志  " + report.getName() + "  ·  分享");
                            }
                        }
                        LinearLayout mediaCard = mediaCards.get(id);
                        if (mediaCard != null) {
                            JSONObject artifact = mediaFromToolOutput(part);
                            String path = artifact == null ? "" : artifact.optString("path");
                            String type = artifact == null ? "" : artifact.optString("type");
                            File file = path.isEmpty() ? null : controller.workspaceFile(path);
                            boolean available = file != null && file.isFile();
                            mediaCard.setVisibility(available ? VISIBLE : GONE);
                            if (available && !file.getPath().equals(mediaCard.getTag())) {
                                mediaCard.setTag(file.getPath());
                                showAgentMediaCard(mediaCard, file, type,
                                    artifact.optString("caption"));
                            }
                            if (available && part.optString("tool").equals("agent_send_media")) {
                                view.setVisibility(GONE);
                                if (detail != null) detail.setVisibility(GONE);
                            }
                        }
                    }
                }
            updateTime();
        }
        @Override
        protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec);
            if (message == null) {
                int viewport = list.getHeight() - list.getPaddingTop() - list.getPaddingBottom();
                if (viewport > getMeasuredHeight())
                    setMeasuredDimension(getMeasuredWidth(), viewport);
            }
        }
        private void addInlineMediaCards(JSONArray items) {
            if (items == null) return;
            for (int i = 0; i < items.length(); i++) {
                JSONObject item = items.optJSONObject(i);
                if (item == null) continue;
                String path = item.optString("path");
                String kind = item.optString("kind");
                if (path.isEmpty()) continue;
                File file = new File(path);
                if (kind.equals("audio")) {
                    LinearLayout audio = column();
                    audio.setTag(path);
                    showAgentMediaCard(audio, file, "audio", "");
                    LayoutParams layout = matchWrap();
                    layout.bottomMargin = dp(8);
                    addView(audio, layout);
                } else if (kind.equals("video")) {
                    LinearLayout video = column();
                    video.setTag(file.getPath());
                    showAgentMediaCard(video, file, "video", "");
                    LayoutParams layout = matchWrap();
                    layout.bottomMargin = dp(8);
                    addView(video, layout);
                }
            }
        }
        void updateTime() {
            if (status == null || message == null)
                return;
            long done = message.optLong("completed"), start = message.optLong("created");
            status.setText(message.optBoolean("active")
                    ? "正在思考 · " + Math.max(0, (System.currentTimeMillis() - start) / 1000) + " 秒"
                    : done == 0 ? "已中断 · 复制"
                                : "用时 " + Math.max(0, (done - start) / 1000) + " 秒 · 复制");
        }
    }
    private String toolStatus(String state) {
        return state.equals("completed") ? "已完成"
            : state.equals("canceled")   ? "已取消"
            : state.equals("error")      ? "失败"
            : state.equals("pending")    ? "等待授权"
                                         : "正在运行";
    }
    static String toolGroupTitle(JSONArray parts) {
        if (parts == null) return "";
        int count = 0;
        boolean running = false, pending = false, failed = false;
        boolean canceled = false, allCanceled = true;
        Map<String, Integer> names = new LinkedHashMap<>();
        for (int i = 0; i < parts.length(); i++) {
            JSONObject part = parts.optJSONObject(i);
            if (part == null || !part.optString("kind").equals("tool") ||
                mediaFromToolOutput(part) != null) continue;
            count++;
            String name = part.optString("tool", "工具");
            names.put(name, names.getOrDefault(name, 0) + 1);
            String state = part.optString("state");
            running |= state.equals("running");
            pending |= state.equals("pending");
            failed |= state.equals("error");
            canceled |= state.equals("canceled");
            allCanceled &= state.equals("canceled");
        }
        if (count == 0) return "";
        String status = running ? "运行中" : pending ? "等待授权" : failed ? "有失败"
            : allCanceled ? "已取消" : canceled ? "部分取消" : "已完成";
        StringBuilder title = new StringBuilder("工具调用 ").append(count).append(" 次，").append(status);
        for (Map.Entry<String, Integer> name : names.entrySet())
            title.append(" · ").append(name.getKey()).append(" ×").append(name.getValue());
        return title.toString();
    }
    private String seedanceToolStage(JSONObject part) {
        if (!part.optString("tool").equals("seedance_video")) return null;
        JSONObject input;
        JSONObject output;
        try { input = new JSONObject(part.optString("input", "{}")); }
        catch (Exception ignored) { input = new JSONObject(); }
        try { output = new JSONObject(part.optString("output", "{}")); }
        catch (Exception ignored) { output = new JSONObject(); }
        if (part.optString("state").equals("error")) return "人物替换 · 当前阶段失败";
        String action = input.optString("action");
        if (action.equals("prepare")) {
            if (part.optString("state").equals("running"))
                return "人物替换 · 正在准备素材";
            if (output.optString("status").equals("processing"))
                return "人物替换 · 素材处理中";
            if (output.optString("status").equals("ready"))
                return "人物替换 · 素材已 Active";
        }
        if (action.equals("delegate") || action.equals("revise")) {
            if (part.optString("state").equals("pending"))
                return "人物替换 · 等待付费确认";
            String status = output.optString("status");
            if (status.equals("queued")) return "人物替换 · 云端排队中";
            if (status.equals("running")) return "人物替换 · 云端生成中";
            if (status.equals("succeeded")) return "人物替换 · 已完成";
            return "人物替换 · 正在提交";
        }
        if (action.equals("continue")) {
            String status = output.optString("status");
            if (status.equals("queued")) return "人物替换 · 云端排队中";
            if (status.equals("running")) return "人物替换 · 云端生成中";
            if (status.equals("succeeded")) return "人物替换 · 已完成";
        }
        return null;
    }
    static String pdfPathFromToolOutput(JSONObject part) {
        if (!part.optString("tool").equals("generate_pdf")
            || !part.optString("state").equals("completed")) return null;
        String output = part.optString("output");
        try {
            JSONObject artifact = new JSONObject(output);
            String path = artifact.optString("path");
            if (artifact.optString("mime_type").equals("application/pdf")
                && path.toLowerCase(Locale.ROOT).endsWith(".pdf")) return path;
        } catch (org.json.JSONException ignored) { }
        int end = output.lastIndexOf(" (");
        if (output.startsWith("Created ") && output.endsWith(" bytes).") && end > 8) {
            String path = output.substring(8, end);
            if (path.toLowerCase(Locale.ROOT).endsWith(".pdf")) return path;
        }
        return null;
    }
    static String diagnosticPathFromToolOutput(JSONObject part) {
        if (!part.optString("tool").equals("mobile_export_diagnostics")
            || !part.optString("state").equals("completed")) return null;
        try {
            JSONObject artifact = new JSONObject(part.optString("output"));
            String path = artifact.optString("path");
            if (artifact.optString("mime_type").equals("text/plain")
                && path.equals(new File(path).getName())
                && path.startsWith("maichat-diagnostics-")
                && path.toLowerCase(Locale.ROOT).endsWith(".txt")
                && artifact.optLong("bytes") > 0
                && artifact.optLong("bytes") <= 20L * 1024 * 1024) return path;
        } catch (org.json.JSONException ignored) { }
        return null;
    }
    private void shareDiagnosticFile(File file) {
        Uri uri = FileProvider.getUriForFile(activity,
            activity.getPackageName() + ".files", file);
        Intent share = new Intent(Intent.ACTION_SEND);
        share.setType("text/plain");
        share.putExtra(Intent.EXTRA_STREAM, uri);
        share.setClipData(ClipData.newUri(activity.getContentResolver(),
            "MaiChat diagnostic log", uri));
        share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        activity.startActivity(Intent.createChooser(share, "分享 MaiChat 排障日志"));
    }
    static JSONObject mediaFromToolOutput(JSONObject part) {
        if (!part.optString("tool").equals("agent_send_media")
            || !part.optString("state").equals("completed")) return null;
        try {
            JSONObject artifact = new JSONObject(part.optString("output"));
            String type = artifact.optString("type");
            if (artifact.optString("delivery").equals("current_ai_session")
                && !artifact.optString("path").isEmpty()
                && (type.equals("image") || type.equals("video") || type.equals("audio"))
                && artifact.optString("mime_type").startsWith(type + "/")) return artifact;
        } catch (org.json.JSONException ignored) { }
        return null;
    }
    private List<MainActivity.GalleryItem> mediaPlaylist() {
        List<MainActivity.GalleryItem> result = new ArrayList<>();
        if (state == null) return result;
        Set<String> seen = new HashSet<>();
        JSONArray all = state.data.optJSONArray("messages");
        if (all == null) return result;
        // 与会话里的气泡顺序一致：用户附件、正文媒体、Agent 交付的媒体。
        for (int i = 0; i < all.length(); i++) {
            JSONObject message = all.optJSONObject(i);
            if (message == null) continue;
            JSONArray parts = message.optJSONArray("parts");
            boolean outgoing = message.optString("role").equals("user");
            if (outgoing && parts != null) {
                for (int j = 0; j < parts.length(); j++) {
                    JSONObject part = parts.optJSONObject(j);
                    if (part != null && (part.optString("kind").equals("image")
                        || part.optString("kind").equals("video")))
                        addPlayableMedia(result, seen, part.optString("path"),
                            part.optString("kind").equals("video"));
                }
            }
            JSONArray inline = message.optJSONArray("inline_media");
            if (inline != null) {
                for (int j = 0; j < inline.length(); j++) {
                    JSONObject item = inline.optJSONObject(j);
                    if (item != null && (item.optString("kind").equals("image")
                        || item.optString("kind").equals("video")))
                        addPlayableMedia(result, seen, item.optString("path"),
                            item.optString("kind").equals("video"));
                }
            }
            if (!outgoing && parts != null) {
                for (int j = 0; j < parts.length(); j++) {
                    JSONObject part = parts.optJSONObject(j);
                    if (part == null) continue;
                    JSONObject artifact = mediaFromToolOutput(part);
                    if (artifact != null && (artifact.optString("type").equals("image")
                        || artifact.optString("type").equals("video")))
                        addPlayableMedia(result, seen, artifact.optString("path"),
                            artifact.optString("type").equals("video"));
                }
            }
        }
        return result;
    }

    private void addPlayableMedia(List<MainActivity.GalleryItem> result, Set<String> seen,
                                  String path, boolean video) {
        File file = controller.workspaceFile(path);
        if (file != null && file.isFile()
            && seen.add(video + "\0" + file.getAbsolutePath()))
            result.add(new MainActivity.GalleryItem(file.getAbsolutePath(), video));
    }

    static int[] videoCardSize(int width, int height) {
        if (width <= 0 || height <= 0) return new int[] {280, 181};
        double scale = Math.min(280.0 / width, 320.0 / height);
        return new int[] {Math.max(1, (int) Math.round(width * scale)),
            Math.max(1, (int) Math.round(height * scale))};
    }

    // 根据文件类型在当前消息位置展示可点击的媒体卡片和实际缩略图。
    // 预览资源就绪后更新该卡片，不用文件名文本代替视频或图片内容。
    private void showAgentMediaCard(LinearLayout container, File file, String type,
                                    String caption) {
        container.removeAllViews();
        if (type.equals("image") || type.equals("video")) {
            boolean video = type.equals("video");
            int[] initialSize = video ? videoCardSize(0, 0) : new int[] {220, 240};
            FrameLayout preview = new FrameLayout(activity);
            if (video) {
                preview.setContentDescription("播放视频 " + file.getName());
                android.graphics.drawable.GradientDrawable background =
                    new android.graphics.drawable.GradientDrawable(
                        android.graphics.drawable.GradientDrawable.Orientation.TL_BR,
                        new int[] {Color.rgb(26, 43, 69), Color.rgb(46, 82, 120)});
                background.setCornerRadius(dp(10));
                preview.setBackground(background);
                preview.setClipToOutline(true);
            }
            ImageView cover = new ImageView(activity);
            cover.setScaleType(ImageView.ScaleType.FIT_CENTER);
            preview.addView(cover, new FrameLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
            MessageImageLoader.load(file.getAbsolutePath(),
                video ? dp(280) : dp(220), video ? dp(320) : dp(240), cover,
                () -> cover.setContentDescription("媒体无法显示"));
            if (video) {
                TextView play = text("▶", 20, Color.WHITE);
                play.setGravity(Gravity.CENTER);
                play.setBackground(MaiChatTheme.rounded(Color.argb(190, 0, 0, 0), 26, activity));
                FrameLayout.LayoutParams playLayout = new FrameLayout.LayoutParams(dp(52), dp(52));
                playLayout.gravity = Gravity.CENTER;
                preview.addView(play, playLayout);
                TextView name = text(file.getName(), 12, Color.WHITE);
                name.setSingleLine(true);
                name.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
                name.setTypeface(MaiChatTypography.semibold());
                name.setPadding(dp(7), dp(4), dp(7), dp(4));
                name.setBackground(MaiChatTheme.rounded(Color.argb(150, 0, 0, 0), 6, activity));
                FrameLayout.LayoutParams nameLayout = new FrameLayout.LayoutParams(
                    LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.LEFT | Gravity.TOP);
                nameLayout.setMargins(dp(8), dp(8), dp(8), 0);
                preview.addView(name, nameLayout);
                name.setMaxWidth(dp(Math.max(1, initialSize[0] - 16)));
                TextView duration = text("0:00", 12, Color.WHITE);
                duration.setPadding(dp(5), dp(2), dp(5), dp(2));
                duration.setBackground(MaiChatTheme.rounded(Color.argb(180, 0, 0, 0), 8, activity));
                FrameLayout.LayoutParams durationLayout =
                    new FrameLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
                durationLayout.gravity = Gravity.RIGHT | Gravity.BOTTOM;
                durationLayout.setMargins(0, 0, dp(8), dp(8));
                preview.addView(duration, durationLayout);
                new Thread(() -> {
                    MediaMetadataRetriever metadata = new MediaMetadataRetriever();
                    try {
                        metadata.setDataSource(file.getAbsolutePath());
                        long ms = Long.parseLong(metadata.extractMetadata(
                            MediaMetadataRetriever.METADATA_KEY_DURATION));
                        String widthText = metadata.extractMetadata(
                            MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH);
                        String heightText = metadata.extractMetadata(
                            MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT);
                        String rotationText = metadata.extractMetadata(
                            MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION);
                        int width = widthText == null ? 0 : Integer.parseInt(widthText);
                        int height = heightText == null ? 0 : Integer.parseInt(heightText);
                        int rotation = rotationText == null ? 0 : Integer.parseInt(rotationText);
                        if (rotation == 90 || rotation == 270) {
                            int swap = width; width = height; height = swap;
                        }
                        int[] size = videoCardSize(width, height);
                        activity.runOnUiThread(() -> {
                            if (file.getPath().equals(container.getTag())) {
                                duration.setText(String.format(Locale.ROOT, "%d:%02d",
                                    ms / 60000, (ms / 1000) % 60));
                                preview.setLayoutParams(new LayoutParams(dp(size[0]), dp(size[1])));
                                name.setMaxWidth(dp(Math.max(1, size[0] - 16)));
                            }
                        });
                    } catch (Exception ignored) { }
                    finally { try { metadata.release(); } catch (Exception ignored) { } }
                }, "agent-video-metadata").start();
                preview.setOnClickListener(view -> {
                    if (!file.isFile()) {
                        Toast.makeText(activity, "文件已被移动或删除", Toast.LENGTH_SHORT).show();
                        return;
                    }
                    if (activity instanceof MainActivity)
                        ((MainActivity) activity).openAgentMedia(
                            file.getAbsolutePath(), true, mediaPlaylist());
                });
            } else {
                preview.setOnClickListener(view -> {
                    if (activity instanceof MainActivity)
                        ((MainActivity) activity).openAgentMedia(
                            file.getAbsolutePath(), false, mediaPlaylist());
                });
            }
            container.addView(preview, new LayoutParams(dp(initialSize[0]), dp(initialSize[1])));
            preview.setOnLongClickListener(view -> {
                showAgentMediaMenu(view, file, type); return true;
            });
        } else if (type.equals("audio")) {
            LinearLayout audio = row();
            audio.setGravity(Gravity.CENTER_VERTICAL);
            audio.setPadding(dp(10), dp(10), dp(10), dp(10));
            audio.setBackground(MaiChatTheme.rounded(Color.rgb(244, 246, 249), 12, activity));
            audio.setContentDescription("播放音频");
            ImageButton play = new ImageButton(activity);
            play.setImageResource(android.R.drawable.ic_media_play);
            play.setImageTintList(android.content.res.ColorStateList.valueOf(MaiChatTheme.BLUE));
            play.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 19, activity));
            play.setPadding(dp(10), dp(10), dp(10), dp(10));
            audio.addView(play, new LayoutParams(dp(38), dp(38)));
            LinearLayout labels = column();
            labels.setPadding(dp(12), 0, 0, 0);
            TextView name = text(file.getName(), 14, MaiChatTheme.TEXT);
            name.setSingleLine(true);
            name.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
            labels.addView(name, matchWrap());
            TextView duration = text("0:00", 11, MaiChatTheme.SECONDARY);
            labels.addView(duration, matchWrap());
            audio.addView(labels, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
            new Thread(() -> {
                MediaMetadataRetriever metadata = new MediaMetadataRetriever();
                try {
                    metadata.setDataSource(file.getAbsolutePath());
                    String value = metadata.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION);
                    long ms = value == null ? 0 : Long.parseLong(value);
                    activity.runOnUiThread(() -> {
                        if (audio.getParent() == container)
                            duration.setText(String.format(Locale.ROOT, "%d:%02d",
                                ms / 60000, (ms / 1000) % 60));
                    });
                } catch (Exception ignored) { }
                finally { try { metadata.release(); } catch (Exception ignored) { } }
            }, "agent-audio-metadata").start();
            final MediaPlayer[] playback = new MediaPlayer[1];
            final boolean[] ready = {false};
            audio.setOnClickListener(view -> {
                try {
                    if (playback[0] == null) {
                        MediaPlayer player = new MediaPlayer();
                        playback[0] = player;
                        player.setDataSource(file.getAbsolutePath());
                        player.setOnPreparedListener(prepared -> {
                            ready[0] = true;
                            prepared.start();
                            play.setImageResource(android.R.drawable.ic_media_pause);
                            audio.setContentDescription("暂停音频");
                        });
                        player.setOnCompletionListener(done -> {
                            play.setImageResource(android.R.drawable.ic_media_play);
                            audio.setContentDescription("播放音频");
                            done.seekTo(0);
                        });
                        player.prepareAsync();
                    } else if (!ready[0]) {
                        return;
                    } else if (playback[0].isPlaying()) {
                        playback[0].pause();
                        play.setImageResource(android.R.drawable.ic_media_play);
                        audio.setContentDescription("播放音频");
                    } else {
                        playback[0].start();
                        play.setImageResource(android.R.drawable.ic_media_pause);
                        audio.setContentDescription("暂停音频");
                    }
                } catch (Exception failure) {
                    duration.setText("音频无法播放");
                }
            });
            play.setOnClickListener(view -> audio.performClick());
            audio.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
                @Override public void onViewAttachedToWindow(View view) { }
                @Override public void onViewDetachedFromWindow(View view) {
                    if (playback[0] != null) {
                        playback[0].release(); playback[0] = null; ready[0] = false;
                    }
                }
            });
            audio.setOnLongClickListener(view -> {
                showAgentMediaMenu(view, file, type); return true;
            });
            container.addView(audio, new LayoutParams(dp(240), LayoutParams.WRAP_CONTENT));
        }
        if (!caption.isEmpty()) {
            TextView description = MaiChatTypography.body(activity);
            description.setText(caption);
            container.addView(description, matchWrap());
        }
    }
    private void showAgentMediaMenu(View anchor, File file, String type) {
        PopupMenu menu = new PopupMenu(activity, anchor);
        if (!type.equals("audio")) menu.getMenu().add("保存到相册");
        menu.getMenu().add("转发或分享");
        menu.setOnMenuItemClickListener(item -> {
            if (item.getTitle().toString().equals("保存到相册")) saveAgentMedia(file, type);
            else shareAgentMedia(file, type);
            return true;
        });
        menu.show();
    }
    private void saveAgentMedia(File file, String type) {
        if (Build.VERSION.SDK_INT < 29) {
            Toast.makeText(activity, "请使用分享功能保存媒体", Toast.LENGTH_SHORT).show();
            return;
        }
        new Thread(() -> {
            Uri target = null;
            try {
                String mime = URLConnection.guessContentTypeFromName(file.getName());
                if (mime == null) mime = type + "/octet-stream";
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, file.getName());
                values.put(MediaStore.MediaColumns.MIME_TYPE, mime);
                values.put(MediaStore.MediaColumns.RELATIVE_PATH,
                    (type.equals("image") ? Environment.DIRECTORY_PICTURES
                                          : Environment.DIRECTORY_MOVIES) + "/MaiChat");
                values.put(MediaStore.MediaColumns.IS_PENDING, 1);
                target = activity.getContentResolver().insert(type.equals("image")
                    ? MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
                    : MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values);
                if (target == null) throw new IllegalStateException("Cannot create gallery entry");
                try (InputStream input = new FileInputStream(file);
                     OutputStream output = activity.getContentResolver().openOutputStream(target)) {
                    if (output == null) throw new IllegalStateException("Cannot write gallery entry");
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                }
                values.clear(); values.put(MediaStore.MediaColumns.IS_PENDING, 0);
                activity.getContentResolver().update(target, values, null, null);
                activity.runOnUiThread(() -> Toast.makeText(activity, "已保存到相册",
                    Toast.LENGTH_SHORT).show());
            } catch (Exception failure) {
                if (target != null) activity.getContentResolver().delete(target, null, null);
                activity.runOnUiThread(() -> Toast.makeText(activity, "保存失败",
                    Toast.LENGTH_SHORT).show());
            }
        }, "agent-media-save").start();
    }
    private void shareAgentMedia(File file, String type) {
        new Thread(() -> {
            try {
                File shared = new File(activity.getCacheDir(),
                    UUID.randomUUID() + "-" + file.getName());
                Files.copy(file.toPath(), shared.toPath(), StandardCopyOption.REPLACE_EXISTING);
                Uri uri = FileProvider.getUriForFile(activity,
                    activity.getPackageName() + ".files", shared);
                String mime = URLConnection.guessContentTypeFromName(file.getName());
                Intent intent = new Intent(Intent.ACTION_SEND);
                intent.setType(mime == null ? type + "/*" : mime);
                intent.putExtra(Intent.EXTRA_STREAM, uri);
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                activity.runOnUiThread(() -> activity.startActivity(
                    Intent.createChooser(intent, "转发媒体")));
            } catch (Exception failure) {
                activity.runOnUiThread(() -> Toast.makeText(activity, "无法分享媒体",
                    Toast.LENGTH_SHORT).show());
            }
        }, "agent-media-share").start();
    }
    private int dp(int value) {
        return MaiChatTheme.dp(activity, value);
    }
    private ImageButton headerIcon(int drawable, String description, OnClickListener action) {
        ImageButton button = new ImageButton(activity);
        button.setImageResource(drawable);
        button.setColorFilter(MaiChatTheme.TEXT);
        button.setScaleType(ImageView.ScaleType.CENTER);
        button.setPadding(dp(8), dp(8), dp(8), dp(8));
        button.setBackground(MaiChatTheme.rounded(Color.rgb(246, 247, 250), 10, activity));
        button.setContentDescription(description);
        button.setOnClickListener(action);
        return button;
    }
    private ImageButton circularIcon(int drawable, String description, boolean blue,
                                     OnClickListener action) {
        ImageButton button = new ImageButton(activity);
        button.setImageResource(drawable);
        button.setColorFilter(blue ? MaiChatTheme.BLUE : MaiChatTheme.TEXT);
        button.setScaleType(ImageView.ScaleType.CENTER);
        button.setPadding(dp(10), dp(10), dp(10), dp(10));
        button.setBackground(blue
            ? MaiChatTheme.bordered(MaiChatTheme.BLUE_SOFT, MaiChatTheme.BORDER, 22, activity)
            : MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 22, activity));
        button.setContentDescription(description);
        button.setOnClickListener(action);
        return button;
    }
    private View attachmentAction(int drawable, String title, String description,
                                  OnClickListener action) {
        LinearLayout item = column();
        item.setGravity(Gravity.CENTER);
        item.setContentDescription(description);
        item.setBackground(MaiChatTheme.rounded(Color.rgb(247, 249, 252), 12, activity));
        item.setOnClickListener(action);
        item.setClickable(true);
        item.setFocusable(true);
        ImageView icon = new ImageView(activity);
        icon.setImageResource(drawable);
        item.addView(icon, new LayoutParams(dp(24), dp(24)));
        TextView label = text(title, 12, MaiChatTheme.TEXT);
        label.setGravity(Gravity.CENTER);
        LayoutParams labelParams = matchWrap();
        labelParams.setMargins(0, dp(5), 0, 0);
        item.addView(label, labelParams);
        return item;
    }
    private View sessionDrawerRow(String title, String detail, boolean selected,
                                  OnClickListener action) {
        LinearLayout row = column();
        row.setPadding(dp(14), dp(10), dp(14), dp(10));
        row.setBackground(MaiChatTheme.rounded(
            selected ? MaiChatTheme.BLUE_SOFT : Color.TRANSPARENT, 10, activity));
        row.setOnClickListener(action);
        row.setClickable(true);
        row.setFocusable(true);
        TextView heading = text(title, 15, MaiChatTheme.TEXT);
        heading.setTypeface(selected ? MaiChatTypography.semibold() : android.graphics.Typeface.SANS_SERIF);
        heading.setSingleLine(true);
        heading.setEllipsize(android.text.TextUtils.TruncateAt.END);
        row.addView(heading, matchWrap());
        if (!detail.isEmpty()) {
            TextView status = text(detail, 12, MaiChatTheme.BLUE);
            LayoutParams statusParams = matchWrap();
            statusParams.setMargins(0, dp(3), 0, 0);
            row.addView(status, statusParams);
        }
        LayoutParams params = matchWrap();
        params.setMargins(0, dp(2), 0, dp(2));
        row.setLayoutParams(params);
        return row;
    }
    private View actionPanelRow(String title, String value, int titleColor,
                                OnClickListener action) {
        LinearLayout row = column();
        row.setPadding(dp(14), dp(9), dp(14), dp(9));
        row.setMinimumHeight(dp(52));
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setOnClickListener(action);
        row.setClickable(true);
        row.setFocusable(true);
        TextView heading = text(title, 14, titleColor);
        heading.setTypeface(MaiChatTypography.semibold());
        row.addView(heading, matchWrap());
        if (!value.isEmpty()) {
            TextView detail = text(value, 11, MaiChatTheme.SECONDARY);
            detail.setSingleLine(true);
            detail.setEllipsize(android.text.TextUtils.TruncateAt.END);
            row.addView(detail, matchWrap());
        }
        return row;
    }
    private View actionPanelDivider() {
        View divider = new View(activity);
        divider.setBackgroundColor(Color.rgb(239, 242, 246));
        return divider;
    }
    private TextView text(String source, int size, int color) {
        return MaiChatTheme.text(activity, source, size, color);
    }
    private Button button(String title, String description, OnClickListener action) {
        Button button = new Button(activity);
        button.setAllCaps(false);
        button.setText(title);
        button.setTextSize(13);
        button.setTextColor(MaiChatTheme.BLUE);
        button.setPadding(dp(3), 0, dp(3), 0);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setContentDescription(description);
        button.setOnClickListener(action);
        return button;
    }
    private LinearLayout row() {
        LinearLayout view = new LinearLayout(activity);
        view.setGravity(Gravity.CENTER_VERTICAL);
        return view;
    }
    private LinearLayout column() {
        LinearLayout view = new LinearLayout(activity);
        view.setOrientation(VERTICAL);
        return view;
    }
    private LayoutParams matchWrap() {
        return new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
    }
}
