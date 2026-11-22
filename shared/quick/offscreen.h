#pragma once
#include <QImage>
#include <QVariantMap>
#include <memory>
class QQuickWindow;
namespace SuiteQuick {
// Headless Qt Quick render target, used by isolated startup probes and tests.
class Offscreen {
public:
    explicit Offscreen(const QSize &size);
    ~Offscreen();
    bool initialize(QString *error);
    QQuickWindow *window() const;
    QImage render(QString *error);
    QVariantMap information() const;
private:
    struct Data;std::unique_ptr<Data> d;
};
}
