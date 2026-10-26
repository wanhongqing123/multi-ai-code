package com.kongshang.maichat;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Keeps one Work-group create request in flight and preserves partial-create recovery. */
final class RemoteIMGroupCreationCoordinator {
    interface Gateway {
        void create(String name, List<String> memberIds,
                    TencentIMClient.GroupCreationCompletion completion);
    }

    interface Completion {
        void onFinished(RemoteIMGroup group, boolean activated,
                        boolean newlyCreated, String error);
    }

    private final Gateway gateway;
    private int generation;
    private boolean creating;
    private Completion inFlight;

    RemoteIMGroupCreationCoordinator(Gateway gateway) { this.gateway = gateway; }

    void reset() {
        generation++; creating = false;
        Completion old = inFlight;
        inFlight = null;
        if (old != null) old.onFinished(null,false,false,"账号已切换");
    }

    void create(String name, List<String> memberIds, Set<String> acceptedFriends,
                String ownerId, boolean connected, Collection<RemoteIMGroup> existing,
                Completion completion) {
        String cleanedName = name == null ? "" : name.trim();
        if (!connected) {
            completion.onFinished(null,false,false,"IM 尚未连接"); return;
        }
        for (RemoteIMGroup group : existing) if (group.pendingActivation()) {
            completion.onFinished(group,false,false,
                "已有创建成功但尚未通知成员的群，请先打开该群并发送消息");
            return;
        }
        if (creating) {
            completion.onFinished(null,false,false,"群聊正在创建，请稍候"); return;
        }
        if (cleanedName.isEmpty() || cleanedName.codePointCount(0,cleanedName.length()) > 30
            || cleanedName.getBytes(StandardCharsets.UTF_8).length > 100
            || memberIds == null || memberIds.isEmpty() || memberIds.size() > 19
            || new HashSet<>(memberIds).size() != memberIds.size()
            || !acceptedFriends.containsAll(memberIds) || memberIds.contains(ownerId)) {
            completion.onFinished(null,false,false,
                "群名称需在 100 字节内，且须选择 1 至 19 位好友");
            return;
        }
        List<String> members = new ArrayList<>(memberIds);
        creating = true;
        inFlight = completion;
        int request = ++generation;
        gateway.create(cleanedName,members,new TencentIMClient.GroupCreationCompletion() {
            @Override public void onCreated(RemoteIMGroup group,boolean activated,
                                             String activationError) {
                if (request != generation) return;
                creating = false;
                inFlight = null;
                completion.onFinished(activated ? group : group.withPendingActivation(true),
                    activated,true,activated ? "" : activationError);
            }
            @Override public void onError(int code,String message) {
                if (request != generation) return;
                creating = false;
                inFlight = null;
                completion.onFinished(null,false,false,message);
            }
        });
    }
}
