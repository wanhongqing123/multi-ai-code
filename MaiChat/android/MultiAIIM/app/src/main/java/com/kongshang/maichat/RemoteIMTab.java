package com.kongshang.maichat;

public enum RemoteIMTab {
    MESSAGES("消息"),
    CONTACTS("通讯录"),
    ME("我");

    private final String title;

    RemoteIMTab(String title) {
        this.title = title;
    }

    public String title() {
        return title;
    }
}
