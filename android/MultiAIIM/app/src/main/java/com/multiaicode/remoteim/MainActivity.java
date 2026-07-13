package com.multiaicode.remoteim;

import android.Manifest;
import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaRecorder;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;

public final class MainActivity extends Activity {
    private static final int BLUE = 0xFF2563EB;
    private static final int GREEN = 0xFF16A34A;
    private static final int PANEL = 0xFFF8FAFC;
    private static final int BORDER = 0xFFD9E4EF;
    private static final int TEXT_PRIMARY = 0xFF0F172A;
    private static final int TEXT_SECONDARY = 0xFF64748B;
    private static final int REQUEST_PICK_IMAGE = 1001;
    private static final int REQUEST_IMAGE_PERMISSION = 1002;
    private static final int REQUEST_RECORD_AUDIO = 1003;
    private static final String[][] CONTROL_COMMANDS = new String[][]{
        {"/status", "查看状态"},
        {"/plan", "切换 Plan"},
        {"/build", "切换 Build"},
        {"/models", "模型列表"},
        {"/model ", "切换模型"},
        {"/help", "命令帮助"}
    };

    private RemoteIMSessionController session;
    private RemoteIMMediaStore mediaStore;
    private RemoteIMTab activeTab = RemoteIMTab.MESSAGES;
    private LinearLayout root;
    private LinearLayout content;
    private EditText messageInput;
    private MediaRecorder recorder;
    private File recordingFile;
    private long recordingStartedAtMillis;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        session = new RemoteIMSessionController(
            new LocalSettingsStore(new File(getFilesDir(), "remote-im-settings/settings.properties")),
            new LocalChatHistoryStore(new File(getFilesDir(), "chat-history"))
        );
        mediaStore = new RemoteIMMediaStore(getCacheDir());
        render();
    }

    @Override
    protected void onPause() {
        super.onPause();
        saveState();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_PICK_IMAGE && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) sendPickedImage(uri);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
        if (requestCode == REQUEST_IMAGE_PERMISSION && granted) {
            openImagePicker();
        } else if (requestCode == REQUEST_RECORD_AUDIO && granted) {
            toggleVoiceRecording();
        } else if (!granted) {
            Toast.makeText(this, "需要权限后才能继续", Toast.LENGTH_SHORT).show();
        }
    }

    private void saveState() {
        try {
            session.saveChatState();
        } catch (IOException err) {
            Toast.makeText(this, "本地历史保存失败", Toast.LENGTH_SHORT).show();
        }
    }

    private ChatState chatState() {
        return session.chatState();
    }

    private void render() {
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFFFFFFFF);
        setContentView(root);

        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        root.addView(content, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));
        if (session.requiresLogin()) {
            renderLogin();
        } else {
            root.addView(bottomTabs(), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(68)
            ));
            renderActiveTab();
        }
    }

    private void renderLogin() {
        content.removeAllViews();
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(28), dp(42), dp(28), dp(28));

        TextView title = new TextView(this);
        title.setText("远程 IM 登录");
        title.setTextSize(28);
        title.setTextColor(TEXT_PRIMARY);
        panel.addView(title, matchWrap());

        TextView subtitle = smallText("登录后进入消息、通讯录和设置。");
        subtitle.setPadding(0, dp(6), 0, dp(20));
        panel.addView(subtitle, matchWrap());

        TextView label = smallText("登录账号");
        label.setTextColor(TEXT_PRIMARY);
        panel.addView(label, matchWrap());

        EditText userIdInput = new EditText(this);
        userIdInput.setSingleLine(true);
        userIdInput.setHint("输入 IM 账号 ID");
        panel.addView(userIdInput, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(52)
        ));

        LinearLayout summary = new LinearLayout(this);
        summary.setOrientation(LinearLayout.VERTICAL);
        summary.setPadding(dp(12), dp(12), dp(12), dp(12));
        summary.setBackgroundColor(PANEL);
        summary.addView(bodyText("基础 IM 配置"));
        summary.addView(smallText("通信配置：内置"));
        summary.addView(smallText("连接凭证：内置"));
        LinearLayout.LayoutParams summaryParams = matchWrap();
        summaryParams.setMargins(0, dp(14), 0, dp(18));
        panel.addView(summary, summaryParams);

        Button login = new Button(this);
        login.setText("登录并进入");
        login.setAllCaps(false);
        login.setTextColor(0xFFFFFFFF);
        login.setBackgroundColor(BLUE);
        login.setOnClickListener(view -> {
            String loginUserId = userIdInput.getText().toString().trim();
            if (loginUserId.isEmpty()) {
                Toast.makeText(this, "请输入登录账号", Toast.LENGTH_SHORT).show();
                return;
            }
            try {
                session.login(loginUserId);
                activeTab = RemoteIMTab.MESSAGES;
                render();
            } catch (IOException err) {
                Toast.makeText(this, "登录设置保存失败", Toast.LENGTH_SHORT).show();
            }
        });
        panel.addView(login, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(52)
        ));

        content.addView(panel, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));
    }

    private void renderActiveTab() {
        content.removeAllViews();
        switch (activeTab) {
            case CONTACTS:
                renderContacts();
                break;
            case ME:
                renderMe();
                break;
            case MESSAGES:
            default:
                renderMessages();
                break;
        }
    }

    private View bottomTabs() {
        LinearLayout tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        tabs.setGravity(Gravity.CENTER);
        tabs.setPadding(dp(12), dp(8), dp(12), dp(8));
        tabs.setBackgroundColor(0xFFFFFFFF);
        tabs.addView(tabButton(RemoteIMTab.MESSAGES), weightParams());
        tabs.addView(tabButton(RemoteIMTab.CONTACTS), weightParams());
        tabs.addView(tabButton(RemoteIMTab.ME), weightParams());
        return tabs;
    }

    private Button tabButton(RemoteIMTab tab) {
        Button button = new Button(this);
        button.setText(tab.title());
        button.setAllCaps(false);
        button.setTextColor(tab == activeTab ? BLUE : TEXT_SECONDARY);
        button.setBackgroundColor(0x00000000);
        button.setOnClickListener(view -> {
            activeTab = tab;
            render();
        });
        return button;
    }

    private void renderMessages() {
        content.addView(header("消息", "远程 IM 会话"), matchWrap());
        content.addView(contactSelector(), matchWrap());

        ScrollView scrollView = new ScrollView(this);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(14), dp(12), dp(14), dp(12));
        scrollView.addView(list);
        String peerId = chatState().selectedPeerId();
        if (peerId == null) {
            list.addView(emptyText("先在通讯录添加一个联系人。"));
        } else {
            for (RemoteIMMessage message : chatState().messagesWith(peerId)) {
                list.addView(messageBubble(message), matchWrap());
            }
        }
        content.addView(scrollView, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));

        content.addView(composer(), matchWrap());
        scrollView.post(() -> scrollView.fullScroll(View.FOCUS_DOWN));
    }

    private View contactSelector() {
        HorizontalScrollView scroller = new HorizontalScrollView(this);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(dp(14), dp(4), dp(14), dp(8));
        for (RemoteIMContact contact : chatState().contacts()) {
            Button button = new Button(this);
            button.setText(contact.displayName());
            button.setAllCaps(false);
            button.setTextColor(contact.userId().equals(chatState().selectedPeerId()) ? 0xFFFFFFFF : TEXT_PRIMARY);
            button.setBackgroundColor(contact.userId().equals(chatState().selectedPeerId()) ? BLUE : PANEL);
            button.setOnClickListener(view -> {
                chatState().selectPeer(contact.userId());
                render();
            });
            row.addView(button, new LinearLayout.LayoutParams(dp(128), dp(44)));
        }
        scroller.addView(row);
        return scroller;
    }

    private View composer() {
        LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.VERTICAL);
        wrapper.setBackgroundColor(0xFFFFFFFF);

        HorizontalScrollView commandScroller = new HorizontalScrollView(this);
        commandScroller.setHorizontalScrollBarEnabled(false);
        commandScroller.setVisibility(View.GONE);

        LinearLayout commandRow = new LinearLayout(this);
        commandRow.setOrientation(LinearLayout.HORIZONTAL);
        commandRow.setGravity(Gravity.CENTER_VERTICAL);
        commandRow.setPadding(dp(12), dp(8), dp(12), 0);
        commandScroller.addView(commandRow);
        wrapper.addView(commandScroller, matchWrap());

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(12), dp(8), dp(12), dp(12));
        bar.setBackgroundColor(0xFFFFFFFF);

        Button voice = new Button(this);
        voice.setText(recorder == null ? "语音" : "停止");
        voice.setAllCaps(false);
        voice.setOnClickListener(view -> {
            if (hasAudioPermission()) {
                toggleVoiceRecording();
            } else {
                requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQUEST_RECORD_AUDIO);
            }
        });
        bar.addView(voice, new LinearLayout.LayoutParams(dp(78), dp(52)));

        messageInput = new EditText(this);
        messageInput.setSingleLine(true);
        messageInput.setHint("输入消息...");
        messageInput.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                updateCommandSuggestions(commandScroller, commandRow, messageInput);
            }
        });
        bar.addView(messageInput, new LinearLayout.LayoutParams(0, dp(52), 1));

        ImageButton image = new ImageButton(this);
        image.setImageResource(android.R.drawable.ic_input_add);
        image.setBackgroundColor(0x00000000);
        image.setContentDescription("发送图片");
        image.setOnClickListener(view -> {
            if (hasImagePermission()) {
                openImagePicker();
            } else {
                requestPermissions(new String[]{imagePermission()}, REQUEST_IMAGE_PERMISSION);
            }
        });
        bar.addView(image, new LinearLayout.LayoutParams(dp(52), dp(52)));

        Button send = new Button(this);
        send.setText("发送");
        send.setAllCaps(false);
        send.setOnClickListener(view -> sendText());
        bar.addView(send, new LinearLayout.LayoutParams(dp(72), dp(52)));
        wrapper.addView(bar, matchWrap());
        return wrapper;
    }

    private void updateCommandSuggestions(
        HorizontalScrollView scroller,
        LinearLayout row,
        EditText input
    ) {
        row.removeAllViews();
        String query = input.getText().toString().trim();
        if (!query.startsWith("/")) {
            scroller.setVisibility(View.GONE);
            return;
        }

        int count = 0;
        for (String[] command : CONTROL_COMMANDS) {
            if (!command[0].startsWith(query)) continue;
            Button button = new Button(this);
            button.setAllCaps(false);
            button.setText(command[0] + "  " + command[1]);
            button.setTextColor(TEXT_PRIMARY);
            button.setBackgroundColor(0xFFEFF6FF);
            button.setOnClickListener(view -> {
                input.setText(command[0]);
                input.setSelection(input.getText().length());
            });
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(132), dp(38));
            params.setMargins(0, 0, dp(8), 0);
            row.addView(button, params);
            count++;
        }
        scroller.setVisibility(count > 0 ? View.VISIBLE : View.GONE);
    }

    private View messageBubble(RemoteIMMessage message) {
        LinearLayout outer = new LinearLayout(this);
        outer.setGravity(message.direction() == RemoteIMMessage.Direction.OUTGOING ? Gravity.END : Gravity.START);
        outer.setPadding(0, dp(5), 0, dp(5));

        LinearLayout bubble = new LinearLayout(this);
        bubble.setOrientation(LinearLayout.VERTICAL);
        bubble.setPadding(dp(12), dp(10), dp(12), dp(10));
        bubble.setBackgroundColor(message.direction() == RemoteIMMessage.Direction.OUTGOING ? 0xFFFFFFFF : 0xFFFFFBEB);

        TextView meta = new TextView(this);
        meta.setText(
            message.fromUserId()
                + " · "
                + RemoteIMTimestampFormatter.format(message.createdAtMillis())
                + " · "
                + statusText(message.status())
        );
        meta.setTextColor(TEXT_SECONDARY);
        meta.setTextSize(12);
        bubble.addView(meta);

        if (message.imageAttachment() != null) {
            bubble.addView(imagePreview(message.imageAttachment()));
        } else if (message.voiceAttachment() != null) {
            TextView voice = bodyText("▶ " + message.text());
            bubble.addView(voice);
        } else {
            bubble.addView(bodyText(message.text()));
        }

        outer.addView(bubble, new LinearLayout.LayoutParams(dp(280), ViewGroup.LayoutParams.WRAP_CONTENT));
        return outer;
    }

    private View imagePreview(RemoteIMImageAttachment attachment) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        Bitmap bitmap = BitmapFactory.decodeFile(attachment.localPath());
        if (bitmap != null) {
            ImageView imageView = new ImageView(this);
            imageView.setImageBitmap(bitmap);
            imageView.setScaleType(ImageView.ScaleType.CENTER_CROP);
            imageView.setBackgroundColor(PANEL);
            imageView.setOnClickListener(view -> showFullScreenImage(attachment.localPath()));
            box.addView(imageView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(180)
            ));
        } else {
            box.addView(bodyText("图片暂不可预览"));
        }
        TextView name = smallText(new File(attachment.localPath()).getName());
        box.addView(name);
        return box;
    }

    private void showFullScreenImage(String path) {
        Bitmap bitmap = BitmapFactory.decodeFile(path);
        if (bitmap == null) {
            Toast.makeText(this, "图片暂不可预览", Toast.LENGTH_SHORT).show();
            return;
        }

        Dialog dialog = new Dialog(this, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(0xFF000000);
        ImageView imageView = new ImageView(this);
        imageView.setImageBitmap(bitmap);
        imageView.setAdjustViewBounds(true);
        imageView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        frame.addView(imageView, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT,
            Gravity.CENTER
        ));
        TextView close = new TextView(this);
        close.setText("关闭");
        close.setTextColor(0xFFFFFFFF);
        close.setTextSize(16);
        close.setPadding(dp(18), dp(14), dp(18), dp(14));
        close.setOnClickListener(view -> dialog.dismiss());
        frame.addView(close, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.END
        ));
        frame.setOnClickListener(view -> dialog.dismiss());
        dialog.setContentView(frame);
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        }
    }

    private void renderContacts() {
        content.addView(header("通讯录", "联系人只需要账号 ID"), matchWrap());
        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(dp(16), dp(10), dp(16), dp(10));
        EditText input = new EditText(this);
        input.setHint("账号 ID");
        form.addView(input, matchWrap());
        Button add = new Button(this);
        add.setText("添加联系人");
        add.setAllCaps(false);
        add.setOnClickListener(view -> {
            String userId = input.getText().toString().trim();
            if (!userId.isEmpty()) {
                chatState().upsertContact(new RemoteIMContact(userId, userId));
                chatState().selectPeer(userId);
                saveState();
                activeTab = RemoteIMTab.MESSAGES;
                render();
            }
        });
        form.addView(add, matchWrap());
        content.addView(form, matchWrap());

        ScrollView scroll = new ScrollView(this);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(16), dp(8), dp(16), dp(16));
        for (RemoteIMContact contact : chatState().contacts()) {
            TextView row = bodyText(contact.displayName() + "\n" + contact.userId());
            row.setPadding(dp(12), dp(12), dp(12), dp(12));
            row.setOnClickListener(view -> {
                chatState().selectPeer(contact.userId());
                activeTab = RemoteIMTab.MESSAGES;
                render();
            });
            list.addView(row, matchWrap());
        }
        scroll.addView(list);
        content.addView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1
        ));
    }

    private void renderMe() {
        content.addView(header("我", "本机 IM 账号"), matchWrap());
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(16), dp(16), dp(16), dp(16));
        EditText owner = new EditText(this);
        owner.setHint("登录账号");
        owner.setSingleLine(true);
        owner.setText(session.settings().loginUserId());
        panel.addView(owner, matchWrap());
        Button save = new Button(this);
        save.setText("保存账号");
        save.setAllCaps(false);
        save.setOnClickListener(view -> {
            String userId = owner.getText().toString().trim();
            if (!userId.isEmpty()) {
                try {
                    session.login(userId);
                    render();
                } catch (IOException err) {
                    Toast.makeText(this, "登录设置保存失败", Toast.LENGTH_SHORT).show();
                }
            }
        });
        panel.addView(save, matchWrap());

        TextView connection = smallText("状态：已登录");
        connection.setPadding(0, dp(18), 0, dp(10));
        panel.addView(connection);

        Button logout = new Button(this);
        logout.setText("退出登录");
        logout.setAllCaps(false);
        logout.setTextColor(0xFFFFFFFF);
        logout.setBackgroundColor(0xFFDC2626);
        logout.setOnClickListener(view -> {
            try {
                session.logout();
                activeTab = RemoteIMTab.MESSAGES;
                render();
            } catch (IOException err) {
                Toast.makeText(this, "退出登录失败", Toast.LENGTH_SHORT).show();
            }
        });
        panel.addView(logout, matchWrap());
        content.addView(panel, matchWrap());
    }

    private void sendText() {
        if (chatState().selectedPeerId() == null) {
            Toast.makeText(this, "请先选择联系人", Toast.LENGTH_SHORT).show();
            return;
        }
        String text = messageInput.getText().toString().trim();
        if (text.isEmpty()) return;
        try {
            session.sendTextMessage(text);
            messageInput.setText("");
            render();
        } catch (IOException err) {
            Toast.makeText(this, "文本消息发送失败", Toast.LENGTH_SHORT).show();
        }
    }

    private void sendPickedImage(Uri uri) {
        if (chatState().selectedPeerId() == null) {
            Toast.makeText(this, "请先选择联系人", Toast.LENGTH_SHORT).show();
            return;
        }
        try {
            File file = copyPickedImage(uri);
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(file.getAbsolutePath(), options);
            session.sendImageMessage(
                file.getAbsolutePath(),
                Math.max(0, options.outWidth),
                Math.max(0, options.outHeight),
                file.length()
            );
            render();
        } catch (IOException err) {
            Toast.makeText(this, "图片读取失败", Toast.LENGTH_SHORT).show();
        }
    }

    private File copyPickedImage(Uri uri) throws IOException {
        InputStream input = getContentResolver().openInputStream(uri);
        return mediaStore.copyPickedImage(input, uri.getLastPathSegment());
    }

    private void openImagePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/*");
        startActivityForResult(intent, REQUEST_PICK_IMAGE);
    }

    private void toggleVoiceRecording() {
        if (recorder == null) {
            startRecording();
        } else {
            stopRecording();
        }
    }

    private void startRecording() {
        if (chatState().selectedPeerId() == null) {
            Toast.makeText(this, "请先选择联系人", Toast.LENGTH_SHORT).show();
            return;
        }
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
            render();
        } catch (IOException | RuntimeException err) {
            recorder = null;
            Toast.makeText(this, "录音启动失败", Toast.LENGTH_SHORT).show();
        }
    }

    private void stopRecording() {
        try {
            recorder.stop();
            int duration = Math.max(1, (int) ((System.currentTimeMillis() - recordingStartedAtMillis) / 1000));
            session.sendVoiceMessage(recordingFile.getAbsolutePath(), duration);
        } catch (RuntimeException err) {
            Toast.makeText(this, "录音保存失败", Toast.LENGTH_SHORT).show();
        } catch (IOException err) {
            Toast.makeText(this, "语音消息发送失败", Toast.LENGTH_SHORT).show();
        } finally {
            recorder.release();
            recorder = null;
            recordingFile = null;
            render();
        }
    }

    private boolean hasAudioPermission() {
        return checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean hasImagePermission() {
        return checkSelfPermission(imagePermission()) == PackageManager.PERMISSION_GRANTED;
    }

    private String imagePermission() {
        return RemoteIMPermissionPolicy.imageReadPermission(Build.VERSION.SDK_INT);
    }

    private View header(String title, String subtitle) {
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setPadding(dp(18), dp(18), dp(18), dp(12));
        TextView titleView = new TextView(this);
        titleView.setText(title);
        titleView.setTextSize(26);
        titleView.setTextColor(TEXT_PRIMARY);
        titleView.setGravity(Gravity.START);
        header.addView(titleView);
        TextView subtitleView = smallText(subtitle);
        header.addView(subtitleView);
        return header;
    }

    private TextView bodyText(String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(TEXT_PRIMARY);
        view.setTextSize(15);
        view.setLineSpacing(0, 1.15f);
        return view;
    }

    private TextView smallText(String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(TEXT_SECONDARY);
        view.setTextSize(13);
        return view;
    }

    private TextView emptyText(String text) {
        TextView view = smallText(text);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(24), dp(80), dp(24), dp(80));
        return view;
    }

    private String statusText(RemoteIMMessage.Status status) {
        switch (status) {
            case PENDING:
                return "发送中";
            case SENT:
                return "已发送";
            case RECEIVED:
                return "已收到";
            case FAILED:
            default:
                return "发送失败";
        }
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
    }

    private LinearLayout.LayoutParams weightParams() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1);
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}
