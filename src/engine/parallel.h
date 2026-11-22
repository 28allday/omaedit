#pragma once

// Rows of a CPU operation spread over the machine's cores. One pool for the
// process (the CPU twin, the copies beside the GPU engine, and Bands::run all
// use it), started on first use and sized from the machine: one helper for
// every hardware thread but the caller's. A call blocks until every row is
// done. Work items must not depend on one another (each writes its own rows),
// so the result is the same however the rows are shared out.
//
// Several callers at once (the viewer's worker, an export, the background
// renderers) are all served: each call is a job of its own on the pool's
// list, its caller works on it too, and idle helpers take blocks from
// whichever job has some left. A call made from inside a block runs in place,
// so nested parallel work never adds threads.
//
// Threads: `m_jobs` and `m_turn` under `m_mutex`. A Job's `next` is atomic;
// its `done` under its own mutex. The job is shared (shared_ptr) so a helper
// that looks at it after its caller has left touches only the counters; `fn`
// lives on the caller's stack and is called only for a block claimed while
// the caller is still waiting for it.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace engine {

class RowPool {
public:
    static RowPool &instance() {
        // Never destroyed: a caller on another thread may still be inside a
        // call, and helpers parked, when the process ends.
        static RowPool *pool = new RowPool;
        return *pool;
    }
    // Threads that help a caller (the caller itself is one more).
    int helpers() const { return int(m_threads.size()); }
    // fn(first, last) over [begin, end) in blocks of at least `grain` rows.
    void run(int begin, int end, int grain, const std::function<void(int, int)> &fn) {
        if (end <= begin) return;
        const int workers = int(m_threads.size()) + 1;
        const int rows = end - begin;
        if (workers == 1 || rows <= grain) { fn(begin, end); return; }
        // A call from inside a block (nested parallel work) runs in place.
        if (t_inPool) { fn(begin, end); return; }
        const int wanted = std::min(rows / std::max(1, grain), workers * 4);
        auto job = std::make_shared<Job>();
        job->fn = &fn;
        job->begin = begin;
        job->end = end;
        job->per = (rows + wanted - 1) / wanted;
        job->blocks = (rows + job->per - 1) / job->per;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_jobs.push_back(job);
        }
        // As many helpers as there are blocks beyond the caller's own.
        const int wake = std::min(job->blocks - 1, int(m_threads.size()));
        if (wake >= int(m_threads.size())) m_wake.notify_all();
        else for (int i = 0; i < wake; ++i) m_wake.notify_one();
        t_inPool = true;
        work(*job);
        t_inPool = false;
        {
            // Every block is claimed: nothing more for a helper to find here.
            std::lock_guard<std::mutex> l(m_mutex);
            m_jobs.erase(std::find(m_jobs.begin(), m_jobs.end(), job));
        }
        std::unique_lock<std::mutex> l(job->mutex);
        job->finished.wait(l, [&] { return job->done == job->blocks; });
    }

private:
    struct Job {
        const std::function<void(int, int)> *fn = nullptr;
        int begin = 0, end = 0, per = 1, blocks = 0;
        std::atomic<int> next{0};
        std::mutex mutex;
        std::condition_variable finished;
        int done = 0;
    };
    RowPool() {
        const unsigned n = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned i = 1; i < n; ++i)
            m_threads.emplace_back([this] { loop(); });
    }
    void loop() {
        t_inPool = true;
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> l(m_mutex);
                m_wake.wait(l, [&] { return bool(job = open()); });
            }
            work(*job);
        }
    }
    // A job with a block still to hand out, turn about so that no caller is
    // left to work alone while another has every helper. Under m_mutex.
    std::shared_ptr<Job> open() {
        const size_t count = m_jobs.size();
        for (size_t i = 0; i < count; ++i) {
            const auto &job = m_jobs[(m_turn + i) % count];
            if (job->next.load(std::memory_order_relaxed) < job->blocks) {
                m_turn = (m_turn + i + 1) % count;
                return job;
            }
        }
        return nullptr;
    }
    static void work(Job &job) {
        for (int block; (block = job.next.fetch_add(1)) < job.blocks;) {
            const int first = job.begin + block * job.per;
            (*job.fn)(first, std::min(job.end, first + job.per));
            std::lock_guard<std::mutex> l(job.mutex);
            if (++job.done == job.blocks) job.finished.notify_all();
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<std::shared_ptr<Job>> m_jobs;
    size_t m_turn = 0;
    static inline thread_local bool t_inPool = false;
};

// for each row block [first, last) of [begin, end)
inline void parallelRows(int begin, int end, const std::function<void(int, int)> &fn, int grain = 8) {
    RowPool::instance().run(begin, end, grain, fn);
}

}  // namespace engine
