#pragma once
// Palette.h -- every colour and font the window uses, in one place.
//
// The window is meant to be screenshotted onto slides, so it keeps to the
// house slide rules: blue for emphasis, grey for structure, near-black for
// text -- plus one red that means "this run is failing" and nothing else.
// Pressure is a single blue ramp, light to dark, not the rainbow the terminal
// uses: a rainbow is not perceptually ordered, and on a slide it fights with
// everything around it.

#include <QColor>
#include <QFont>
#include <QStringList>
#include <QtGlobal>

#include <cmath>

namespace Pal {

inline QColor bg()         { return QColor(0xFF, 0xFF, 0xFF); }
inline QColor line()       { return QColor(0xDA, 0xDF, 0xE5); }
inline QColor grid()       { return QColor(0xEC, 0xEF, 0xF3); }
inline QColor text()       { return QColor(0x16, 0x1A, 0x1F); }
inline QColor muted()      { return QColor(0x6B, 0x74, 0x80); }
inline QColor accent()     { return QColor(0x1F, 0x5F, 0xA8); }
inline QColor accentSoft() { return QColor(0xE6, 0xEF, 0xF9); }
inline QColor fault()      { return QColor(0xC0, 0x39, 0x2B); }
inline QColor empty()      { return QColor(0xF1, 0xF3, 0xF6); }

// Light to dark blue for u in [0, 1]. Written so that a nan lands on 0.
inline QColor ramp(double u)
{
    if (!(u >= 0)) u = 0;
    if (u > 1) u = 1;
    struct Stop { double u; int r, g, b; };
    static const Stop s[3] = {{0.0, 0xEE, 0xF4, 0xFB},
                              {0.5, 0x86, 0xAD, 0xDB},
                              {1.0, 0x0B, 0x3A, 0x6F}};
    const Stop &a = u < 0.5 ? s[0] : s[1];
    const Stop &b = u < 0.5 ? s[1] : s[2];
    const double k = (u - a.u) / (b.u - a.u);
    auto mix = [k](int x, int y) { return int(std::lround(x + k * (y - x))); };
    return QColor(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
}

// White or near-black, whichever reads on this fill.
inline QColor onFill(const QColor &c)
{
    const double lum = 0.2126 * c.red() + 0.7152 * c.green() + 0.0722 * c.blue();
    return lum < 150 ? QColor(0xFF, 0xFF, 0xFF) : text();
}

// One family for everything. Nanum first because the lab machines have it;
// the rest are what a stock Ubuntu falls back to.
inline QFont font(int px, bool bold = false)
{
    QFont f;
#if QT_VERSION >= QT_VERSION_CHECK(5, 13, 0)
    f.setFamilies({QStringLiteral("NanumSquare"), QStringLiteral("NanumGothic"),
                   QStringLiteral("Noto Sans CJK KR"), QStringLiteral("Noto Sans KR"),
                   QStringLiteral("DejaVu Sans")});
#else
    f.setFamily(QStringLiteral("NanumSquare"));
#endif
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

// "#rrggbb", for style sheets.
inline QString css(const QColor &c) { return c.name(QColor::HexRgb); }

}  // namespace Pal
