package com.kongshang.maichat;

public enum RemoteIMApprovalAction {
    APPROVE_ONCE("approve-once", "同意本次"),
    APPROVE_PREFIX("approve-prefix", "同意并记住"),
    REJECT("reject", "拒绝"),
    RESOLVED("resolved", "审批已处理"),
    AUTO_DECLINED("auto-declined", "审批已自动拒绝");

    private final String wireValue;
    private final String title;

    RemoteIMApprovalAction(String wireValue, String title) {
        this.wireValue = wireValue;
        this.title = title;
    }

    public String wireValue() {
        return wireValue;
    }

    public String title() {
        return title;
    }

    public String decisionDisplayText() {
        return "审批操作：" + title;
    }

    public static RemoteIMApprovalAction fromWireValue(String value) {
        if (value == null) return null;
        for (RemoteIMApprovalAction action : values()) {
            if (action.wireValue.equals(value.trim())) return action;
        }
        return null;
    }
}
