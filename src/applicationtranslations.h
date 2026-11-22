#pragma once
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

// English is the complete fallback catalogue. A matching catalogue may be
// embedded or installed beside the executable in translations/. Load before
// constructing the backend/QML so both C++ and QML start in the same language.
class ApplicationTranslations {
public:
    explicit ApplicationTranslations(QCoreApplication &app) {
        if (m_english.load(QStringLiteral(":/i18n/omaedit_en_GB.qm")))
            app.installTranslator(&m_english);
        const QString override = qEnvironmentVariable("OMAEDIT_LANGUAGE");
        const QLocale locale = override.isEmpty() ? QLocale::system() : QLocale(override);
        if (m_qt.load(locale, QStringLiteral("qt"), QStringLiteral("_"),
                      QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
            app.installTranslator(&m_qt);
        if (m_application.load(locale, QStringLiteral("omaedit"), QStringLiteral("_"),
                               QCoreApplication::applicationDirPath() + QStringLiteral("/translations"))
            || m_application.load(locale, QStringLiteral("omaedit"), QStringLiteral("_"),
                                  QStringLiteral(":/i18n")))
            app.installTranslator(&m_application);
    }
private:
    QTranslator m_english, m_qt, m_application;
};
