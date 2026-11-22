// Private to the Backend sources: their includes and the helpers more than one
// of them uses (backendhelpers.cpp).
#ifndef BACKEND_P_H
#define BACKEND_P_H

#include "framesampling.h"
#include "creationnames.h"
#include "retimeedit.h"
#include "retimemap.h"
#include "speedrampedit.h"
#include "reframetofit.h"
#include "markeredit.h"
#include <QImageReader>
#include "backend.h"
#include "clipentry.h"
#include "audiotimeline.h"
#include "audiotimingedit.h"
#include "sourcetiming.h"
#include "videoeffects.h"
#include "editengine.h"
#include "render/transitionstyles.h"
#include "hardwareprobe.h"
#include "viewerprocessor.h"
#include "scopeprocessor.h"
#include "linearrenderer.h"
#include "timelinerenderer.h"
#include <QRegularExpression>
#include <QRunnable>
#include <QUuid>
#include <QCryptographicHash>
#include <QThread>
#include <QProcess>
#include "timeline.h"

#include <QFile>

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QStorageInfo>
#include <QSettings>
#include <QDesktopServices>
#include <QDirIterator>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include "storagepreferences.h"
#include "thumbnaildiskcache.h"
#include "interchange.h"
#include "projectfile.h"
#include "audioengine.h"
#include "playbackview.h"
#include "startup.h"


#include <QSet>
#include <algorithm>
#include <limits>
#include <cmath>

#include "exportspec.h"
#include "clipcategories.h"
#include "filepicker.h"
#include "playerworker.h"
#include "projectsafety.h"
#include "renderqueue.h"
#include "scopes.h"
#include <QAudioDevice>
#include <QJSValue>
#include <QMediaDevices>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include "thumbnailprovider.h"

bool nameNestedClip(QVariant &clip, const QVariantList &sequences);
QVariant plainVariant(const QVariant &v);

#endif // BACKEND_P_H
