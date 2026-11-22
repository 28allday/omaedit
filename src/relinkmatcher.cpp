#include "relinkmatcher.h"
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <cmath>

namespace {
QString relativeInside(const QString &root, const QString &path) {
    if (!QDir::isAbsolutePath(root) || !QDir::isAbsolutePath(path)) return {};
    const auto relative = QDir(QDir::cleanPath(root)).relativeFilePath(QDir::cleanPath(path));
    return relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative) ? QString() : relative;
}
bool hasTimecode(const QVariantMap &source) {
    return source.contains("timecodeFrames") || !source.value("timecode").toString().trimmed().isEmpty();
}
}

QVariantMap RelinkMatcher::normalized(const QVariantMap &options) {
    QVariantMap out;
    for (const auto *key : {"matchFilename", "matchSize", "matchDuration"}) out[key] = options.value(key, true).toBool();
    for (const auto *key : {"matchRelative", "matchTimecode", "matchChecksum", "caseSensitive"}) out[key] = options.value(key, false).toBool();
    for (const auto *key : {"originalRoot", "replacementRoot"}) out[key] = options.value(key).toString().trimmed();
    return out;
}

QString RelinkMatcher::missingFacts(const QVariantMap &source, const QVariantMap &options) {
    const auto rules = normalized(options);
    const bool any = rules.value("matchFilename").toBool() || rules.value("matchRelative").toBool() ||
        rules.value("matchTimecode").toBool() || rules.value("matchChecksum").toBool() ||
        (rules.value("matchSize").toBool() && source.value("size").toLongLong() > 0) ||
        (rules.value("matchDuration").toBool() && source.value("duration").toInt() > 0);
    if (!any) return QObject::tr("Select a matching rule with recorded source information.");
    if (rules.value("matchRelative").toBool() &&
        (relativeInside(rules.value("originalRoot").toString(), source.value("path").toString()).isEmpty() ||
         !QDir::isAbsolutePath(rules.value("replacementRoot").toString())))
        return QObject::tr("Relative-path matching needs both base folders and a source inside the original base.");
    if (rules.value("matchTimecode").toBool() && !hasTimecode(source))
        return QObject::tr("No recorded source timecode; this rule cannot verify a replacement.");
    static const QRegularExpression sha256("^[0-9a-fA-F]{64}$");
    if (rules.value("matchChecksum").toBool() && !sha256.match(source.value("checksum").toString()).hasMatch())
        return QObject::tr("No recorded SHA256 checksum; this rule cannot verify a replacement.");
    return {};
}

bool RelinkMatcher::fileMatches(const QVariantMap &source, const QString &candidate, qint64 size,
                               const QVariantMap &options) {
    const auto rules = normalized(options);
    if (!missingFacts(source, rules).isEmpty()) return false;
    const auto sensitivity = rules.value("caseSensitive").toBool() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const auto path = source.value("path").toString();
    if (rules.value("matchFilename").toBool() && QFileInfo(path).fileName().compare(QFileInfo(candidate).fileName(), sensitivity)) return false;
    if (rules.value("matchRelative").toBool()) {
        const auto original = relativeInside(rules.value("originalRoot").toString(), path);
        const auto replacement = relativeInside(rules.value("replacementRoot").toString(), candidate);
        if (replacement.isEmpty() || original.compare(replacement, sensitivity)) return false;
    }
    const qint64 recordedSize = source.value("size").toLongLong();
    return !rules.value("matchSize").toBool() || recordedSize <= 0 || recordedSize == size;
}

bool RelinkMatcher::metadataMatches(const QVariantMap &source, const QVariantMap &candidate,
                                   const QVariantMap &options) {
    const auto rules = normalized(options);
    if (!missingFacts(source, rules).isEmpty()) return false;
    if (rules.value("matchDuration").toBool() && source.value("duration").toInt() > 0 &&
        source.value("duration").toInt() != candidate.value("duration").toInt()) return false;
    if (rules.value("matchTimecode").toBool()) {
        if (!hasTimecode(candidate)) return false;
        if (source.contains("timecodeFrames") && candidate.contains("timecodeFrames")) {
            if (source.value("timecodeFrames").toLongLong() != candidate.value("timecodeFrames").toLongLong()) return false;
        } else if (source.value("timecode").toString().trimmed() != candidate.value("timecode").toString().trimmed()) return false;
        const double fps = source.value("fps").toDouble(), other = candidate.value("fps").toDouble();
        if (fps > 0 && (other <= 0 || std::abs(fps - other) > .001)) return false;
    }
    return !rules.value("matchChecksum").toBool() ||
        source.value("checksum").toString().compare(candidate.value("checksum").toString(), Qt::CaseInsensitive) == 0;
}

QStringList RelinkMatcher::evidence(const QVariantMap &source, const QVariantMap &options) {
    const auto rules = normalized(options);
    QStringList fields;
    if (rules.value("matchFilename").toBool()) fields << QObject::tr("filename");
    if (rules.value("matchRelative").toBool()) fields << QObject::tr("relative path");
    if (rules.value("matchSize").toBool() && source.value("size").toLongLong() > 0) fields << QObject::tr("file size");
    if (rules.value("matchDuration").toBool() && source.value("duration").toInt() > 0) fields << QObject::tr("frame count");
    if (rules.value("matchTimecode").toBool()) fields << QObject::tr("timecode");
    if (rules.value("matchChecksum").toBool()) fields << QObject::tr("SHA256");
    return fields;
}
