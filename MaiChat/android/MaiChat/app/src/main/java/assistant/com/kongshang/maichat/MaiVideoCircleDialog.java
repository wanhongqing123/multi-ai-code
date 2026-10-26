// Android presentation for the account-bound AI video circle.
package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.util.Collections;
import org.json.JSONArray;
import org.json.JSONObject;

final class MaiVideoCircleDialog {
    private static final class CappedScrollView extends ScrollView {
        private final int maximumHeight;
        CappedScrollView(Activity activity, int maximumHeight) {
            super(activity);
            this.maximumHeight = maximumHeight;
        }
        @Override protected void onMeasure(int widthSpec, int heightSpec) {
            int available = View.MeasureSpec.getMode(heightSpec) == View.MeasureSpec.UNSPECIFIED
                ? maximumHeight : Math.min(View.MeasureSpec.getSize(heightSpec), maximumHeight);
            super.onMeasure(widthSpec, View.MeasureSpec.makeMeasureSpec(
                available, View.MeasureSpec.AT_MOST));
        }
    }

    private final Activity activity;
    private final MaiVideoCircle client;
    private final MaiChatAttachmentDownload.NativeTransport downloads;
    private MaiChatAttachmentDownload.Scope downloadScope =
        new MaiChatAttachmentDownload.Scope();
    private AlertDialog dialog;
    private LinearLayout content;
    private TextView hint;
    private Button discover, mine, blocked;
    private int mode = MaiVideoCircle.FEED;
    private int generation;
    private String cursor = "";
    private boolean loading;
    private boolean playing;

    MaiVideoCircleDialog(Activity activity, MaiVideoCircle client) {
        this.activity = activity;
        this.client = client;
        this.downloads = new MaiChatAttachmentDownload.NativeTransport(
            client::playbackTrustPem);
    }

