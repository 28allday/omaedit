#pragma once

// A solid clip's colour ("color:" + text) as the timeline has always read it
// (the generator's colour property): red, green, blue, black and white are the pure
// primaries (not the web colours of those names), 0xRRGGBBAA, #RRGGBB and
// #AARRGGBB; anything else as Qt reads it. Invalid when it reads as nothing.
#include <QColor>
#include <QString>

inline QColor solidColour(const QString &text) {
    const QString s = text.trimmed();
    if (s == QLatin1String("red")) return QColor(255, 0, 0);
    if (s == QLatin1String("green")) return QColor(0, 255, 0);
    if (s == QLatin1String("blue")) return QColor(0, 0, 255);
    if (s == QLatin1String("black")) return QColor(0, 0, 0);
    if (s == QLatin1String("white")) return QColor(255, 255, 255);
    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) && s.size() == 10) {
        bool ok = false;
        const uint v = s.mid(2).toUInt(&ok, 16);
        if (ok) return QColor(int(v >> 24 & 0xff), int(v >> 16 & 0xff), int(v >> 8 & 0xff), int(v & 0xff));
    }
    return QColor(s);  // #RRGGBB, #AARRGGBB and Qt's names
}
