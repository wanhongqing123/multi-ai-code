package com.kongshang.maichat;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
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
import java.util.*;
import org.json.JSONArray;
import org.json.JSONObject;

/** Stable composer + recycled message rows. No native calls, disk I/O or parsing on UI. */
final class AIAssistantPanel extends LinearLayout implements AIAssistantController.Listener {
    static final int REQUEST_FILE = 7107;
    static final int REQUEST_IMAGE = 7108;
    static final int REQUEST_CAMERA = 7109;
    static final int REQUEST_CAMERA_PERMISSION = 7110;
    private final Activity activity;
    final AIAssistantController controller;
    final EditText composer;
    final Button send;
    private final Button modelButton, policyButton;
    private final TextView error;
    private final LatestMessageList list;
    private final LinearLayout pending;
    private final LinearLayout attachmentTray;
    private final MessageAdapter adapter = new MessageAdapter();
    private AIAssistantController.State state;
    private String selected = "", pendingSignature = "";
    private final Map<String, String> drafts = new HashMap<>();
    private final Map<String, List<AIAssistantController.ImportedFile>> attachments = new HashMap<>();
    private final Set<String> expanded = new HashSet<>();
    private List<JSONObject> messages = Collections.emptyList();
    private boolean following = true, submitting;
    private int visibleLimit = 50;
    private final Button older;
    private boolean visibleToUser = true;
    private File pendingCameraFile;
    private final VoiceRecordingController voiceRecorder;
    private final SpeechRecognizer speechRecognizer;
    private boolean voiceRecording, cancelVoice;
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
        LinearLayout header = row();
        header.addView(button("☰", "对话列表", v -> sessions()), new LayoutParams(dp(48), dp(48)));
        TextView title = text("AI 助手", 18, MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        header.addView(title, new LayoutParams(0, dp(48), 1));
        header.addView(button("＋", "新对话", v -> {
            following = true;
            controller.create();
        }), new LayoutParams(dp(46), dp(48)));
        header.addView(button("⋯", "对话选项", v -> menu()), new LayoutParams(dp(46), dp(48)));
        addView(header, matchWrap());
        list = new LatestMessageList();
        list.setDivider(null);
        list.setDividerHeight(dp(16));
        list.setPadding(dp(16), dp(12), dp(16), dp(12));
        list.setClipToPadding(false);
        older = button("显示更早消息", "显示更早消息", v -> {
            visibleLimit += 50;
            updateMessages();
        });
        list.addHeaderView(older, null, false);
        list.setAdapter(adapter);
        list.setOnScrollListener(new AbsListView.OnScrollListener() {
            @Override
            public void onScrollStateChanged(AbsListView view, int scrollState) {
                if (scrollState != SCROLL_STATE_IDLE)
                    following = false;
                else if (atBottom())
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
        LinearLayout box = column();
        box.setPadding(dp(12), dp(6), dp(12), dp(6));
        box.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 18, activity));
        composer = new EditText(activity);
        composer.setTag("ai-composer");
        composer.setContentDescription("AI 消息输入框");
        composer.setHint("随心输入");
        composer.setTextSize(14);
        composer.setMinLines(1);
        composer.setMaxLines(6);
        composer.setBackgroundColor(Color.TRANSPARENT);
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
                following = true;
                locateLatest();
            }
        });
        box.addView(composer, matchWrap());
        attachmentTray = column();
        box.addView(attachmentTray, matchWrap());
        LinearLayout actions = row();
        actions.addView(button("＋", "添加图片或文件", v -> attachmentMenu()),
            new LayoutParams(dp(36), dp(40)));
        Button voice = button("◉", "按住语音转文字", null);
        voice.setOnTouchListener((view, event) -> voiceTouch(event));
        actions.addView(voice, new LayoutParams(dp(36), dp(40)));
        policyButton = button("请求批准", "权限设置", v -> settings());
        policyButton.setTextSize(11);
        actions.addView(policyButton, new LayoutParams(LayoutParams.WRAP_CONTENT, dp(40)));
        modelButton = button("未配置模型", "切换模型", v -> modelMenu());
        modelButton.setTextSize(11);
        modelButton.setSingleLine(true);
        actions.addView(modelButton, new LayoutParams(0, dp(40), 1));
        send = button("↑", "发送", v -> send());
        send.setTextColor(Color.WHITE);
        send.setBackground(MaiChatTheme.rounded(Color.rgb(29, 32, 36), 18, activity));
        actions.addView(send, new LayoutParams(dp(36), dp(36)));
        box.addView(actions, matchWrap());
        LayoutParams bp = matchWrap();
        bp.setMargins(dp(12), dp(8), dp(12), dp(10));
        addView(box, bp);
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
            boolean firstSend = submitting && selected.isEmpty();
            selected = next.selected;
            if (firstSend && attachments.containsKey(previous))
                attachments.put(selected, attachments.remove(previous));
            if (!firstSend)
                composer.setText(drafts.getOrDefault(selected, ""));
            visibleLimit = 50;
            expanded.clear();
            following = true;
        }
        state = next;
        error.setText(next.error);
        error.setVisibility(next.error.isEmpty() ? GONE : VISIBLE);
        modelButton.setText(next.data.optBoolean("configured") ? next.model : "未配置模型");
        policyButton.setText(next.policy.equals("never") ? "完全访问"
                : next.policy.equals("unless-trusted")   ? "帮我批准"
                                                         : "请求批准");
        updateSend();
        updateAttachments();
        updateMessages();
        updatePending();
    }
    private void updateMessages() {
        if (state == null)
            return;
        JSONArray array = state.data.optJSONArray("messages");
        int count = array == null ? 0 : array.length();
        List<JSONObject> next = new ArrayList<>();
        for (int i = Math.max(0, count - visibleLimit); i < count; i++) next.add(array.optJSONObject(i));
        messages = next;
        older.setVisibility(count > visibleLimit ? VISIBLE : GONE);
        adapter.notifyDataSetChanged();
        if (following)
            locateLatest();
    }
    private boolean atBottom() {
        if (list.getCount() == 0)
            return true;
        View last = list.getChildAt(list.getChildCount() - 1);
        return list.getLastVisiblePosition() == list.getCount() - 1 && last != null
            && last.getBottom() <= list.getHeight() - list.getPaddingBottom() + dp(30);
    }
    private void locateLatest() {
        list.requestLayout();
    }
    private final class LatestMessageList extends ListView {
        LatestMessageList() {
            super(activity);
        }
        @Override
        protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            super.onLayout(changed, left, top, right, bottom);
            if (!following || getCount() == 0)
                return;
            int target = getCount() - 1;
            setSelectionFromTop(target, 0);
            layoutChildren();
            View last = getChildAt(target - getFirstVisiblePosition());
            if (last != null) {
                setSelectionFromTop(
                    target, getHeight() - getPaddingBottom() - getPaddingTop() - last.getHeight());
                layoutChildren();
            }
        }
    }
    private void updateSend() {
        if (send == null)
            return;
        boolean busy = state != null && state.busy();
        send.setText(busy ? "■" : "↑");
        send.setContentDescription(busy ? "停止" : "发送");
        send.setEnabled(state != null && state.ready && !submitting
            && (busy || !composer.getText().toString().trim().isEmpty()
                || !currentAttachments().isEmpty()));
        // Keep the native editor focused while submit runs; disabling it collapses the IME.
    }
    private void send() {
        if (state == null || submitting)
            return;
        if (state.busy()) {
            controller.action("stop", new JSONObject(), null);
            return;
        }
        if (!state.data.optBoolean("configured")) {
            settings();
            return;
        }
        String source = composer.getText().toString(), origin = selected;
        List<AIAssistantController.ImportedFile> sending = new ArrayList<>(currentAttachments());
        if (source.trim().isEmpty() && !sending.isEmpty())
            source = sending.size() == 1 ? "请查看这张图片。" : "请查看这些图片。";
        if (!sending.isEmpty() && !AIAssistantMediaPolicy.supportsImages(state.model)) {
            Toast.makeText(activity, "glm-5.3 仅支持文本，请先切换到 glm-5.3-flash。",
                Toast.LENGTH_LONG).show();
            return;
        }
        String submittedText = source;
        submitting = true;
        updateSend();
        following = true;
        controller.send(source, sending, success -> {
            submitting = false;
            if (success && (origin.isEmpty() || selected.equals(origin))
                && (composer.getText().toString().equals(submittedText)
                    || composer.getText().toString().trim().isEmpty()))
                composer.setText("");
            if (success) {
                attachments.remove(origin);
                if (origin.isEmpty()) attachments.remove(selected);
            }
            updateAttachments();
            updateSend();
            if (success)
                locateLatest();
        });
    }
    void importFile(Uri uri) {
        String origin = selected;
        controller.importFile(uri, file -> {
            if (file == null)
                return;
            acceptImportedFile(file, origin);
        });
    }
    private List<AIAssistantController.ImportedFile> currentAttachments() {
        return attachments.getOrDefault(selected, Collections.emptyList());
    }
    private void acceptImportedFile(AIAssistantController.ImportedFile file, String target) {
        if (!file.image) {
            String reference = "\n请查看工作区文件：" + file.relativePath + "\n";
            if (target.equals(selected)) composer.append(reference);
            else drafts.put(target, drafts.getOrDefault(target, "") + reference);
            return;
        }
        attachments.computeIfAbsent(target, ignored -> new ArrayList<>()).add(file);
        if (target.equals(selected)) {
            updateAttachments(); updateSend();
            if (state != null && AIAssistantMediaPolicy.supportsImages(state.model)) send();
            else Toast.makeText(activity, "图片已保留，请切换到 glm-5.3-flash 后发送。",
                Toast.LENGTH_LONG).show();
        }
    }
    private void updateAttachments() {
        if (attachmentTray == null) return;
        attachmentTray.removeAllViews();
        for (AIAssistantController.ImportedFile file : currentAttachments()) {
            LinearLayout chip = row();
            chip.setPadding(dp(8), dp(4), dp(8), dp(4));
            chip.setBackground(MaiChatTheme.bordered(MaiChatTheme.BLUE_SOFT,
                MaiChatTheme.BORDER, 9, activity));
            ImageView preview = new ImageView(activity);
            preview.setScaleType(ImageView.ScaleType.CENTER_CROP);
            File image = controller.workspaceFile(file.relativePath);
            if (image != null) MessageImageLoader.load(image.getPath(), dp(72), dp(54), preview, null);
            chip.addView(preview, new LayoutParams(dp(72), dp(54)));
            TextView name = text(file.relativePath, 12, MaiChatTheme.TEXT);
            name.setMaxLines(2);
            LayoutParams nameParams = new LayoutParams(0, dp(54), 1);
            nameParams.setMargins(dp(8), 0, dp(8), 0);
            chip.addView(name, nameParams);
            chip.addView(button("×", "移除图片", view -> {
                List<AIAssistantController.ImportedFile> values = attachments.get(selected);
                if (values != null) {
                    values.remove(file);
                    if (values.isEmpty()) attachments.remove(selected);
                }
                updateAttachments(); updateSend();
            }), new LayoutParams(dp(36), dp(54)));
            LayoutParams params = matchWrap();
            params.setMargins(0, dp(4), 0, dp(4));
            attachmentTray.addView(chip, params);
        }
    }
    private void attachmentMenu() {
        new AlertDialog.Builder(activity)
            .setItems(new String[] {"从相册选择", "拍照", "导入文本文件"}, (dialog, which) -> {
                if (which == 0) {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                        .setType("image/*").addCategory(Intent.CATEGORY_OPENABLE);
                    activity.startActivityForResult(intent, REQUEST_IMAGE);
                } else if (which == 1) requestCamera();
                else {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                        .setType("text/*").addCategory(Intent.CATEGORY_OPENABLE);
                    activity.startActivityForResult(intent, REQUEST_FILE);
                }
            })
            .show();
    }
    boolean handleActivityResult(int requestCode, Intent data) {
        if (requestCode == REQUEST_FILE) {
            if (data != null && data.getData() != null) importFile(data.getData());
            return true;
        }
        if (requestCode == REQUEST_IMAGE) {
            if (data != null && data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++)
                    importFile(data.getClipData().getItemAt(i).getUri());
            } else if (data != null && data.getData() != null) importFile(data.getData());
            return true;
        }
        if (requestCode == REQUEST_CAMERA) {
            File file = pendingCameraFile; pendingCameraFile = null;
            if (file != null) {
                String target = selected;
                controller.importCameraFile(file, imported -> {
                    if (imported != null) acceptImportedFile(imported, target);
                });
            }
            return true;
        }
        return false;
    }
    void handleActivityCancelled(int requestCode) {
        if (requestCode == REQUEST_CAMERA && pendingCameraFile != null) {
            pendingCameraFile.delete();
            pendingCameraFile = null;
        }
    }
    void onCameraPermission(boolean granted) {
        if (granted) openCamera();
        else Toast.makeText(activity, "没有相机权限，无法拍照", Toast.LENGTH_LONG).show();
    }
    void onAudioPermission(boolean granted) {
        Toast.makeText(activity, granted ? "麦克风已启用，请按住语音按钮"
            : "没有麦克风权限，无法语音转文字", Toast.LENGTH_LONG).show();
    }
    private void requestCamera() {
        if (activity.checkSelfPermission(Manifest.permission.CAMERA)
            == android.content.pm.PackageManager.PERMISSION_GRANTED) openCamera();
        else activity.requestPermissions(new String[] {Manifest.permission.CAMERA},
            REQUEST_CAMERA_PERMISSION);
    }
    private void openCamera() {
        controller.prepareCameraFile(file -> {
            if (file == null) return;
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
        String[] choices = {"glm-5.3", "glm-5.3-flash", "模型与权限设置"};
        new AlertDialog.Builder(activity).setTitle("选择模型").setItems(choices, (dialog, which) -> {
            if (which == 2) { settings(); return; }
            if (state.busy()) {
                Toast.makeText(activity, "请先停止当前任务再切换模型", Toast.LENGTH_LONG).show();
                return;
            }
            if (choices[which].equalsIgnoreCase(state.model)) return;
            modelButton.setEnabled(false);
            controller.switchModel(choices[which], success -> {
                modelButton.setEnabled(true);
                if (!success) Toast.makeText(activity, controller.state.error,
                    Toast.LENGTH_LONG).show();
            });
        }).show();
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
            cancelVoice = event.getY() < -dp(60); return true;
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
        boolean started = voiceRecorder.tryStart(new VoiceRecordingController.Completion() {
            @Override public void partial(String text) {
                if (voiceRecording && generation == voiceGeneration) composer.setText(text);
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
                if (generation == voiceGeneration)
                    Toast.makeText(activity, "语音转文字失败", Toast.LENGTH_LONG).show();
            }
        });
        if (!started) {
            Toast.makeText(activity, "麦克风正在被其他录音使用", Toast.LENGTH_LONG).show();
            return;
        }
        voiceRecording = true; cancelVoice = false;
        composer.setHint("松开发送，上滑取消");
    }
    private void finishVoice(boolean cancel) {
        if (!voiceRecording) return;
        voiceRecording = false; cancelVoice = false;
        composer.setHint("随心输入");
        if (cancel) {
            voiceGeneration++;
            composer.setText("");
        }
        voiceRecorder.finish(cancel);
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
                TextView heading = text(
                    "允许执行 " + permission.optString("tool") + "？ 点击查看参数", 13, MaiChatTheme.TEXT);
                heading.setOnClickListener(v -> details("操作参数", permission.optString("input")));
                pending.addView(heading, matchWrap());
                LinearLayout choices = row();
                for (String[] choice : new String[][] {{"拒绝", "denied"}, {"允许一次", "approved"},
                         {"本会话允许", "approved_for_session"}})
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
        if (state == null)
            return;
        JSONArray items = state.data.optJSONArray("sessions");
        if (items == null)
            return;
        String[] titles = new String[items.length() + 1];
        titles[0] = "＋ 新对话";
        for (int i = 0; i < items.length(); i++) {
            JSONObject s = items.optJSONObject(i);
            String title = s.optString("title");
            titles[i + 1] =
                (title.equals("New session") ? "新对话" : title) + (s.optBoolean("busy") ? " ···" : "");
        }
        new AlertDialog.Builder(activity)
            .setTitle("对话")
            .setItems(titles,
                (d, which) -> {
                    following = true;
                    if (which == 0)
                        controller.create();
                    else
                        controller.select(items.optJSONObject(which - 1).optString("id"));
                })
            .setNegativeButton("取消", null)
            .show();
    }
    private void menu() {
        new AlertDialog.Builder(activity)
            .setItems(new String[] {"模型与权限", "清空当前对话", "删除当前对话"},
                (d, which) -> {
                    if (which == 0) {
                        settings();
                        return;
                    }
                    if (state == null || state.busy() || selected.isEmpty())
                        return;
                    String op = which == 1 ? "clear" : "delete";
                    new AlertDialog.Builder(activity)
                        .setMessage(which == 1 ? "清空当前对话的所有消息？" : "删除当前对话？")
                        .setNegativeButton("取消", null)
                        .setPositiveButton(
                            "确定", (dialog, w) -> controller.action(op, new JSONObject(), null))
                        .show();
                })
            .show();
    }
    private void settings() {
        if (state == null || !state.ready)
            return;
        LinearLayout form = column();
        form.setPadding(dp(18), dp(8), dp(18), dp(8));
        EditText url = field("HTTPS API 地址", state.baseUrl), name = field("模型名称", state.model),
                 key = field("API Key（留空保留原密钥）", "");
        key.setInputType(
            android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD);
        form.addView(url, matchWrap());
        form.addView(name, matchWrap());
        form.addView(key, matchWrap());
        Spinner policy = new Spinner(activity);
        policy.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item,
            new String[] {"请求批准", "帮我批准", "完全访问"}));
        policy.setSelection(state.policy.equals("never") ? 2 : state.policy.equals("unless-trusted") ? 1 : 0);
        form.addView(policy, matchWrap());
        form.addView(text("密钥使用系统 Keystore 加密保存。完全访问允许自动修改手机工作区文件和访问网络。",
                         12, MaiChatTheme.SECONDARY),
            matchWrap());
        TextView validation = text("", 12, Color.RED);
        form.addView(validation, matchWrap());
        AlertDialog dialog = new AlertDialog.Builder(activity)
                                 .setTitle("模型与权限")
                                 .setView(form)
                                 .setNegativeButton("取消", null)
                                 .setPositiveButton("保存", null)
                                 .create();
        dialog.setOnShowListener(
            v -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(button -> {
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
                controller.save(url.getText().toString(), name.getText().toString(),
                    new String[] {"on-request", "unless-trusted", "never"}[policy.getSelectedItemPosition()],
                    key.getText().toString(), success -> {
                        if (success)
                            dialog.dismiss();
                        else {
                            validation.setText(controller.state.error);
                            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                        }
                    });
            }));
        dialog.show();
    }
    private EditText field(String hint, String initial) {
        EditText view = new EditText(activity);
        view.setTextSize(14);
        view.setSingleLine(true);
        view.setHint(hint);
        view.setText(initial);
        return view;
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
        TextView status;
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
                    status = null;
                    TextView empty = text("有什么可以帮你？\n\n可以聊天、分析图片和文件，也可以语音转文字。", 16,
                        MaiChatTheme.SECONDARY);
                    empty.setPadding(0, dp(50), 0, dp(50));
                    empty.setGravity(Gravity.CENTER);
                    addView(empty, matchWrap());
                }
                return;
            }
            JSONArray array = value.optJSONArray("parts");
            if (array == null)
                array = new JSONArray();
            StringBuilder structure = new StringBuilder(value.optString("id"));
            for (int i = 0; i < array.length(); i++)
                structure.append(array.optJSONObject(i).optString("id"))
                    .append(expanded.contains(array.optJSONObject(i).optString("id")));
            boolean outgoing = value.optString("role").equals("user");
            if (!key.equals(structure.toString())) {
                key = structure.toString();
                removeAllViews();
                parts.clear();
                imageParts.clear();
                status = null;
                if (outgoing) {
                    LinearLayout bubble = column();
                    TextView body = MaiChatTypography.body(activity);
                    bubble.addView(body, matchWrap());
                    for (int i = 0; i < array.length(); i++) {
                        JSONObject part = array.optJSONObject(i);
                        if (part == null || !part.optString("kind").equals("image")) continue;
                        ImageView image = new ImageView(activity);
                        image.setAdjustViewBounds(true);
                        image.setScaleType(ImageView.ScaleType.CENTER_CROP);
                        bubble.addView(image, new LayoutParams(dp(220), dp(150)));
                        imageParts.put(part.optString("id"), image);
                    }
                    bubble.setPadding(dp(12), dp(10), dp(12), dp(10));
                    bubble.setBackground(MaiChatTheme.rounded(Color.rgb(244, 244, 247), 16, activity));
                    LayoutParams lp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
                    lp.gravity = Gravity.END;
                    lp.leftMargin = dp(28);
                    addView(bubble, lp);
                    parts.put("user", body);
                } else {
                    for (int phase = 0; phase < 2; phase++)
                        for (int i = 0; i < array.length(); i++) {
                            JSONObject part = array.optJSONObject(i);
                            String id = part.optString("id"), kind = part.optString("kind");
                            if ((phase == 0) != kind.equals("reasoning"))
                                continue;
                            TextView view = kind.equals("text") ? MaiChatTypography.body(activity)
                                                                : text("", 12, MaiChatTheme.SECONDARY);
                            parts.put(id, view);
                            LayoutParams lp = matchWrap();
                            lp.bottomMargin = dp(12);
                            addView(view, lp);
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
                                addView(detail, lp);
                            }
                        }
                    status = text("", 11, MaiChatTheme.SECONDARY);
                    addView(status, matchWrap());
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
            }
            if (outgoing) {
                StringBuilder body = new StringBuilder();
                for (int i = 0; i < array.length(); i++) {
                    JSONObject part = array.optJSONObject(i);
                    if (part.optString("kind").equals("text")) body.append(part.optString("text"));
                    if (part.optString("kind").equals("image")) {
                        ImageView image = imageParts.get(part.optString("id"));
                        File file = controller.workspaceFile(part.optString("path"));
                        if (image != null && file != null)
                            MessageImageLoader.load(file.getPath(), dp(440), dp(300), image,
                                () -> image.setContentDescription("图片无法显示"));
                    }
                }
                parts.get("user").setText(body.toString());
            } else
                for (int i = 0; i < array.length(); i++) {
                    JSONObject part = array.optJSONObject(i);
                    String id = part.optString("id"), kind = part.optString("kind");
                    TextView view = parts.get(id);
                    if (view == null)
                        continue;
                    if (kind.equals("text")) {
                        String source = part.optString("text");
                        view.setVisibility(source.isEmpty() ? GONE : VISIBLE);
                        if (!source.equals(view.getContentDescription())) {
                            view.setContentDescription(source);
                            MarkdownRenderer.bindStreaming(view, source);
                        }
                    } else {
                        view.setText(kind.equals("reasoning")
                                ? "思考过程  ›"
                                : toolStatus(part.optString("state")) + " " + part.optString("tool") + "  ›");
                        TextView detail = parts.get(id + ":detail");
                        if (detail != null)
                            detail.setText(kind.equals("reasoning") ? part.optString("text")
                                                                    : part.optString("input") + "\n"
                                        + part.optString("output") + "\n" + part.optString("error"));
                    }
                }
            updateTime();
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
            : state.equals("error")      ? "失败"
            : state.equals("pending")    ? "等待授权"
                                         : "正在运行";
    }
    private int dp(int value) {
        return MaiChatTheme.dp(activity, value);
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
