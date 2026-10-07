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
        if (source.trim().isEmpty() && sending.isEmpty()) return;
        if (sending.stream().anyMatch(file -> file.image) &&
            !AIAssistantMediaPolicy.supportsImages(state.model)) {
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
                    ((MainActivity) activity).requestGalleryAccess(() -> {
                        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                            .setType("image/*").addCategory(Intent.CATEGORY_OPENABLE);
                        activity.startActivityForResult(intent, REQUEST_IMAGE);
                    });
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
        String[] choices = {"glm-5.3", "glm-5.3-flash", "deepseek-flash", "模型与权限设置"};
        new AlertDialog.Builder(activity).setTitle("选择模型").setItems(choices, (dialog, which) -> {
            if (which == choices.length - 1) { settings(); return; }
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
    private boolean isPaidGeneration(String tool) {
        return tool.equals("wan_video") || tool.equals("wan_video_edit")
            || tool.equals("seedance_video") || tool.equals("seedream_image")
            || tool.equals("qwen_image") || tool.equals("glm_video") || tool.equals("glm_image")
            || tool.equals("kling_video") || tool.equals("kling_image")
            || tool.equals("minimax_video") || tool.equals("minimax_image");
    }
    private String paidApprovalTitle(String tool, JSONObject input) {
        boolean video = tool.equals("wan_video") || tool.equals("wan_video_edit")
            || tool.equals("seedance_video") || tool.equals("glm_video")
            || tool.equals("kling_video") || tool.equals("minimax_video");
        return "确认" + (isPaidRevision(tool, input) ? "编辑" : "生成")
            + (video ? "视频" : "图片");
    }
    private boolean isPaidRevision(String tool, JSONObject input) {
        return input.optString("action").equals("revise")
            || input.optString("mode").equals("edit")
            || input.optString("mode").equals("extend") || tool.equals("wan_video_edit");
    }
    private String paidProvider(String tool) {
        if (tool.equals("wan_video") || tool.equals("wan_video_edit")) return "万相";
        if (tool.equals("qwen_image")) return "通义千问";
        if (tool.equals("glm_video") || tool.equals("glm_image")) return "GLM";
        if (tool.equals("kling_video") || tool.equals("kling_image")) return "可灵";
        if (tool.equals("minimax_video") || tool.equals("minimax_image")) return "海螺 / MiniMax";
        return tool.equals("seedance_video") ? "Seedance" : "Seedream";
    }
    private String paidSpecs(JSONObject input) {
        JSONObject production = input.optJSONObject("production");
        if (production == null) production = new JSONObject();
        java.util.List<String> values = new java.util.ArrayList<>();
        String model = input.optString("model");
        if (model.equals("MiniMax-H3") || model.equals("MiniMax-H3-Max"))
            values.add(model.equals("MiniMax-H3-Max") ? "H3 Max" : "H3");
        int duration = input.optInt("duration", production.optInt("duration", 0));
        if (duration > 0) values.add(duration + " 秒");
        String resolution = input.optString("resolution", production.optString("resolution"));
        String ratio = input.optString("ratio", production.optString("ratio"));
        if (!resolution.isEmpty()) values.add(resolution);
        if (!ratio.isEmpty()) values.add(ratio.equals("adaptive") ? "按素材比例" : ratio);
        if (!input.optString("virtual_avatar_asset_id").isEmpty())
            values.add("平台虚拟人像");
        if (!input.optString("authorized_portrait_asset_id").isEmpty())
            values.add("已授权真人形象");
        if (!input.optString("size").isEmpty()) values.add(input.optString("size"));
        JSONArray images = input.optJSONArray("reference_image_paths");
        if (images == null) images = input.optJSONArray("image_paths");
        int imageCount = images == null ? 0 : images.length();
        if (!input.optString("reference_image_path").isEmpty()) imageCount++;
        if (!input.optString("image_path").isEmpty()
            || !input.optString("first_frame").isEmpty()) imageCount++;
        if (!input.optString("last_frame_path").isEmpty()
            || !input.optString("last_frame").isEmpty()) imageCount++;
        JSONArray content = input.optJSONArray("content");
        int videoCount = !input.optString("video_path").isEmpty()
            || !input.optString("reference_video_path").isEmpty()
            || !input.optString("reference_video").isEmpty() ? 1 : 0;
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
                if (paid) {
                    pending.addView(text(paidApprovalTitle(tool, input), 17, MaiChatTheme.TEXT), matchWrap());
                    String note = !input.optString("virtual_avatar_asset_id").isEmpty()
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
                    String specs = paidSpecs(input);
                    if (!specs.isEmpty())
                        pending.addView(text(specs, 13, MaiChatTheme.SECONDARY), matchWrap());
                    TextView more = text("查看完整请求", 12, MaiChatTheme.SECONDARY);
                    more.setOnClickListener(v -> details("完整请求", permission.optString("input")));
                    pending.addView(more, matchWrap());
                } else {
                    TextView heading = text(
                        "允许执行 " + tool + "？ 点击查看参数", 13, MaiChatTheme.TEXT);
                    heading.setOnClickListener(v -> details("操作参数", permission.optString("input")));
                    pending.addView(heading, matchWrap());
                }
                LinearLayout choices = row();
                java.util.List<String[]> availableChoices = new java.util.ArrayList<>();
                availableChoices.add(new String[] {paid ? "取消" : "拒绝", "denied"});
                availableChoices.add(new String[] {
                    paid ? (isPaidRevision(tool, input) ? "确认编辑" : "确认生成")
                    : permission.optBoolean("rememberOnApproval")
                        ? (permission.optInt("fileCount", 0) > 1
                            ? "允许并记住这些文件" : "允许并记住此文件")
                        : "允许一次", "approved"});
                if (permission.optBoolean("allowForSession", true)
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
                 key = field("API Key（留空保留原密钥）", ""),
                 arkKey = field("方舟创作 Key（留空保留原密钥）", ""),
                 wanKey = field("百炼创作 Key（Wan / Qwen，留空保留原密钥）", ""),
                 wanWorkspace = field("百炼 Workspace ID", controller.wanWorkspaceId()),
                 klingKey = field("可灵 API Key（留空保留原密钥）", ""),
                 miniMaxKey = field("海螺 / MiniMax API Key（留空保留原密钥）", "");
        key.setInputType(
            android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD);
        arkKey.setInputType(
            android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD);
        wanKey.setInputType(
            android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD);
        klingKey.setInputType(
            android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD);
        miniMaxKey.setInputType(
            android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD);
        form.addView(url, matchWrap());
        form.addView(name, matchWrap());
        form.addView(text("接口协议", 14, MaiChatTheme.SECONDARY), matchWrap());
        Spinner wire = new Spinner(activity);
        wire.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item,
            new String[] {"Chat Completions", "Responses"}));
        wire.setSelection(controller.currentWire().equals("chat_completions") ? 0 : 1);
        form.addView(wire, matchWrap());
        final String[] glmChatUrl = {controller.glmChatBaseUrl()};
        Runnable updateEndpoint = () -> {
            String selectedName = name.getText().toString().trim();
            String currentUrl = url.getText().toString().trim();
            if (selectedName.startsWith("glm-") &&
                currentUrl.startsWith("https://open.bigmodel.cn/")) {
                if (wire.getSelectedItemPosition() == 1 &&
                    !currentUrl.equals(AIAssistantController.GLM_RESPONSES_URL)) {
                    glmChatUrl[0] = currentUrl;
                    url.setText(AIAssistantController.GLM_RESPONSES_URL);
                } else if (wire.getSelectedItemPosition() == 0 &&
                           currentUrl.equals(AIAssistantController.GLM_RESPONSES_URL)) {
                    url.setText(glmChatUrl[0]);
                }
            } else if (selectedName.equals("deepseek-flash") &&
                       !currentUrl.equals(AIAssistantController.DEEPSEEK_URL)) {
                url.setText(AIAssistantController.DEEPSEEK_URL);
            }
        };
        wire.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                                 int position, long id) { updateEndpoint.run(); }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        final String[] lastProvider = {state.model.equals("deepseek-flash") ? "deepseek"
            : state.model.startsWith("glm-") ? "glm" : "custom"};
        name.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence text, int start, int count,
                                                    int after) {}
            @Override public void onTextChanged(CharSequence text, int start, int before,
                                                int count) {}
            @Override public void afterTextChanged(Editable text) {
                String selected = text.toString().trim();
                String provider = selected.equals("deepseek-flash") ? "deepseek"
                    : selected.startsWith("glm-") ? "glm" : "custom";
                if (!provider.equals(lastProvider[0])) {
                    if (provider.equals("deepseek")) {
                        url.setText(AIAssistantController.DEEPSEEK_URL);
                        wire.setSelection(controller.deepseekWire().equals("chat_completions") ? 0 : 1);
                    } else if (provider.equals("glm")) {
                        url.setText(controller.glmBaseUrl());
                        wire.setSelection(controller.glmWire().equals("chat_completions") ? 0 : 1);
                    }
                    lastProvider[0] = provider;
                }
                updateEndpoint.run();
            }
        });
        form.addView(key, matchWrap());
        form.addView(arkKey, matchWrap());
        form.addView(wanKey, matchWrap());
        form.addView(wanWorkspace, matchWrap());
        form.addView(klingKey, matchWrap());
        form.addView(miniMaxKey, matchWrap());
        Spinner policy = new Spinner(activity);
        policy.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item,
            new String[] {"请求批准", "帮我批准", "完全访问"}));
        policy.setSelection(state.policy.equals("never") ? 2 : state.policy.equals("unless-trusted") ? 1 : 0);
        form.addView(policy, matchWrap());
        form.addView(text("密钥使用系统 Keystore 加密保存。三种模式均可访问 App 目录；帮我批准会在首次修改每个文件时询问，完全访问不逐次询问。",
                         12, MaiChatTheme.SECONDARY),
            matchWrap());
        form.addView(text("本地图片处理使用 FFmpeg（LGPLv2.1+）；完整源码随项目放在 MaiAgent/third_party/ffmpeg。",
                         12, MaiChatTheme.SECONDARY), matchWrap());
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
                    key.getText().toString(), wire.getSelectedItemPosition() == 0
                        ? "chat_completions" : "responses", glmChatUrl[0], success -> {
                        if (!success) {
                            validation.setText(controller.state.error);
                            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                            return;
                        }
                        controller.saveArkKey(arkKey.getText().toString(), arkSaved -> {
                            if (!arkSaved) {
                                validation.setText(controller.state.error);
                                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                                return;
                            }
                            controller.saveWanCredentials(wanKey.getText().toString(),
                                wanWorkspace.getText().toString(), wanSaved -> {
                                    if (wanSaved) {
                                        controller.saveCreativeKeys(klingKey.getText().toString(),
                                            miniMaxKey.getText().toString(), creativeSaved -> {
                                                if (creativeSaved) dialog.dismiss();
                                                else {
                                                    validation.setText(controller.state.error);
                                                    dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                                                }
                                            });
                                    } else {
                                        validation.setText(controller.state.error);
                                        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                                    }
                                });
                        });
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
        final Map<String, Button> videoParts = new HashMap<>();
        final Map<String, Button> pdfCards = new HashMap<>();
        final Map<String, LinearLayout> mediaCards = new HashMap<>();
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
                    videoParts.clear();
                    pdfCards.clear();
                    mediaCards.clear();
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
                videoParts.clear();
                pdfCards.clear();
                mediaCards.clear();
                status = null;
                if (outgoing) {
                    LinearLayout bubble = column();
                    TextView body = MaiChatTypography.body(activity);
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
                        Button video = new Button(activity);
                        video.setAllCaps(false);
                        video.setTextSize(14);
                        video.setBackground(MaiChatTheme.bordered(
                            MaiChatTheme.BLUE_SOFT, MaiChatTheme.BORDER, 10, activity));
                        bubble.addView(video, new LayoutParams(dp(220), dp(88)));
                        videoParts.put(part.optString("id"), video);
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
                                LinearLayout media = column();
                                media.setVisibility(GONE);
                                mediaCards.put(id, media);
                                LayoutParams mediaLayout = matchWrap();
                                mediaLayout.bottomMargin = dp(12);
                                addView(media, mediaLayout);
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
                            MessageImageLoader.load(file.getPath(), dp(220), dp(150), image,
                                () -> image.setContentDescription("图片无法显示"));
                    }
                    if (part.optString("kind").equals("video")) {
                        Button card = videoParts.get(part.optString("id"));
                        File file = controller.workspaceFile(part.optString("path"));
                        if (card != null) {
                            boolean playable = file != null && file.isFile();
                            card.setText(playable ? "▶  " + file.getName() : "视频文件已丢失");
                            card.setEnabled(playable && activity instanceof MainActivity);
                            card.setOnClickListener(view -> {
                                if (playable) ((MainActivity) activity).openAgentVideo(
                                    file.getAbsolutePath());
                            });
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
                        Button pdfCard = pdfCards.get(id);
                        if (pdfCard != null) {
                            String path = pdfPathFromToolOutput(part);
                            pdfCard.setVisibility(path == null ? GONE : VISIBLE);
                            if (path != null) {
                                pdfCard.setTag(path);
                                pdfCard.setText("PDF  " + new File(path).getName() + "  ·  预览");
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
    private void showAgentMediaCard(LinearLayout container, File file, String type,
                                    String caption) {
        container.removeAllViews();
        if (type.equals("image") || type.equals("video")) {
            FrameLayout preview = new FrameLayout(activity);
            ImageView cover = new ImageView(activity);
            cover.setScaleType(ImageView.ScaleType.FIT_CENTER);
            preview.addView(cover, new FrameLayout.LayoutParams(dp(220), dp(240)));
            MessageImageLoader.load(file.getAbsolutePath(), dp(220), dp(240), cover,
                () -> cover.setContentDescription("媒体无法显示"));
            if (type.equals("video")) {
                TextView play = text("▶", 30, Color.WHITE);
                play.setGravity(Gravity.CENTER);
                play.setBackground(MaiChatTheme.rounded(Color.argb(180, 0, 0, 0), 28, activity));
                FrameLayout.LayoutParams playLayout = new FrameLayout.LayoutParams(dp(52), dp(52));
                playLayout.gravity = Gravity.CENTER;
                preview.addView(play, playLayout);
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
                        activity.runOnUiThread(() -> {
                            if (file.getPath().equals(container.getTag()))
                                duration.setText(String.format(Locale.ROOT, "%d:%02d",
                                    ms / 60000, (ms / 1000) % 60));
                        });
                    } catch (Exception ignored) { }
                    finally { try { metadata.release(); } catch (Exception ignored) { } }
                }, "agent-video-metadata").start();
                preview.setOnClickListener(view -> {
                    if (activity instanceof MainActivity)
                        ((MainActivity) activity).openAgentVideo(file.getAbsolutePath());
                });
            } else {
                preview.setOnClickListener(view -> {
                    if (activity instanceof MainActivity)
                        ((MainActivity) activity).showFullScreenImage(file.getAbsolutePath());
                });
            }
            container.addView(preview, new LayoutParams(dp(220), dp(240)));
            preview.setOnLongClickListener(view -> {
                showAgentMediaMenu(view, file, type); return true;
            });
        } else if (type.equals("audio")) {
            Button audio = button("▶  " + file.getName(), "播放音频", null);
            audio.setBackground(MaiChatTheme.bordered(
                MaiChatTheme.BLUE_SOFT, MaiChatTheme.BORDER, 10, activity));
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
                            prepared.start(); audio.setText("❚❚  " + file.getName());
                        });
                        player.setOnCompletionListener(done -> {
                            audio.setText("▶  " + file.getName());
                            done.seekTo(0);
                        });
                        player.prepareAsync();
                    } else if (!ready[0]) {
                        return;
                    } else if (playback[0].isPlaying()) {
                        playback[0].pause(); audio.setText("▶  " + file.getName());
                    } else {
                        playback[0].start(); audio.setText("❚❚  " + file.getName());
                    }
                } catch (Exception failure) {
                    audio.setText("音频无法播放");
                }
            });
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
            container.addView(audio, matchWrap());
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
