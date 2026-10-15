// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMOrigin 负责远程 IM 消息来源标识，本文件给出其实现。
// 主要入口：wireValue、fromWireValue。
package com.kongshang.maichat;

public enum RemoteIMOrigin {
    HUMAN("human"),
    MACHINE("machine");

    private final String wireValue;

    RemoteIMOrigin(String wireValue) {
        this.wireValue = wireValue;
    }

    public String wireValue() {
        return wireValue;
    }

    public static RemoteIMOrigin fromWireValue(String value) {
        return HUMAN.wireValue.equals(value) ? HUMAN : MACHINE;
    }
}
