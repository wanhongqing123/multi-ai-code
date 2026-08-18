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
