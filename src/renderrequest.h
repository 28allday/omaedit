#pragma once
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>


// One picture/audio request, or one export pass. Copies shared by layer jobs keep
// cancellation and the first rendering failure independent of fallback
// pictures and of the lifetime of the producer that reported them.
class RenderRequest {
public:
    using Cancel = std::function<bool()>;
    explicit RenderRequest(Cancel cancel = {}) : m_cancel(std::move(cancel)) {}
    void cancel() { m_cancelled.store(true, std::memory_order_relaxed); }
    bool cancelled() const {
        return m_cancelled.load(std::memory_order_relaxed) || (m_cancel && m_cancel());
    }
    void fail(const QString &error) {
        if (error.isEmpty() || cancelled()) return;
        std::lock_guard lock(m_mutex);
        if (m_error.isEmpty()) m_error = error;
    }
    QString error() const { std::lock_guard lock(m_mutex); return m_error; }
private:
    const Cancel m_cancel;
    std::atomic<bool> m_cancelled{false};
    mutable std::mutex m_mutex;
    QString m_error;
};

// Scope synchronous image/audio callbacks, and explicitly pass current() when
// launching layer jobs. Nested decoders participate in their caller's request.
class RenderScope {
public:
    explicit RenderScope(std::shared_ptr<RenderRequest> request = {}) : m_previous(s_current) {
        s_current = request ? std::move(request) : m_previous ? m_previous : std::make_shared<RenderRequest>();
    }
    ~RenderScope() { s_current = std::move(m_previous); }
    RenderScope(const RenderScope &) = delete;
    RenderScope &operator=(const RenderScope &) = delete;
    static std::shared_ptr<RenderRequest> current() { return s_current; }
    static bool cancelled() { return s_current && s_current->cancelled(); }
    static void fail(const QString &error) { if (s_current) s_current->fail(error); }
private:
    inline static thread_local std::shared_ptr<RenderRequest> s_current;
    std::shared_ptr<RenderRequest> m_previous;
};
