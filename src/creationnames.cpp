#include "creationnames.h"
#include <QCoreApplication>
#include <cmath>
#include <limits>

namespace CreationNames {
QString problem(const QString &name, const QStringList &existing) {
    const QString clean=name.trimmed();
    if (clean.isEmpty()) return QCoreApplication::translate("CreationNames","Enter a name.");
    if (clean.size()>120) return QCoreApplication::translate("CreationNames","Keep the name within 120 characters.");
    for (const QChar c : clean)
        if (c.category()==QChar::Other_Control || c.category()==QChar::Separator_Line || c.category()==QChar::Separator_Paragraph)
            return QCoreApplication::translate("CreationNames","Use a single-line name without control characters.");
    for (const auto &taken : existing)
        if (clean.compare(taken.trimmed(),Qt::CaseInsensitive)==0)
            return QCoreApplication::translate("CreationNames","That name is already in use. Choose another name.");
    return {};
}
QString nextSequence(const QStringList &existing) {
    for (int n=existing.size()+1;;++n) {
        const auto candidate=QCoreApplication::translate("CreationNames","Timeline %1").arg(n);
        if (problem(candidate,existing).isEmpty()) return candidate;
    }
}
QString projectProblem(const QVariantMap &s) {
    const auto title=s.value("title").toString();
    if (const auto error=problem(title); !error.isEmpty()) return error;
    if (title.contains('/') || title.contains('\\') || title.trimmed()=="." || title.trimmed()=="..")
        return QCoreApplication::translate("CreationNames","Use a project name without path separators.");
    const auto integer=[&s](const char *key,int low,int high) {
        bool ok=false; const double value=s.value(QLatin1String(key)).toDouble(&ok);
        return ok && std::isfinite(value) && value>=low && value<=high && std::floor(value)==value;
    };
    if (!integer("width",16,8192) || !integer("height",16,8192) || s.value("width").toInt()%2 || s.value("height").toInt()%2)
        return QCoreApplication::translate("CreationNames","Use even frame dimensions between 16 and 8192 pixels.");
    if (!integer("fpsNum",1,1000000) || !integer("fpsDen",1,1000000)
        || s.value("fpsNum").toDouble()/s.value("fpsDen").toDouble()<1
        || s.value("fpsNum").toDouble()/s.value("fpsDen").toDouble()>240)
        return QCoreApplication::translate("CreationNames","Use a frame rate between 1 and 240 fps.");
    if (!integer("colorspace",601,2020) || !QList<int>{601,709,2020}.contains(s.value("colorspace").toInt()))
        return QCoreApplication::translate("CreationNames","Choose a supported project colour space.");
    if (!integer("sampleRate",44100,96000) || !QList<int>{44100,48000,96000}.contains(s.value("sampleRate").toInt())
        || !integer("channels",1,6) || !QList<int>{1,2,6}.contains(s.value("channels").toInt()))
        return QCoreApplication::translate("CreationNames","Choose a supported audio sample rate and channel layout.");
    if (!integer("startFrame",0,std::numeric_limits<int>::max()))
        return QCoreApplication::translate("CreationNames","Use a non-negative starting frame within the supported range.");
    // Optional: the whole frames a second the starting frame was counted at.
    if (s.contains("startRate") && !integer("startRate",0,1000))
        return QCoreApplication::translate("CreationNames","Use a whole starting rate between 0 and 1000, or leave it out.");
    // Timecode has no twenty-fourth hour. The rate the start is counted at is
    // known when it is written, or when the format is a chosen one; a start
    // waiting for its first clip's rate with none written is not judged here.
    const int counted=s.value("startRate").toInt()>0 ? s.value("startRate").toInt()
        : s.value("custom").toBool() ? qRound(s.value("fpsNum").toDouble()/s.value("fpsDen").toDouble()) : 0;
    if (counted>0 && s.value("startFrame").toLongLong()>=86400ll*counted)
        return QCoreApplication::translate("CreationNames","Use a starting timecode before 24:00:00:00.");
    if (s.value("custom").metaType().id()!=QMetaType::Bool)
        return QCoreApplication::translate("CreationNames","Choose automatic or custom project format.");
    return {};
}
}
