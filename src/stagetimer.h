#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QtGlobal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

// OMAEDIT_STAGE_TRACE=1: where a render's time goes. Each named stage adds
// its wall time; at exit the totals, counts and means go to stderr.
namespace StageTimer {
inline bool on() {
    static const bool enabled = qEnvironmentVariableIsSet("OMAEDIT_STAGE_TRACE");
    return enabled;
}
struct Totals {
    QMutex mutex;
    std::map<std::string, std::pair<qint64, qint64>> stages;  // name → (ns, count)
    ~Totals() {
        for (const auto &[name, total] : stages)
            std::fprintf(stderr, "stage %-28s %9.1f ms total  %6lld×  %8.2f ms mean\n", name.c_str(),
                         double(total.first) / 1e6, static_cast<long long>(total.second),
                         double(total.first) / 1e6 / double(std::max<qint64>(1, total.second)));
    }
};
inline Totals &totals() {
    static Totals t;
    return t;
}
inline void add(const char *stage, qint64 ns) {
    QMutexLocker lock(&totals().mutex);
    auto &slot = totals().stages[stage];
    slot.first += ns;
    ++slot.second;
}
// Forgets what has been added so far: a measurement begins after its warm-up.
inline void reset() {
    QMutexLocker lock(&totals().mutex);
    totals().stages.clear();
}
// What has been added so far (for a measurement that reads its own figures).
inline std::map<std::string, std::pair<qint64, qint64>> snapshot() {
    QMutexLocker lock(&totals().mutex);
    return totals().stages;
}
// The time from here to the end of the scope, as `stage`.
struct Scope {
    const char *stage;
    QElapsedTimer timer;
    explicit Scope(const char *name) : stage(name) { if (on()) timer.start(); }
    ~Scope() { if (timer.isValid()) add(stage, timer.nsecsElapsed()); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};
// Stages one after another: each next() ends the stage before.
struct Sequence {
    const char *stage = nullptr;
    QElapsedTimer timer;
    void next(const char *name) {
        finish();
        stage = name;
        if (on()) timer.start();
    }
    void finish() {
        if (stage && timer.isValid()) add(stage, timer.nsecsElapsed());
        stage = nullptr;
        timer.invalidate();
    }
    ~Sequence() { finish(); }
};
}  // namespace StageTimer
