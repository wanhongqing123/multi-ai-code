#include "MaiBlockingCheck.h"

#include <cstdio>
#include <cstdlib>

#include "MaiThread.h"

namespace {

// 每个线程一份。默认允许——绝大多数线程本来就是拿来干活的，
// 只有明确声明"我现在不许慢"的那一小段才收紧。
thread_local bool tBlockingAllowed = true;

}  // namespace

bool maiIsBlockingAllowed() {
    return tBlockingAllowed;
}

void maiAssertBlockingAllowed(const char* what) {
    if (tBlockingAllowed) return;

    // 这里只能用 stderr：核心库不带日志设施，而且这条信息必须在进程
    // 死掉之前出来。
    const std::string name = MaiThread::currentName();
    std::fprintf(stderr,
                 "\n[MaiAgent] blocking call inside a no-blocking scope: %s\n"
                 "  thread: %s\n"
                 "  This usually means slow work inside a MaiEventBus handler.\n"
                 "  Handlers run synchronously on whichever thread published the event;\n"
                 "  while streaming that is the network read thread, so reading a file or\n"
                 "  writing to the store there directly slows the model down.\n"
                 "  Queue the work and do it on another thread instead (that is what the\n"
                 "  SSE code in the HTTP adapter does). If it really must happen here,\n"
                 "  say so explicitly with MaiScopedAllowBlocking.\n\n",
                 what ? what : "(unnamed)", name.empty() ? "(unnamed)" : name.c_str());
    std::fflush(stderr);
    std::abort();
}

MaiScopedDisallowBlocking::MaiScopedDisallowBlocking() : mPrevious(tBlockingAllowed) {
    tBlockingAllowed = false;
}

MaiScopedDisallowBlocking::~MaiScopedDisallowBlocking() {
    // 恢复成进来之前的状态，不是无条件放开——嵌套时外层的限制要留着。
    tBlockingAllowed = mPrevious;
}

MaiScopedAllowBlocking::MaiScopedAllowBlocking() : mPrevious(tBlockingAllowed) {
    tBlockingAllowed = true;
}

MaiScopedAllowBlocking::~MaiScopedAllowBlocking() {
    tBlockingAllowed = mPrevious;
}
