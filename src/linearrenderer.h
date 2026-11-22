#pragma once
#include "workingframe.h"
#include <functional>
#include <memory>

class LinearRenderer {
public:
    LinearRenderer();
    ~LinearRenderer();
    static bool enabled(const QVariantList &entries, const QVariantMap &project = {}, int depth = 0);
    void configure(const QVariantList &entries, const QVariantList &tracks, const QVariantMap &project,
                   const QSize &size, double fps);
    QString validate() const;
    WorkingFrame render(int frame, bool before, QString *error);
    QImage matte(int frame, QString *error);
    // Where the output's frames come from instead of this renderer's own CPU
    // path (the render engine, when it draws the timeline); empty: its own.
    using FrameSource = std::function<WorkingFrame(int frame, QString *error)>;
    void setFrameSource(FrameSource source);
    static QString outputSpace(const QString &delivery,const QString &toneMap={});
    // The delivery's picture at `frame`: 16-bit RGBA in
    // `space`, at `size` when given. Null with `error` set.
    QImage deliver(int frame, const QSize &size, const QString &space, QString *error = nullptr);
    // The same frame before its output transform: scene-linear working light,
    // framed and flattened as the delivery asks (an OpenEXR sequence's file).
    WorkingFrame deliverLinear(int frame, const QSize &size, QString *error = nullptr);
private:
    struct State;
    std::shared_ptr<State> d;
};
