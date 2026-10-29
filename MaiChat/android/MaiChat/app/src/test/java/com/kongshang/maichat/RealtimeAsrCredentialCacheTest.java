// 文件作用：验证登录预取异步排队、并发合并、90 秒刷新及账号/同账号重登隔离。
// 手动执行器与固定时钟让测试无需网络、线程等待或真实凭据。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.io.IOException;
import java.util.ArrayDeque;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import org.junit.Test;

public final class RealtimeAsrCredentialCacheTest {
    private final ArrayDeque<Runnable> queued = new ArrayDeque<>();
    private final Executor executor = queued::add;
    private final AtomicLong now = new AtomicLong(RealtimeAsrTicketTest.NOW);
    private final RealtimeAsrCredentialCache cache = new RealtimeAsrCredentialCache(executor, now::get);

    private RealtimeAsrCredential issued() throws Exception {
        return RealtimeAsrCredential.parse(RealtimeAsrTicketTest.fixture(now.get()), now.get());
    }

    @Test public void loginPrefetchReturnsBeforeNetworkAndConcurrentRequestsShareOneLoad() throws Exception {
        cache.activate("account-a:1");
        AtomicInteger loads = new AtomicInteger();
        CompletableFuture<RealtimeAsrCredential> first = cache.get("account-a:1", () -> {
            loads.incrementAndGet(); return issued();
        });
        assertFalse(first.isDone());
        assertEquals(0, loads.get());
        assertSame(first, cache.get("account-a:1", this::issued));
        assertEquals(1, queued.size());
        queued.remove().run();
        assertEquals(1, loads.get());
        assertSame(first.get(1, TimeUnit.SECONDS), cache.get("account-a:1", this::issued).get());
        assertTrue(queued.isEmpty());
    }

    @Test public void credentialsRefreshAtTheNinetySecondBoundary() throws Exception {
        cache.activate("account-a:1");
        CompletableFuture<RealtimeAsrCredential> first = cache.get("account-a:1", this::issued);
        queued.remove().run();
        now.addAndGet(1709);
        assertSame(first.get(), cache.get("account-a:1", this::issued).get());
        now.incrementAndGet();
        CompletableFuture<RealtimeAsrCredential> refreshed = cache.get("account-a:1", this::issued);
        assertFalse(refreshed.isDone());
        queued.remove().run();
        assertNotSame(first.get(), refreshed.get());
    }

    @Test public void accountChangeAndSameAccountReloginRejectLateResults() throws Exception {
        cache.activate("account-a:1");
        CompletableFuture<RealtimeAsrCredential> old = cache.get("account-a:1", () -> {
            cache.activate("account-b:2");
            return issued();
        });
        queued.remove().run();
        assertFailed(old);
        CompletableFuture<RealtimeAsrCredential> current = cache.get("account-b:2", this::issued);
        queued.remove().run();
        assertNotNull(current.get());
        cache.activate("account-b:3");
        assertFailed(cache.get("account-b:2", this::issued));
        CompletableFuture<RealtimeAsrCredential> relogin = cache.get("account-b:3", this::issued);
        assertFalse(relogin.isDone());
        queued.remove().run();
        assertNotSame(current.get(), relogin.get());
    }

    @Test public void logoutCancelsQueuedPrefetchAndCannotStartAnotherRequest() throws Exception {
        cache.activate("account-a:1");
        AtomicInteger loads = new AtomicInteger();
        CompletableFuture<RealtimeAsrCredential> old = cache.get("account-a:1", () -> {
            loads.incrementAndGet(); return issued();
        });
        cache.activate("");
        queued.remove().run();
        assertEquals(0, loads.get());
        assertFailed(old);
        assertFailed(cache.get("account-a:1", this::issued));
        assertTrue(queued.isEmpty());
    }

    @Test public void failedPrefetchCanRetryWithoutReturningStaleCredentials() throws Exception {
        cache.activate("account-a:1");
        CompletableFuture<RealtimeAsrCredential> failed = cache.get("account-a:1", () -> {
            throw new IOException("fixture network failure");
        });
        queued.remove().run();
        assertFailed(failed);
        CompletableFuture<RealtimeAsrCredential> retry = cache.get("account-a:1", this::issued);
        queued.remove().run();
        assertNotNull(retry.get());
    }

    private void assertFailed(CompletableFuture<RealtimeAsrCredential> request) throws Exception {
        try { request.get(1, TimeUnit.SECONDS); fail("Stale or failed request succeeded"); }
        catch (ExecutionException expected) { }
    }
}
