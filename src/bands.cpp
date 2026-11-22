#include "bands.h"
#include "engine/parallel.h"
#include <QThreadPool>
#include <atomic>

namespace Bands {
QThreadPool *pool() {
    // Only its size is read (how many helpers a caller may count on): the
    // work itself runs on the process's one pool, engine::RowPool, so this
    // one never starts a thread. Never destroyed, as before.
    static QThreadPool *threads = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(qMax(1, engine::RowPool::instance().helpers()));
        return p;
    }();
    return threads;
}
bool run(int count, const std::function<bool(int)> &task) {
    if (count <= 0)
        return true;
    // Each piece is a "row" of the shared pool: the caller takes pieces too,
    // several callers are served at once, and a call from inside a piece
    // runs in place (no threads on top of threads).
    std::atomic<bool> failed{false};
    engine::RowPool::instance().run(0, count, 1, [&](int first, int last) {
        for (int i = first; i < last; ++i)
            if (!task(i)) failed.store(true, std::memory_order_relaxed);
    });
    return !failed.load();
}
} // namespace Bands
