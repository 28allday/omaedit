#pragma once
#include <QImage>
#include <QVariantMap>
#include <atomic>
#include <functional>
namespace ViewerProcessing {
// Decoded frames must retain immutable backing storage while queued. A still
// path is read on the worker; its gallery revision invalidates request reuse.
struct Request {QImage frame;QString file;quint64 fileRevision=0;QVariantMap options;int divisor=1;bool matte=false;};
struct Result {QImage frame;QSize nativeSize;double pixelAspect=1;QString error;double milliseconds=0;bool cacheHit=false;QVariantMap conversion;};
// Pure image processing: run on a processing thread, never on the UI thread.
using ColourTransform=std::function<QImage(const QImage &,const QVariantMap &,const std::atomic<bool> &,QVariantMap *,QString *)>;
Result render(const Request &request,const std::atomic<bool> &cancelled,const ColourTransform &transform={});
QByteArray key(const Request &request);
}
Q_DECLARE_METATYPE(ViewerProcessing::Result)