    static void confirmPublish(Activity activity, MaiVideoCircle client,
                               JSONObject completedPart, File original) {
        if (MaiVideoCircle.inspectCard(completedPart).status != MaiVideoCircle.OK ||
            original == null || !original.isFile()) return;
        LinearLayout panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(activity, 22), dp(activity, 20),
            dp(activity, 22), dp(activity, 18));
        TextView heading = text(activity, "发布到 AI 视频圈", 20, MaiChatTheme.TEXT);
        heading.setTypeface(MaiChatTypography.semibold());
        LinearLayout.LayoutParams headingLayout = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        headingLayout.bottomMargin = dp(activity, 14);
        panel.addView(heading, headingLayout);
        ImageView cover = new ImageView(activity);
        cover.setScaleType(ImageView.ScaleType.CENTER_CROP);
        panel.addView(cover, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(activity, 176)));
        MessageImageLoader.load(original.getAbsolutePath(), dp(activity, 320),
            dp(activity, 176), cover, () -> cover.setContentDescription("视频封面不可用"));
        cover.setContentDescription("预览待发布视频");
        cover.setOnClickListener(view -> {
            if (activity instanceof MainActivity)
                ((MainActivity) activity).openAgentMedia(original.getAbsolutePath(), true,
                    Collections.singletonList(
                        new MainActivity.GalleryItem(original.getAbsolutePath(), true)));
        });
        TextView badge = text(activity, "AI 生成 · 将公开给其他 MaiChat 用户", 13,
            MaiChatTheme.BLUE);
        badge.setPadding(0, dp(activity, 12), 0, dp(activity, 4));
        panel.addView(badge);
        MaiChatAuthClient auth = MaiChatAuthClient.shared(activity);
        String initialAccount = auth.accountId();
        String publisher = auth.nickname().isEmpty() ? auth.accountId() : auth.nickname();
        TextView identity = text(activity, "发布者：" + publisher, 12,
            MaiChatTheme.SECONDARY);
        panel.addView(identity);
        EditText title = new EditText(activity);
        title.setSingleLine(true);
        title.setTextSize(16);
        title.setHint("给视频起个标题");
        title.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE);
        title.setFilters(new android.text.InputFilter[] {
            new android.text.InputFilter.LengthFilter(80)
        });
        panel.addView(title);
        TextView detail = text(activity,
            "提交时会生成带 AI 标识的新文件。审核通过后，其他用户可以浏览和播放。", 13,
            MaiChatTheme.SECONDARY);
        detail.setPadding(0, dp(activity, 8), 0, 0);
        panel.addView(detail);
        ScrollView scroll = new ScrollView(activity);
        scroll.setFillViewport(false);
        scroll.addView(panel);
        AlertDialog prompt = new AlertDialog.Builder(activity).setView(scroll).create();
        LinearLayout actions = new LinearLayout(activity);
        actions.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams actionsLayout = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(activity, 48));
        actionsLayout.topMargin = dp(activity, 18);
        Button cancel = new Button(activity);
        cancel.setText("取消");
        cancel.setAllCaps(false);
        cancel.setTextColor(MaiChatTheme.SECONDARY);
        cancel.setBackgroundColor(Color.TRANSPARENT);
        cancel.setOnClickListener(view -> prompt.dismiss());
        actions.addView(cancel, new LinearLayout.LayoutParams(dp(activity, 76),
            dp(activity, 48)));
        Button submit = new Button(activity);
        submit.setText("提交审核");
        submit.setAllCaps(false);
        submit.setTextColor(Color.WHITE);
        submit.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 10, activity));
        LinearLayout.LayoutParams submitLayout = new LinearLayout.LayoutParams(
            dp(activity, 110), dp(activity, 48));
        submitLayout.leftMargin = dp(activity, 10);
        actions.addView(submit, submitLayout);
        panel.addView(actions, actionsLayout);
        submit.setOnClickListener(view -> {
                if (!initialAccount.equals(auth.accountId())) {
                    prompt.dismiss();
                    Toast.makeText(activity, "账号已切换，请重新选择视频", Toast.LENGTH_LONG).show();
                    return;
                }
                String value = title.getText().toString().trim();
                if (value.isEmpty() || value.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 240) {
                    title.setError("请输入不超过 80 字的标题");
                    return;
                }
                prompt.dismiss();
                AlertDialog progress = new AlertDialog.Builder(activity)
                    .setTitle("准备发布")
                    .setMessage("正在添加 AI 标识并上传视频…")
                    .setNegativeButton("取消", (d, which) -> client.cancel())
                    .setCancelable(false)
                    .create();
                progress.show();
                client.publish(completedPart, original, value, result -> {
                    progress.dismiss();
                    Toast.makeText(activity, result.message, Toast.LENGTH_LONG).show();
                });
            });
        prompt.show();
    }

    void show() {
        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackground(MaiChatTheme.rounded(Color.WHITE, 20, activity));
        root.setPadding(dp(activity, 18), dp(activity, 16),
            dp(activity, 18), dp(activity, 20));
        LinearLayout heading = new LinearLayout(activity);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text(activity, "AI 视频圈", 20, MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        heading.addView(title, new LinearLayout.LayoutParams(0,
            ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button close = button("关闭", () -> dialog.dismiss());
        close.setContentDescription("关闭 AI 视频圈");
        heading.addView(close);
        root.addView(heading);
        TextView note = text(activity, "AI 生成 · 审核后公开", 12, MaiChatTheme.SECONDARY);
        note.setPadding(0, dp(activity, 4), 0, dp(activity, 14));
        root.addView(note);
        LinearLayout tabs = new LinearLayout(activity);
        discover = button("发现", () -> select(MaiVideoCircle.FEED));
        mine = button("我的", () -> select(MaiVideoCircle.MINE));
        blocked = button("已屏蔽", () -> select(MaiVideoCircle.BLOCKS_LIST));
        tabs.addView(discover, new LinearLayout.LayoutParams(0, dp(activity, 48), 1));
        tabs.addView(mine, new LinearLayout.LayoutParams(0, dp(activity, 48), 1));
        tabs.addView(blocked, new LinearLayout.LayoutParams(0, dp(activity, 48), 1));
        root.addView(tabs);
        hint = text(activity, "", 13, MaiChatTheme.SECONDARY);
        hint.setPadding(0, dp(activity, 14), 0, dp(activity, 8));
        root.addView(hint);
        ScrollView scroll = new CappedScrollView(activity,
            Math.round(activity.getResources().getDisplayMetrics().heightPixels * 0.58f));
        scroll.setFillViewport(false);
        scroll.setVerticalScrollBarEnabled(true);
        content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        dialog = new AlertDialog.Builder(activity).setView(root).create();
        dialog.setOnDismissListener(ignored -> {
            generation++;
            stopPlayback();
            client.cancel();
        });
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(
                Color.TRANSPARENT));
            window.setGravity(Gravity.BOTTOM);
            window.setLayout(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        select(MaiVideoCircle.FEED);
    }

    private void select(int next) {
        stopPlayback();
        mode = next;
        cursor = "";
        generation++;
        loading = false;
        content.removeAllViews();
        discover.setSelected(next == MaiVideoCircle.FEED);
        mine.setSelected(next == MaiVideoCircle.MINE);
        blocked.setSelected(next == MaiVideoCircle.BLOCKS_LIST);
        discover.setContentDescription("发现" + (next == MaiVideoCircle.FEED ? "，已选中" : ""));
        mine.setContentDescription("我的" + (next == MaiVideoCircle.MINE ? "，已选中" : ""));
        blocked.setContentDescription("已屏蔽" +
            (next == MaiVideoCircle.BLOCKS_LIST ? "，已选中" : ""));
        discover.setTextColor(next == MaiVideoCircle.FEED ? MaiChatTheme.BLUE : MaiChatTheme.TEXT);
        mine.setTextColor(next == MaiVideoCircle.MINE ? MaiChatTheme.BLUE : MaiChatTheme.TEXT);
        blocked.setTextColor(next == MaiVideoCircle.BLOCKS_LIST ? MaiChatTheme.BLUE : MaiChatTheme.TEXT);
        load(false);
    }

    private void load(boolean more) {
        if (loading || dialog == null || !dialog.isShowing()) return;
        loading = true;
        final int requestGeneration = generation;
        hint.setText("正在加载…");
        if (more && content.getChildCount() > 0)
            content.removeViewAt(content.getChildCount() - 1);
        JSONObject payload = new JSONObject();
        if (more && !cursor.isEmpty()) {
            try { payload.put("before", cursor); } catch (Exception ignored) { }
        }
        client.request(mode, payload, result -> {
            if (requestGeneration != generation || dialog == null || !dialog.isShowing()) return;
            loading = false;
            if (!result.ok()) {
                hint.setText(MaiVideoCircle.failureText(result));
                Button retry = button("重试", () -> load(false));
                content.addView(retry);
                return;
            }
            JSONObject body = result.json();
            JSONArray rows = mode == MaiVideoCircle.BLOCKS_LIST
                ? body.optJSONArray("authors") : body.optJSONArray("posts");
            if (rows == null) rows = new JSONArray();
            if (!more) content.removeAllViews();
            if (rows.length() == 0 && content.getChildCount() == 0) {
                String empty = mode == MaiVideoCircle.FEED ? "还没有公开视频"
                    : mode == MaiVideoCircle.MINE ? "你还没有发布视频" : "没有屏蔽的创作者";
                content.addView(text(activity, empty, 15, MaiChatTheme.SECONDARY));
            }
            for (int i = 0; i < rows.length(); ++i) {
                JSONObject row = rows.optJSONObject(i);
                if (row == null) continue;
                content.addView(mode == MaiVideoCircle.BLOCKS_LIST
                    ? blockedRow(row) : postRow(row, mode == MaiVideoCircle.MINE));
            }
            hint.setText(mode == MaiVideoCircle.FEED ? "审核通过的视频"
                : mode == MaiVideoCircle.MINE ? "发布与审核状态" : "你屏蔽的创作者");
            cursor = body.isNull("next_cursor") ? "" : body.optString("next_cursor", "");
            if ((mode == MaiVideoCircle.FEED || mode == MaiVideoCircle.MINE) &&
                !cursor.isEmpty())
                content.addView(button("加载更多", () -> load(true)));
        });
    }

    private View postRow(JSONObject post, boolean own) {
        LinearLayout card = card();
        String id = post.optString("id");
        boolean myPublicPost = !own && post.optString("author_id").equals(
            MaiChatAuthClient.shared(activity).accountId());
        LinearLayout summary = new LinearLayout(activity);
        summary.setGravity(Gravity.CENTER_VERTICAL);
        TextView poster = text(activity, "▶", 29, Color.WHITE);
        poster.setGravity(Gravity.CENTER);
        poster.setContentDescription("AI 生成视频");
        GradientDrawable posterBackground = new GradientDrawable(
            GradientDrawable.Orientation.TL_BR,
            new int[] {Color.rgb(30, 68, 113), Color.rgb(69, 139, 189)});
        posterBackground.setCornerRadius(dp(activity, 10));
        poster.setBackground(posterBackground);
        summary.addView(poster, new LinearLayout.LayoutParams(dp(activity, 68),
            dp(activity, 68)));
        LinearLayout labels = new LinearLayout(activity);
        labels.setOrientation(LinearLayout.VERTICAL);
        labels.setPadding(dp(activity, 12), 0, 0, 0);
        TextView title = text(activity, post.optString("title", "视频"), 16, MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        title.setMaxLines(2);
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        labels.addView(title);
        String author = own || myPublicPost ? "我的视频" :
            post.optString("author_name", "创作者");
        String status = own ? " · " + statusText(post.optString("status")) : "";
        TextView metadata = text(activity, "AI 生成 · " + author + status, 12,
            MaiChatTheme.SECONDARY);
        metadata.setPadding(0, dp(activity, 5), 0, 0);
        labels.addView(metadata);
        summary.addView(labels, new LinearLayout.LayoutParams(0,
            ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        card.addView(summary);
        String reviewReason = post.isNull("review_reason") ? "" :
            post.optString("review_reason");
        if (own && !reviewReason.isEmpty()) {
            TextView reason = text(activity, reviewReason, 12,
                MaiChatTheme.SECONDARY);
            card.addView(reason);
        }
        LinearLayout actions = new LinearLayout(activity);
        actions.setGravity(Gravity.END);
        actions.setPadding(0, dp(activity, 7), 0, 0);
        if (!own || post.optString("status").equals("pending") ||
            post.optString("status").equals("approved"))
            actions.addView(button("播放", () -> play(id)));
        if (own) {
            if (!post.optString("status").equals("withdrawn"))
                actions.addView(button("撤回", () -> confirmWithdraw(id)));
        } else if (!myPublicPost) {
            actions.addView(button("举报", () -> report(id)));
            actions.addView(button("屏蔽作者", () -> confirmBlock(post.optString("author_id"))));
        }
        card.addView(actions);
        return card;
    }

    private View blockedRow(JSONObject author) {
        LinearLayout card = card();
        TextView title = text(activity, author.optString("author_name", "创作者"), 16,
            MaiChatTheme.TEXT);
        card.addView(title);
        card.addView(button("取消屏蔽", () -> act(MaiVideoCircle.UNBLOCK,
            value("author_id", author.optString("author_id")))));
        return card;
    }

    private void play(String postId) {
        if (playing) return;
        playing = true;
        hint.setText("正在获取播放链接…");
        client.request(MaiVideoCircle.PLAY_LINK, value("post_id", postId), result -> {
            if (!result.ok()) {
                playing = false;
                hint.setText(MaiVideoCircle.failureText(result));
                return;
            }
            String url = result.json().optString("read_url");
            if (!url.startsWith("https://")) {
                playing = false;
                hint.setText("播放链接无效");
                return;
            }
            File directory = new File(activity.getCacheDir(), "video-circle-playback");
            if (!directory.isDirectory() && !directory.mkdirs()) {
                playing = false;
                hint.setText("无法准备播放缓存");
                return;
            }
            File target = new File(directory, postId + ".mp4");
            hint.setText("正在下载视频…");
            MaiChatAttachmentDownload.Scope playbackScope = downloadScope;
            downloads.download(url, target, 200_000_000, playbackScope, download -> {
                if (download.ok()) trimPlaybackCache(directory, target);
                activity.runOnUiThread(() -> {
                    if (dialog == null || !dialog.isShowing() ||
                        playbackScope != downloadScope) return;
                    playing = false;
                    if (!download.ok()) {
                        hint.setText("视频下载失败，请重试");
                        return;
                    }
                    hint.setText("AI 生成 · 审核后公开");
                    if (activity instanceof MainActivity)
                        ((MainActivity) activity).openAgentMedia(target.getAbsolutePath(),
                            true, Collections.singletonList(
                                new MainActivity.GalleryItem(target.getAbsolutePath(), true)));
                });
            });
        });
    }

    private void confirmWithdraw(String postId) {
        new AlertDialog.Builder(activity).setMessage("撤回后其他人将不能再打开这个视频。确定撤回？")
            .setNegativeButton("取消", null)
            .setPositiveButton("撤回", (ignored, which) ->
                act(MaiVideoCircle.WITHDRAW, value("post_id", postId))).show();
    }

    private void confirmBlock(String authorId) {
        new AlertDialog.Builder(activity).setMessage("屏蔽后不再显示这位创作者的视频。确定屏蔽？")
            .setNegativeButton("取消", null)
            .setPositiveButton("屏蔽", (ignored, which) ->
                act(MaiVideoCircle.BLOCK, value("author_id", authorId))).show();
    }

    private void report(String postId) {
        String[] titles = {"垃圾内容", "骚扰", "色情", "暴力", "侵权", "其他"};
        String[] reasons = {"spam", "harassment", "sexual", "violence", "copyright", "other"};
        new AlertDialog.Builder(activity).setTitle("举报视频")
            .setItems(titles, (ignored, which) -> {
                JSONObject payload = value("post_id", postId);
                try { payload.put("reason", reasons[which]); } catch (Exception impossible) { }
                act(MaiVideoCircle.REPORT, payload);
            }).show();
    }

    private void act(int action, JSONObject payload) {
        stopPlayback();
        hint.setText("正在处理…");
        client.request(action, payload, result -> {
            if (!result.ok()) {
                hint.setText(MaiVideoCircle.failureText(result));
                return;
            }
            String notice = action == MaiVideoCircle.REPORT ? "举报已提交" :
                action == MaiVideoCircle.BLOCK ? "已屏蔽" :
                action == MaiVideoCircle.UNBLOCK ? "已取消屏蔽" :
                result.json().optBoolean("cleanup_pending") ?
                    "已从列表移除，文件清理中" : "已撤回";
            Toast.makeText(activity, notice, Toast.LENGTH_SHORT).show();
            select(mode);
        });
    }

    private void stopPlayback() {
        playing = false;
        downloadScope.cancel();
        downloadScope = new MaiChatAttachmentDownload.Scope();
    }

    private static void trimPlaybackCache(File directory, File keep) {
        File[] files = directory.listFiles((folder, name) -> name.endsWith(".mp4"));
        if (files == null) return;
        java.util.Arrays.sort(files, (left, right) ->
            Long.compare(right.lastModified(), left.lastModified()));
        int retained = 0;
        long bytes = 0;
        for (File file : files) {
            if (file.equals(keep)) {
                retained++;
                bytes += file.length();
                continue;
            }
            if (retained >= 3 || bytes + file.length() > 400_000_000) file.delete();
            else {
                retained++;
                bytes += file.length();
            }
        }
    }

    private LinearLayout card() {
        LinearLayout card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(activity, 14), dp(activity, 12),
            dp(activity, 14), dp(activity, 12));
        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.WHITE);
        background.setCornerRadius(dp(activity, 12));
        background.setStroke(dp(activity, 1), MaiChatTheme.BORDER);
        card.setBackground(background);
        LinearLayout.LayoutParams margin = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        margin.bottomMargin = dp(activity, 10);
        card.setLayoutParams(margin);
        return card;
    }

    private Button button(String label, Runnable action) {
        Button button = new Button(activity);
        button.setText(label);
        button.setTextSize(13);
        button.setTextColor(MaiChatTheme.BLUE);
        button.setAllCaps(false);
        button.setMinHeight(dp(activity, 48));
        button.setMinimumHeight(dp(activity, 48));
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setOnClickListener(view -> action.run());
        return button;
    }

    private static TextView text(Activity activity, String value, int size, int color) {
        TextView view = new TextView(activity);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private static JSONObject value(String key, String value) {
        JSONObject result = new JSONObject();
        try { result.put(key, value); } catch (Exception impossible) { }
        return result;
    }

    private static String statusText(String status) {
        switch (status) {
            case "pending": return "待审核";
            case "approved": return "已公开";
            case "rejected": return "未通过";
            case "withdrawn": return "已撤回";
            case "hidden": return "已下架";
            default: return status;
        }
    }

    private static int dp(Activity activity, int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
