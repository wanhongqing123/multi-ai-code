package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.*;
import java.util.*;
import org.json.JSONArray;
import org.json.JSONObject;

/** Stable composer + recycled message rows. No native calls, disk I/O or parsing on UI. */
final class AIAssistantPanel extends LinearLayout implements AIAssistantController.Listener {
    static final int REQUEST_FILE = 7107;
    private final Activity activity;
    final AIAssistantController controller;
    final EditText composer;
    final Button send;
    private final Button modelButton, policyButton;
    private final TextView error;
    private final LatestMessageList list;
    private final LinearLayout pending;
    private final MessageAdapter adapter = new MessageAdapter();
    private AIAssistantController.State state;
    private String selected = "", pendingSignature = "";
    private final Map<String, String> drafts = new HashMap<>();
    private final Set<String> expanded = new HashSet<>();
    private List<JSONObject> messages = Collections.emptyList();
    private boolean following = true, submitting;
    private int visibleLimit = 50;
    private final Button older;
    private boolean visibleToUser = true;
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
        this(activity, AIAssistantController.shared(activity));
    }
    AIAssistantPanel(Activity activity, AIAssistantController controller) {
        super(activity);
        this.activity = activity;
        this.controller = controller;
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
        LinearLayout actions = row();
        actions.addView(button("＋", "导入文本文件", v -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                                .setType("text/*")
                                .addCategory(Intent.CATEGORY_OPENABLE);
            activity.startActivityForResult(intent, REQUEST_FILE);
        }), new LayoutParams(dp(36), dp(40)));
        policyButton = button("请求批准", "权限设置", v -> settings());
        policyButton.setTextSize(11);
        actions.addView(policyButton, new LayoutParams(LayoutParams.WRAP_CONTENT, dp(40)));
        modelButton = button("未配置模型", "模型设置", v -> settings());
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
            drafts.put(selected, composer.getText().toString());
            boolean firstSend = submitting && selected.isEmpty();
            selected = next.selected;
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
            && (busy || !composer.getText().toString().trim().isEmpty()));
        // Keep the native editor focused while submit runs; disabling it collapses the IME.
    }
    private void send() {
        if (state == null)
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
        submitting = true;
        updateSend();
        following = true;
        controller.send(source, success -> {
            submitting = false;
            if (success && (origin.isEmpty() || selected.equals(origin))
                && composer.getText().toString().equals(source))
                composer.setText("");
            updateSend();
            if (success)
                locateLatest();
        });
    }
    void importFile(Uri uri) {
        String origin = selected;
        controller.importFile(uri, name -> {
            if (name == null)
                return;
            String text = "\n请查看文件：" + name + "\n";
            if (origin.equals(selected))
                composer.append(text);
            else
                drafts.put(origin, drafts.getOrDefault(origin, "") + text);
        });
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
                    status = null;
                    TextView empty = text("有什么可以帮你？\n\n可以聊天、分析文本和处理导入的文件。", 16,
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
                status = null;
                if (outgoing) {
                    TextView body = MaiChatTypography.body(activity);
                    body.setPadding(dp(12), dp(10), dp(12), dp(10));
                    body.setBackground(MaiChatTheme.rounded(Color.rgb(244, 244, 247), 16, activity));
                    LayoutParams lp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
                    lp.gravity = Gravity.END;
                    lp.leftMargin = dp(28);
                    addView(body, lp);
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
                for (int i = 0; i < array.length(); i++)
                    body.append(array.optJSONObject(i).optString("text"));
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
