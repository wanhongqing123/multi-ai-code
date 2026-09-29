#include "MaiGraphicsTaskRunner.h"

#include <stdexcept>
#include <utility>

#include "MaiThread.h"

MaiGraphicsTaskRunner::MaiGraphicsTaskRunner() : mWorker([this] { run(); }) {
    std::unique_lock<std::mutex> lock(mMutex);
    mReady.wait(lock, [this] { return mWorkerId != std::thread::id(); });
}

MaiGraphicsTaskRunner::~MaiGraphicsTaskRunner() {
    stop();
}

std::future<void> MaiGraphicsTaskRunner::postTask(std::function<void()> task) {
    std::packaged_task<void()> packaged(std::move(task));
    std::future<void> result = packaged.get_future();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStopping) {
            std::promise<void> rejected;
            result = rejected.get_future();
            rejected.set_exception(
                std::make_exception_ptr(std::runtime_error("graphics task runner stopped")));
            return result;
        }
        mTasks.push_back(std::move(packaged));
    }
    mHasTask.notify_one();
    return result;
}

bool MaiGraphicsTaskRunner::isCurrentThread() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mWorkerId == std::this_thread::get_id();
}

void MaiGraphicsTaskRunner::stop() {
    if (isCurrentThread()) {
        // Joining ourselves would deadlock. Destruction from a posted task is a caller error.
        std::terminate();
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStopping = true;
    }
    mHasTask.notify_one();
    if (mWorker.joinable()) mWorker.join();
}

void MaiGraphicsTaskRunner::run() {
    MaiThread::setCurrentName("mai-graphics");
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mWorkerId = std::this_thread::get_id();
    }
    mReady.notify_one();

    for (;;) {
        std::packaged_task<void()> task;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mHasTask.wait(lock, [this] { return mStopping || !mTasks.empty(); });
            if (mTasks.empty()) break;
            task = std::move(mTasks.front());
            mTasks.pop_front();
        }
        task();
    }
}
