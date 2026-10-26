package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;

public final class RemoteIMGroupCreationCoordinatorTest {
    private static final class FakeSDK implements RemoteIMGroupCreationCoordinator.Gateway {
        int calls;
        String name;
        List<String> members;
        TencentIMClient.GroupCreationCompletion pending;

        @Override public void create(String name,List<String> members,
                                      TencentIMClient.GroupCreationCompletion completion) {
            calls++;
            this.name = name;
            this.members = new ArrayList<>(members);
            pending = completion;
        }
    }

    @Test public void rejectsInvalidOrUnacceptedMembersBeforeCallingSdk() {
        FakeSDK sdk = new FakeSDK();
        RemoteIMGroupCreationCoordinator coordinator = new RemoteIMGroupCreationCoordinator(sdk);
        AtomicReference<String> error = new AtomicReference<>();
        RemoteIMGroupCreationCoordinator.Completion completion =
            (group,activated,created,description) -> error.set(description);
        coordinator.create("",List.of("friend"),Set.of("friend"),"owner",true,List.of(),completion);
        assertNotNull(error.get());
        coordinator.create("hello",List.of("stranger"),Set.of("friend"),"owner",true,List.of(),completion);
        assertNotNull(error.get());
        coordinator.create("hello",List.of("owner"),Set.of("owner"),"owner",true,List.of(),completion);
        assertNotNull(error.get());
        coordinator.create("hello",List.of("friend","friend"),Set.of("friend"),"owner",true,
            List.of(),completion);
        assertNotNull(error.get());
        coordinator.create("hello",List.of("friend"),Set.of("friend"),"owner",false,List.of(),completion);
        assertEquals("IM 尚未连接",error.get());
        coordinator.create("abcdefghijklmnopqrstuvwxyzabcde",List.of("friend"),Set.of("friend"),
            "owner",true,List.of(),completion);
        assertNotNull(error.get());
        assertEquals(0,sdk.calls);
    }

    @Test public void fakeSdkProvesSingleCreateAndPartialActivationRecovery() {
        FakeSDK sdk = new FakeSDK();
        RemoteIMGroupCreationCoordinator coordinator = new RemoteIMGroupCreationCoordinator(sdk);
        AtomicReference<RemoteIMGroup> result = new AtomicReference<>();
        AtomicReference<Boolean> created = new AtomicReference<>();
        AtomicReference<Boolean> activated = new AtomicReference<>();
        RemoteIMGroupCreationCoordinator.Completion completion = (group,active,newlyCreated,error) -> {
            result.set(group); activated.set(active); created.set(newlyCreated);
        };
        coordinator.create("  QA group  ",List.of("friend"),Set.of("friend"),"owner",true,
            List.of(),completion);
        assertEquals(1,sdk.calls);
        assertEquals("QA group",sdk.name);
        assertEquals(List.of("friend"),sdk.members);
        coordinator.create("another",List.of("friend"),Set.of("friend"),"owner",true,
            List.of(),completion);
        assertEquals(1,sdk.calls);
        assertNull(result.get());
        assertFalse(created.get());

        sdk.pending.onCreated(new RemoteIMGroup("sdk-group","QA group",2),false,
            "activation delivery failed");
        assertNotNull(result.get());
        assertTrue(result.get().pendingActivation());
        assertFalse(activated.get());
        assertTrue(created.get());
        coordinator.create("another",List.of("friend"),Set.of("friend"),"owner",true,
            List.of(result.get()),completion);
        assertEquals(1,sdk.calls);
        assertEquals("sdk-group",result.get().groupId());
        assertFalse(created.get());
        assertTrue(result.get().pendingActivation());
    }

    @Test public void accountResetIgnoresOldSdkCallbackAndAllowsNewCreate() {
        FakeSDK sdk = new FakeSDK();
        RemoteIMGroupCreationCoordinator coordinator = new RemoteIMGroupCreationCoordinator(sdk);
        AtomicReference<RemoteIMGroup> result = new AtomicReference<>();
        RemoteIMGroupCreationCoordinator.Completion completion = (group,active,created,error) ->
            result.set(group);
        coordinator.create("first",List.of("friend"),Set.of("friend"),"owner",true,
            List.of(),completion);
        TencentIMClient.GroupCreationCompletion stale = sdk.pending;
        coordinator.reset();
        assertNull(result.get());
        stale.onCreated(new RemoteIMGroup("stale","first",2),true,"");
        assertNull(result.get());
        coordinator.create("second",List.of("friend"),Set.of("friend"),"owner",true,
            List.of(),completion);
        assertEquals(2,sdk.calls);
        sdk.pending.onCreated(new RemoteIMGroup("fresh","second",2),true,"");
        assertEquals("fresh",result.get().groupId());
        assertFalse(result.get().pendingActivation());
    }
}
