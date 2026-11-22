#pragma once
#include <QVariantList>
#include <QStringList>
namespace MediaCsv {
QVariantList parse(const QByteArray &bytes, QStringList *headers, QString *error);
QByteArray encode(const QVariantList &records, const QStringList &headers);
}
