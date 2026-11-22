#pragma once
#include <QStringList>
#include <QVariantMap>

namespace CreationNames {
QString problem(const QString &name, const QStringList &existing = {});
QString nextSequence(const QStringList &existing);
QString projectProblem(const QVariantMap &settings);
}
