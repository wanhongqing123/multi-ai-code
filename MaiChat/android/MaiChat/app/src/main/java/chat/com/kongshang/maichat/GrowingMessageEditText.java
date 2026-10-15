// 文件作用：Android 聊天会话的消息呈现与输入；GrowingMessageEditText 负责文本内容的整理与展示，本文件给出其实现。
// 类型关系：GrowingMessageEditText extends android.widget.EditText；审阅时可据此追踪父类、接口或回调契约。
// 主要入口：configure。
package com.kongshang.maichat;

import android.content.Context;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.inputmethod.EditorInfo;

public final class GrowingMessageEditText extends android.widget.EditText {
    public GrowingMessageEditText(Context context) {
        super(context);
        configure();
    }

    public GrowingMessageEditText(Context context, AttributeSet attrs) {
        super(context, attrs);
        configure();
    }

    private void configure() {
        setSingleLine(false);
        setMinLines(1);
        setMaxLines(5);
        setGravity(android.view.Gravity.TOP | android.view.Gravity.START);
        setRawInputType(
            InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
        );
        setImeOptions(EditorInfo.IME_ACTION_SEND | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        setVerticalScrollBarEnabled(true);
        setOverScrollMode(OVER_SCROLL_IF_CONTENT_SCROLLS);
        setBackgroundColor(android.graphics.Color.TRANSPARENT);
        setPadding(
            MaiChatTheme.dp(getContext(), 13),
            MaiChatTheme.dp(getContext(), 11),
            MaiChatTheme.dp(getContext(), 13),
            MaiChatTheme.dp(getContext(), 9)
        );
    }
}
