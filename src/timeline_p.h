// Private to the Timeline sources: their includes and the helpers more than one
// of them uses (timeline_helpers.cpp).
#ifndef TIMELINE_P_H
#define TIMELINE_P_H

#include "render/effectsteps.h"
#include "render/transitionstyles.h"
#include "animatedwebp.h"
#include "stagetimer.h"
#include <QScopeGuard>
#include <future>
#include "projectfile.h"
#include "clipentry.h"
#include "audiotimeline.h"
#include "audioclipboard.h"
#include "audiotimingedit.h"
#include "audiosamplesource.h"
#include "retimeedit.h"
#include "retimemap.h"
#include "speedrampedit.h"
#include "sourcetiming.h"
#include "multicamtiming.h"
#include "timelinerate.h"
#include "timeline.h"
#include "framesampling.h"
#include "bands.h"
#include "editengine.h"
#include "linearrenderer.h"
#include "engine.h"
#include "timelinerenderer.h"
#include "engines.h"
#include "mixer.h"
#include "peaklimit.h"
#include "loudness.h"
#include "sink.h"
#include "renderrequest.h"
#include "colourpipeline.h"
#include "projectsafety.h"
#include "mediarefs.h"
#include "lutfilter.h"
#include "gradeengine.h"
#include <utility>
#include <QCoreApplication>
#include <limits>
#include <QScopedValueRollback>
#include <QCryptographicHash>
#include <QTemporaryFile>
#include <QDateTime>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include "storagepreferences.h"
#include "mediaprobe.h"
#include <QMutex>

#include <QVariantMap>
#include <QSaveFile>
#include <QtMath>

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/parseutils.h>
}

#include <cmath>
#include <numeric>
#include <functional>
#include <vector>

#include "graph.h"
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <QSet>
#include "videoeffects.h"
#include <QUuid>

QList<QByteArray> bypassTags(const QString &key);
QString bypassKeyOf(edit::Filter &f);
bool laneOverwrite(edit::Playlist *pl, int position, edit::Producer &cut);
void profileFromProbe(edit::Profile &profile, const media::Probe &probe);
int attributeRank(edit::Filter &f);
void reorderAttributeFilters(edit::Producer *cut);
void fadeLengths(edit::Producer *cut, int in, int out, int *fadeIn, int *fadeOut);
bool connectedRect(edit::Producer *cut, double *x, double *y, double *w,
                   double *h, double *rotation = nullptr, double *anchorX = nullptr,
                   double *anchorY = nullptr);
void holdRetimedTail(edit::Profile &profile, edit::Producer &producer, double rate);
void attachRecord(edit::Service &target, const char *service, const QVariantMap &config, const char *marker);
std::unique_ptr<edit::Producer> boundedCut(edit::Producer &producer,
                                                 int in, int out);
void rebaseEntryKeys(QVariantMap &entry, int moved);
// The clocks of an entry's effects that run as it plays, when its in point
// moves by `moved` frames: each counts from the clip's first frame, and its
// clock makes up the difference, so every frame the clip still shows is the
// picture it was (timeline_mixes.cpp).
void rebaseEntryClocks(QVariantMap &entry, int moved);
bool moveEntryStart(QVariantMap &entry, int start, const edit::Profile &profile);
void splitKeys(const QVariantList &keys, const char *valueName, int relative,
                      QVariantList *left, QVariantList *right);
bool applyVolumeKf(edit::Profile &profile, edit::Producer *cut,
                          const QVariantList &kf, int cutIn, int cutOut);
QVariantList volumeKfOf(edit::Producer *cut);
bool applyOpacityKf(edit::Profile &profile, edit::Producer *cut,
                           const QVariantList &kf, int cutIn, int cutOut);
QVariantList opacityKfOf(edit::Producer *cut);
bool applyOpacityValue(edit::Profile &profile, edit::Producer *cut,
                              double alpha);
bool applyVolumeDb(edit::Profile &profile, edit::Producer *cut,
                          double db);
bool applyPanValue(edit::Profile &profile, edit::Producer *cut,
                          double pan);
bool applyEqPreset(edit::Profile &profile, edit::Producer *cut,
                          const QString &preset);

struct MulticamPoint {
    bool valid = false;
    double seconds = 0.;
    bool backwards = false;
    double source = 0.;
    double offsetSeconds = 0.;
};
QVariantMap multicamDefinition(const QVariantMap &entry);
// Two clips read their source from the same origin: neither has one, or both
// have the same (two parts of a multicam segment put on an angle one after
// the other have theirs worked out apart, and differ in the last bit).
bool sameSourceOrigin(const QVariantMap &a, const QVariantMap &b);
bool multicamParentTiming(const QVariantMap &entry,const QVariantMap &camera,double fps,RetimeMap *timing);
MulticamPoint multicamPoint(const QVariantMap &entry, const QVariantMap &camera,
                            int relativeFrame, double fps);
QVariantMap heldMulticamTiming(const QVariantMap &entry,const QVariantMap &camera,double fps,
                              int outputFrame,int frames);

// Counts one settling of the timeline by putting back only what changed, or by
// rebuilding it whole (Timeline::narrowConforms, wholeConforms).
void noteSettle(bool narrow);

#endif // TIMELINE_P_H
