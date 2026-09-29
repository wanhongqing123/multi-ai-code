#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

// Fixed-thread task runner for one graphics device. This has SingleThreadTaskRunner semantics:
// accepted tasks run in posting order on one physical thread for the runner's entire lifetime.
//
// postTask is thread-safe. The returned future carries any task exception. stop drains accepted
// tasks before joining, rejects new tasks, and must be called from outside the graphics thread.
// The owner must stop/destroy the runner only after producers have stopped posting. GPU resources
// should be destroyed in a posted task before stop returns; no task runs after stop returns.
class MaiGraphicsTaskRunner {
public:
    MaiGraphicsTaskRunner();
    ~MaiGraphicsTaskRunner();

    MaiGraphicsTaskRunner(const MaiGraphicsTaskRunner&) = delete;
    MaiGraphicsTaskRunner& operator=(const MaiGraphicsTaskRunner&) = delete;

    std::future<void> postTask(std::function<void()> task);
    bool isCurrentThread() const;
    void stop();

private:
    void run();

    mutable std::mutex mMutex;
    std::condition_variable mReady;
    std::condition_variable mHasTask;
    std::deque<std::packaged_task<void()>> mTasks;
    std::thread::id mWorkerId;
    bool mStopping = false;
    std::thread mWorker;
};
