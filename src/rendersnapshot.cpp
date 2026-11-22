#include "rendersnapshot.h"
#include <QDir>
#include <QJsonDocument>
#include <QSet>
#include "projectfile.h"
#include "projectsafety.h"
#include <QFileInfo>
#include <QDateTime>
#include <functional>

QVariantMap RenderSnapshot::filesNow(const QVariant &value) {
  QSet<QString> paths;
  std::function<void(const QVariant &)> collect = [&](const QVariant &v) {
    if (v.metaType().id() == QMetaType::QVariantMap) {
      const auto map = v.toMap();
      for (auto it = map.begin(); it != map.end(); ++it) {
        if (it.key() == "compoundJson")
          collect(QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant());
        else
          collect(it.value());
      }
    } else if (v.metaType().id() == QMetaType::QVariantList) {
      for (const auto &item : v.toList()) collect(item);
    } else if (v.metaType().id() == QMetaType::QString) {
      const auto path = v.toString();
      if (QDir::isAbsolutePath(path)) paths.insert(path);
    }
  };
  collect(value);
  QVariantMap facts;
  for (const auto &path : std::as_const(paths)) {
    const QFileInfo f(path);
    facts[path] = QVariantList{f.exists(), f.size(), f.lastModified().toMSecsSinceEpoch(), f.canonicalFilePath()};
  }
  return facts;
}

QVariantMap RenderSnapshot::nestedDocuments(const QVariantList &entries) {
  QVariantMap documents;
  QSet<QString> visited;
  std::function<void(const QVariantList &)> scan =
      [&](const QVariantList &items) {
        for (const auto &item : items) {
          const auto clip = item.toMap();
          if (clip.contains("compoundJson"))
            scan(QJsonDocument::fromJson(
                     clip.value("compoundJson").toString().toUtf8())
                     .toVariant()
                     .toList());
          const QString path = clip.value("path").toString();
          if (!ProjectFile::isProject(path) ||
              visited.contains(path) || visited.size() >= 256)
            continue;
          visited.insert(path);
          const QVariantMap doc = ProjectSafety::readDocument(path);
          if (!doc.isEmpty()) {
            documents[path] = doc;
            scan(doc.value("entries").toList());
          }
        }
      };
  scan(entries);
  return documents;
}

QVariantList RenderSnapshot::materialize(const QVariantList &entries,
                                         const QVariantMap &documents,
                                         const QString &directory,
                                         QString *error) {
  QMap<QString, QString> paths;
  for (auto it = documents.begin(); it != documents.end(); ++it)
    paths[it.key()] =
        QDir(directory).filePath(QString("nest-%1").arg(paths.size()) + ProjectFile::extension());
  std::function<QVariantList(const QVariantList &)> replace =
      [&](const QVariantList &items) {
        QVariantList out;
        for (const auto &v : items) {
          auto clip = v.toMap();
          const auto path = clip.value("path").toString();
          if (paths.contains(path))
            clip["path"] = paths.value(path);
          if (clip.contains("compoundJson"))
            clip["compoundJson"] = QString::fromUtf8(
                QJsonDocument::fromVariant(
                    replace(QJsonDocument::fromJson(
                                clip.value("compoundJson").toString().toUtf8())
                                .toVariant()
                                .toList()))
                    .toJson(QJsonDocument::Compact));
          out.append(clip);
        }
        return out;
      };
  for (auto it = documents.begin(); it != documents.end(); ++it) {
    auto doc = it.value().toMap();
    doc["entries"] = replace(doc.value("entries").toList());
    if (!ProjectSafety::writeDocument(paths.value(it.key()), doc)) {
      *error = "Could not save a queued nested sequence.";
      return {};
    }
  }
  return replace(entries);
}
