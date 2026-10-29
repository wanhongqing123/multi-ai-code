// 文件作用：仅在内存缓存当前登录的 STS 凭据，合并并发领取并拒绝旧登录的迟到结果。
// 获取和预取共用同一个 Future；执行器负责网络，缓存锁内不执行网络或完成外部回调。
package com.kongshang.maichat;

import java.io.IOException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.function.LongSupplier;

final class RealtimeAsrCredentialCache {
    interface Loader { RealtimeAsrCredential load() throws Exception; }
    private final Executor executor;
    private final LongSupplier clock;
    private String session = "";
    private RealtimeAsrCredential cached;
    private CompletableFuture<RealtimeAsrCredential> pending;

    RealtimeAsrCredentialCache(Executor executor, LongSupplier clock) {
        this.executor = executor; this.clock = clock;
    }

    void activate(String nextSession) {
        CompletableFuture<RealtimeAsrCredential> previous;
        synchronized (this) {
            session = nextSession;
            cached = null;
            previous = pending;
            pending = null;
        }
        if (previous != null) previous.completeExceptionally(changedSession());
    }

    CompletableFuture<RealtimeAsrCredential> get(String expectedSession, Loader loader) {
        CompletableFuture<RealtimeAsrCredential> request;
        synchronized (this) {
            if (session.isEmpty() || !session.equals(expectedSession))
                return failed(changedSession());
            if (cached != null && cached.expiresAt > clock.getAsLong() + 90)
                return CompletableFuture.completedFuture(cached);
            cached = null;
            if (pending != null) return pending;
            request = new CompletableFuture<>();
            pending = request;
        }
        try {
            executor.execute(() -> {
                try {
                    synchronized (this) {
                        if (!session.equals(expectedSession) || pending != request)
                            throw changedSession();
                    }
                    RealtimeAsrCredential issued = loader.load();
                    synchronized (this) {
                        if (!session.equals(expectedSession) || pending != request)
                            throw changedSession();
                        if (issued == null || issued.expiresAt <= clock.getAsLong() + 90)
                            throw new IOException("语音识别临时凭据已过期");
                        cached = issued;
                        pending = null;
                    }
                    request.complete(issued);
                } catch (Exception error) {
                    synchronized (this) { if (pending == request) pending = null; }
                    request.completeExceptionally(error);
                }
            });
        } catch (RuntimeException error) {
            synchronized (this) { if (pending == request) pending = null; }
            request.completeExceptionally(error);
        }
        return request;
    }

    private static IOException changedSession() { return new IOException("登录账号已变化，请重试语音输入"); }
    private static CompletableFuture<RealtimeAsrCredential> failed(Exception error) {
        CompletableFuture<RealtimeAsrCredential> result = new CompletableFuture<>();
        result.completeExceptionally(error);
        return result;
    }
}
