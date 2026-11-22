#include "mediacsv.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSet>
#include <QVariantMap>

QVariantList MediaCsv::parse(const QByteArray &bytes, QStringList *headers, QString *error) {
    error->clear(); headers->clear();
    if (bytes.size() > 64 * 1024 * 1024) { *error = QCoreApplication::translate("MediaCsv", "CSV exceeds 64 MB."); return {}; }
    QString text = QString::fromUtf8(bytes);
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    QList<QStringList> rows; QStringList row; QString field;
    bool quoted = false, closed = false, touched = false;
    auto endField = [&] { row.append(field); field.clear(); closed = false; touched = false; };
    auto endRow = [&] { endField(); if (row.size() != 1 || !row.first().isEmpty()) rows.append(row); row.clear(); };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i+1 < text.size() && text.at(i+1) == QLatin1Char('"')) { field += c; ++i; }
                else { quoted = false; closed = true; }
            } else field += c;
        } else if (c == QLatin1Char(',')) endField();
        else if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            if (c == QLatin1Char('\r') && i+1 < text.size() && text.at(i+1) == QLatin1Char('\n')) ++i;
            endRow();
        } else if (c == QLatin1Char('"') && !touched && !closed && field.isEmpty()) { quoted = true; touched = true; }
        else if (closed || c == QLatin1Char('"')) { *error = QStringLiteral("Malformed quoted CSV field."); return {}; }
        else { field += c; touched = true; }
        if (rows.size() > 100000) { *error = QCoreApplication::translate("MediaCsv", "CSV exceeds 100,000 records."); return {}; }
    }
    if (quoted) { *error = QCoreApplication::translate("MediaCsv", "Unclosed quoted CSV field."); return {}; }
    if (touched || closed || !row.isEmpty()) endRow();
    if (rows.isEmpty()) { *error = QCoreApplication::translate("MediaCsv", "CSV has no header row."); return {}; }
    *headers = rows.takeFirst(); QSet<QString> names;
    for (QString &name : *headers) {
        name = name.trimmed();
        if (name.isEmpty() || names.contains(name)) { *error = QCoreApplication::translate("MediaCsv", "CSV headers must be nonempty and unique."); return {}; }
        names.insert(name);
    }
    if (!names.contains(QStringLiteral("libraryId")) && !names.contains(QStringLiteral("path"))) { *error = QCoreApplication::translate("MediaCsv", "CSV needs a libraryId or path column to match media."); return {}; }
    QVariantList records;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i).size() != headers->size()) { *error = QCoreApplication::translate("MediaCsv", "CSV record %1 has a different number of fields from the header.").arg(i+1); return {}; }
        QVariantMap record;
        for (int j = 0; j < headers->size(); ++j) record.insert(headers->at(j), rows.at(i).at(j));
        records.append(record);
    }
    return records;
}
QByteArray MediaCsv::encode(const QVariantList &records, const QStringList &headers) {
    auto cell = [](QString value) { value.replace('"', "\"\""); return QLatin1Char('"') + value + QLatin1Char('"'); };
    QStringList header;
    for (const QString &key : headers) header.append(cell(key));
    QByteArray output = header.join(',').toUtf8() + "\r\n";
    for (const QVariant &v : records) {
        const QVariantMap record = v.toMap(); QStringList row;
        for (const QString &key : headers) {
            const QVariant value = record.value(key);
            const bool structured = value.metaType().id() == QMetaType::QVariantList || value.metaType().id() == QMetaType::QStringList || value.metaType().id() == QMetaType::QVariantMap;
            row.append(cell(structured ? QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact)) : value.toString()));
        }
        output += row.join(',').toUtf8() + "\r\n";
    }
    return output;
}
