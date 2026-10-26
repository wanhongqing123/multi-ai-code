// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMTab 负责 IM 主页面签的选中状态，本文件给出其实现。
// 主要入口：title。
package com.kongshang.maichat;

public enum RemoteIMTab {
    MESSAGES("消息"),
    CLOUD("云盘"),
    ASSISTANT("创作中心"),
    CONTACTS("好友列表"),
    REMOTE("远程"),
    ME("我的");

    private final String title;

    RemoteIMTab(String title) {
        this.title = title;
    }

    public String title() {
        return title;
    }
}
