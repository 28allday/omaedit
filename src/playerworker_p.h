// Private to the PlayerWorker sources: their includes and the helpers more than one
// of them uses (playerworkerhelpers.cpp).
#ifndef PLAYERWORKER_P_H
#define PLAYERWORKER_P_H

#include "framesampling.h"
#include "projectfile.h"
#include "keyer.h"
#include "mixer.h"
#include "stabilise.h"
#include "mediapool.h"
#include "thumbnaildiskcache.h"
#include "creationnames.h"
#include "retimeedit.h"
#include "markeredit.h"
#include "playerworker.h"
#include "clipentry.h"
#include "clipcategories.h"
#include <QUuid>
#include <functional>
#include "projectsafety.h"
#include "keptmedia.h"
#include <QSettings>
#include "colourpipeline.h"
#include "gradeengine.h"
#include "linearrenderer.h"
#include "timelinerenderer.h"
#include "mediarefs.h"
#include "timelinerate.h"
#include "sourcetiming.h"
#include "rendersnapshot.h"
#include "previewrendercache.h"
#include <QTemporaryDir>
#include <QScopeGuard>

#include <cmath>
#include <cstdio>
#include <limits>
#include <QSaveFile>
#include <QTemporaryFile>
#include <vector>

#include <QCoreApplication>
#include <QEvent>
#include <QDateTime>
#include <QTimer>
#include <chrono>
#include <QDir>
#include <QCryptographicHash>
#include <QStandardPaths>
#include "storagepreferences.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIODevice>
#include <QtMath>

#include <algorithm>
#include <numeric>
#include <QUuid>
#include <utility>
#include <QRegularExpression>


#include "mediaplayer.h"
#include "exportspec.h"
#include "renderrequest.h"
#include "audiotimeline.h"

#include <csignal>
#include <sys/types.h>
#include <unistd.h>

void signalExportGroup(QProcess *process, int sig);

inline constexpr int kFrequency = 48000;
inline constexpr int kChannels = 2;
bool completeSamplingFile(const QString &path);
bool publishSamplingFile(const QString &stage, const QString &path);
QVariantMap sampledEntry(QVariantMap entry, const QString &path, const QVariantMap &profile);
int entryEnd(const QVariantList &entries, int index);
QString historyLabel(const QList<QVariantList> &stack);

#endif // PLAYERWORKER_P_H
