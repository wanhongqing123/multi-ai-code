// 文件作用：Android 联系人和群组管理；ContactGroups 负责联系人分组的数据和编辑流程，本文件给出其实现。
// 主要入口：normalize、isAcceptableName。
package com.kongshang.maichat;

public final class ContactGroups {
    private ContactGroups() {}

    public static String normalize(String value) {
        return value == null ? "" : value.trim();
    }

    public static boolean isAcceptableName(String normalized) {
        return normalized != null && !normalized.isEmpty();
    }
}
